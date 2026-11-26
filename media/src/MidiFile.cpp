// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/MidiFile.h>

#include <daw/core/Files.h>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace daw {

namespace {

void putVlq(std::string& out, std::uint32_t value)
{
    char bytes[5];
    int n = 0;
    bytes[n++] = static_cast<char>(value & 0x7F);
    while ((value >>= 7) != 0)
        bytes[n++] = static_cast<char>(0x80 | (value & 0x7F));
    while (n > 0)
        out.push_back(bytes[--n]);
}

void putBig(std::string& out, std::uint32_t value, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

struct Event {
    std::int64_t tick;
    int order; // at the same tick: meta first, then note-offs, then note-ons
    std::string bytes;
};

std::string trackChunk(std::vector<Event> events)
{
    std::stable_sort(events.begin(), events.end(),
                     [](const Event& a, const Event& b) { return std::tie(a.tick, a.order) < std::tie(b.tick, b.order); });
    std::string body;
    std::int64_t last = 0;
    for (const Event& e : events) {
        putVlq(body, static_cast<std::uint32_t>(std::max<std::int64_t>(0, e.tick - last)));
        body += e.bytes;
        last = std::max(last, e.tick);
    }
    body += std::string("\x00\xFF\x2F\x00", 4); // end of track
    std::string chunk = "MTrk";
    putBig(chunk, static_cast<std::uint32_t>(body.size()), 4);
    return chunk + body;
}

std::string metaText(int type, const std::string& text)
{
    std::string bytes{'\xFF', static_cast<char>(type)};
    putVlq(bytes, static_cast<std::uint32_t>(text.size()));
    return bytes + text;
}

} // namespace

std::string encodeMidiFile(const MidiSong& song)
{
    std::vector<Event> conductor;
    if (!song.name.empty())
        conductor.push_back({0, 0, metaText(0x03, song.name)});
    for (const MidiTimeSignature& t : song.timeSignatures) {
        int power = 0;
        while ((1 << power) < t.denominator && power < 7)
            ++power;
        std::string bytes{'\xFF', '\x58', '\x04', static_cast<char>(std::clamp(t.numerator, 1, 255)), static_cast<char>(power),
                          '\x18', '\x08'};
        conductor.push_back({t.tick, 1, bytes});
    }
    for (const MidiTempoEvent& t : song.tempo) {
        const auto micros = static_cast<std::uint32_t>(std::clamp(std::lround(60'000'000.0 / std::max(1.0, t.bpm)), 1L, 0xFFFFFFL));
        std::string bytes{'\xFF', '\x51', '\x03'};
        putBig(bytes, micros, 3);
        conductor.push_back({t.tick, 1, bytes});
    }

    std::string file = "MThd";
    putBig(file, 6, 4);
    putBig(file, 1, 2);
    putBig(file, static_cast<std::uint32_t>(1 + song.tracks.size()), 2);
    putBig(file, static_cast<std::uint32_t>(std::clamp(song.ticksPerQuarter, 1, 0x7FFF)), 2);
    file += trackChunk(std::move(conductor));
    for (const MidiTrackData& track : song.tracks) {
        std::vector<Event> events;
        if (!track.name.empty())
            events.push_back({0, 0, metaText(0x03, track.name)});
        const char channel = static_cast<char>(track.channel & 0x0F);
        for (const MidiNoteEvent& n : track.notes) {
            const char key = static_cast<char>(std::clamp(n.key, 0, 127));
            events.push_back({n.tick, 2, std::string{static_cast<char>(0x90 | channel), key, static_cast<char>(std::clamp(n.velocity, 1, 127))}});
            events.push_back({n.tick + std::max(1, n.lengthTicks), 1, std::string{static_cast<char>(0x80 | channel), key, '\x40'}});
        }
        file += trackChunk(std::move(events));
    }
    return file;
}

Status writeMidiFile(const std::filesystem::path& path, const MidiSong& song)
{
    return writeFileAtomically(path, encodeMidiFile(song));
}

namespace {

class Reader {
public:
    Reader(std::string_view bytes, std::size_t begin, std::size_t end) : bytes_(bytes), pos_(begin), end_(end) {}
    bool atEnd() const noexcept { return pos_ >= end_; }
    bool ok() const noexcept { return ok_; }
    std::uint8_t byte()
    {
        if (pos_ >= end_) {
            ok_ = false;
            return 0;
        }
        return static_cast<std::uint8_t>(bytes_[pos_++]);
    }
    std::uint8_t peek() const { return pos_ < end_ ? static_cast<std::uint8_t>(bytes_[pos_]) : 0; }
    std::uint32_t vlq()
    {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t b = byte();
            value = (value << 7) | (b & 0x7F);
            if ((b & 0x80) == 0)
                return value;
        }
        ok_ = false;
        return 0;
    }
    std::string_view take(std::uint32_t n)
    {
        if (n > end_ - pos_) {
            ok_ = false;
            pos_ = end_;
            return {};
        }
        const std::string_view out = bytes_.substr(pos_, n);
        pos_ += n;
        return out;
    }

private:
    std::string_view bytes_;
    std::size_t pos_, end_;
    bool ok_ = true;
};

std::uint32_t big(std::string_view b, std::size_t at, int n)
{
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i)
        v = (v << 8) | static_cast<std::uint8_t>(b[at + static_cast<std::size_t>(i)]);
    return v;
}

struct RawEvent {
    std::uint64_t tick;
    std::uint32_t track;
    std::uint32_t sequence;
    std::uint8_t status; // channel status, or 0xFF for a tempo change, 0xFE for any other meta / sysex
    std::uint8_t data1, data2;
    std::uint32_t tempo; // microseconds per quarter note (tempo changes)
};

