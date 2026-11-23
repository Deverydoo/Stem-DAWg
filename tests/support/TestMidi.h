// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Builds MIDI files and a minimal SoundFont in code for tests: a one-note
// song and a SoundFont holding a single looped 480 Hz sine (root key 71).

#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

namespace daw::test {

inline std::string le16(unsigned v)
{
    return {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff)};
}

inline std::string le32(std::uint32_t v)
{
    return le16(v & 0xffff) + le16(v >> 16);
}

inline std::string be16(unsigned v)
{
    return {static_cast<char>((v >> 8) & 0xff), static_cast<char>(v & 0xff)};
}

inline std::string be32(std::uint32_t v)
{
    return be16(v >> 16) + be16(v & 0xffff);
}

inline std::string riffChunk(const char* id, const std::string& data)
{
    std::string out = std::string(id, 4) + le32(static_cast<std::uint32_t>(data.size())) + data;
    if (data.size() % 2)
        out.push_back('\0');
    return out;
}

inline std::string name20(const char* name)
{
    std::string out(name);
    out.resize(20, '\0');
    return out;
}

inline constexpr double kMidiTestRate = 48000.0;
inline constexpr int kPeriod = 100; // 480 Hz at 48 kHz

/// A SoundFont with one preset (bank 0, program 0) playing a looped sine whose
/// root key is 71: key 71 sounds at 480 Hz.
inline std::string makeSoundFont()
{
    std::string samples;
    const int frames = kPeriod * 48;
    for (int i = 0; i < frames; ++i)
        samples += le16(static_cast<unsigned>(static_cast<std::int16_t>(std::lround(16000.0 * std::sin(2.0 * std::numbers::pi * i / kPeriod)))) & 0xffff);
    samples.append(46 * 2, '\0'); // the zero samples SF2 requires after each sample

    const std::string info = "INFO" + riffChunk("ifil", le16(2) + le16(1)) + riffChunk("INAM", std::string("Test\0", 6));
    const std::string sdta = "sdta" + riffChunk("smpl", samples);

    const auto bag = [](unsigned gen, unsigned mod) { return le16(gen) + le16(mod); };
    const auto gen = [](unsigned oper, unsigned amount) { return le16(oper) + le16(amount); };
    const std::string zeroMod(10, '\0');
    std::string pdta = "pdta";
    pdta += riffChunk("phdr", name20("Test") + le16(0) + le16(0) + le16(0) + le32(0) + le32(0) + le32(0)
                                  + name20("EOP") + le16(0) + le16(0) + le16(1) + le32(0) + le32(0) + le32(0));
    pdta += riffChunk("pbag", bag(0, 0) + bag(1, 0));
    pdta += riffChunk("pmod", zeroMod);
    pdta += riffChunk("pgen", gen(41, 0) + gen(0, 0)); // instrument 0
    pdta += riffChunk("inst", name20("Sine") + le16(0) + name20("EOI") + le16(1));
    pdta += riffChunk("ibag", bag(0, 0) + bag(2, 0));
    pdta += riffChunk("imod", zeroMod);
    pdta += riffChunk("igen", gen(54, 1) + gen(53, 0) + gen(0, 0)); // looped, sample 0
    pdta += riffChunk("shdr", name20("Sine") + le32(0) + le32(frames) + le32(0) + le32(frames) + le32(48000)
                                  + std::string(1, static_cast<char>(71)) + std::string(1, '\0') + le16(0) + le16(1)
                                  + name20("EOS") + std::string(26, '\0'));

    const std::string body = "sfbk" + riffChunk("LIST", info) + riffChunk("LIST", sdta) + riffChunk("LIST", pdta);
    return "RIFF" + le32(static_cast<std::uint32_t>(body.size())) + body;
}

inline std::string varLen(std::uint32_t v)
{
    std::string out(1, static_cast<char>(v & 0x7f));
    while ((v >>= 7) != 0)
        out.insert(out.begin(), static_cast<char>(0x80 | (v & 0x7f)));
    return out;
}

struct Event {
    std::uint32_t tick;
    std::string bytes;
};

/// Format-0 MIDI file, 480 ticks per quarter note.
inline std::string makeMidi(const std::vector<Event>& events)
{
    std::string track;
    std::uint32_t last = 0;
    for (const Event& e : events) {
        track += varLen(e.tick - last) + e.bytes;
        last = e.tick;
    }
    track += varLen(0) + std::string("\xff\x2f\x00", 3);
    return "MThd" + be32(6) + be16(0) + be16(1) + be16(480) + "MTrk" + be32(static_cast<std::uint32_t>(track.size())) + track;
}

inline std::string tempo(std::uint32_t microsPerQuarter)
{
    return std::string("\xff\x51\x03", 3) + std::string(1, static_cast<char>(microsPerQuarter >> 16))
           + std::string(1, static_cast<char>((microsPerQuarter >> 8) & 0xff)) + std::string(1, static_cast<char>(microsPerQuarter & 0xff));
}

inline std::string noteOn(int channel, int key, int velocity)
{
    return {static_cast<char>(0x90 | channel), static_cast<char>(key), static_cast<char>(velocity)};
}

inline std::string noteOff(int channel, int key)
{
    return {static_cast<char>(0x80 | channel), static_cast<char>(key), 0};
}

/// One note, key 71, from 1.0 s to 1.5 s at 120 BPM.
inline std::string oneNoteSong()
{
    return makeMidi({{0, tempo(500000)}, {0, std::string("\xc0\x00", 2)}, {960, noteOn(0, 71, 100)}, {1440, noteOff(0, 71)}});
}

} // namespace daw::test