constexpr std::size_t kMaxEvents = 20'000'000;

} // namespace

Result<ParsedMidi> parseMidiFile(std::string_view bytes)
{
    const auto bad = [] { return makeError(ErrorCode::ParseError, "not a valid MIDI file"); };
    if (bytes.size() < 14 || bytes.substr(0, 4) != "MThd")
        return bad();
    const std::uint32_t headerLength = big(bytes, 4, 4);
    if (headerLength < 6 || 8 + static_cast<std::uint64_t>(headerLength) > bytes.size())
        return bad();
    const std::uint32_t division = big(bytes, 12, 2);
    if (division == 0)
        return bad();

    std::vector<RawEvent> raw;
    std::size_t pos = 8 + headerLength;
    std::uint32_t track = 0;
    while (pos + 8 <= bytes.size()) {
        const std::string_view id = bytes.substr(pos, 4);
        const std::uint32_t length = big(bytes, pos + 4, 4);
        const std::size_t begin = pos + 8;
        const std::size_t end = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), static_cast<std::uint64_t>(begin) + length));
        pos = end;
        if (id != "MTrk")
            continue; // unknown chunks are skipped
        Reader r(bytes, begin, end);
        std::uint64_t tick = 0;
        std::uint8_t running = 0;
        std::uint32_t sequence = 0;
        while (!r.atEnd() && r.ok()) {
            tick += r.vlq();
            std::uint8_t status = r.peek();
            if (status >= 0x80)
                r.byte();
            else if (running != 0)
                status = running; // running status: this byte is data
            else
                break; // data without a status: the rest of the track is unreadable
            if (status == 0xFF) {
                const std::uint8_t type = r.byte();
                const std::string_view data = r.take(r.vlq());
                if (type == 0x2F)
                    break; // end of track
                RawEvent e{tick, track, sequence++, 0xFE, 0, 0, 0};
                if (type == 0x51 && data.size() == 3) {
                    e.status = 0xFF;
                    e.tempo = big(data, 0, 3);
                }
                raw.push_back(e);
            } else if (status == 0xF0 || status == 0xF7) {
                r.take(r.vlq());
                raw.push_back({tick, track, sequence++, 0xFE, 0, 0, 0});
            } else if (status >= 0xF0) {
                break; // system real-time / common messages do not belong in a file
            } else {
                running = status;
                const std::uint8_t kind = status & 0xF0;
                const std::uint8_t d1 = r.byte() & 0x7F;
                const std::uint8_t d2 = (kind == 0xC0 || kind == 0xD0) ? 0 : static_cast<std::uint8_t>(r.byte() & 0x7F);
                raw.push_back({tick, track, sequence++, status, d1, d2, 0});
            }
            if (raw.size() > kMaxEvents)
                return makeError(ErrorCode::LimitExceeded, "the MIDI file has too many events");
        }
        ++track;
    }
    if (track == 0)
        return bad();

    // Merge the tracks by tick (tempo changes apply to every track), then
    // turn ticks into seconds through the tempo map in double precision.
    std::stable_sort(raw.begin(), raw.end(), [](const RawEvent& a, const RawEvent& b) {
        return std::tie(a.tick, a.track, a.sequence) < std::tie(b.tick, b.track, b.sequence);
    });
    ParsedMidi out;
    const bool smpte = (division & 0x8000) != 0;
    double secondsPerTick = 0.0;
    double quarterTicks = 0.0;
    if (smpte) {
        const int framesPerSecond = -static_cast<std::int8_t>(division >> 8);
        const int ticksPerFrame = static_cast<int>(division & 0xFF);
        if (framesPerSecond <= 0 || ticksPerFrame <= 0)
            return bad();
        secondsPerTick = 1.0 / (framesPerSecond * ticksPerFrame);
    } else {
        quarterTicks = static_cast<double>(division);
        secondsPerTick = 0.5 / quarterTicks; // 120 BPM until the first tempo change
    }
    std::uint64_t segmentTick = 0;
    double segmentSeconds = 0.0;
    for (const RawEvent& e : raw) {
        const double seconds = segmentSeconds + static_cast<double>(e.tick - segmentTick) * secondsPerTick;
        out.lastEventSeconds = std::max(out.lastEventSeconds, seconds);
        if (e.status == 0xFF) {
            if (!smpte && e.tempo > 0) {
                segmentTick = e.tick;
                segmentSeconds = seconds;
                secondsPerTick = e.tempo / 1e6 / quarterTicks;
                out.tempo.push_back({seconds, 60'000'000.0 / e.tempo});
            }
        } else if (e.status != 0xFE) {
            out.events.push_back({seconds, e.status, e.data1, e.data2});
        }
    }
    // Length in quarter notes, for the average tempo (SMPTE time has no tempo: count it as 120 BPM).
    out.quarterNotes = smpte ? out.lastEventSeconds * 2.0 : raw.empty() ? 0.0 : static_cast<double>(raw.back().tick) / quarterTicks;
    return out;
}

Result<MidiContents> readMidiFile(const std::filesystem::path& path)
{
    auto bytes = readFileBytes(path);
    if (!bytes)
        return bytes.error();
    auto parsed = parseMidiFile(*bytes);
    if (!parsed)
        return makeError(ErrorCode::ParseError, "not a valid MIDI file: " + pathToUtf8(path));
    MidiContents contents;
    for (const MidiEvent& e : parsed->events)
        if ((e.status & 0xF0) == 0x90 && e.data2 > 0)
            contents.notes.push_back({e.seconds, e.status & 0x0F, e.data1, e.data2});
    contents.tempo = std::move(parsed->tempo);
    return contents;
}

} // namespace daw
