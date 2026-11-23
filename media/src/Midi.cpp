// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/Midi.h>

#include <daw/core/Files.h>

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <tml.h>
#include <tsf.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

namespace daw {

namespace {

constexpr int kDrumChannel = 9; // MIDI channel 10

struct MidiMessages {
    tml_message* first = nullptr;
    ~MidiMessages() { tml_free(first); }
};

Result<std::unique_ptr<MidiMessages>> loadMessages(const std::filesystem::path& path)
{
    // Read the bytes ourselves: the loader's own file API cannot open Unicode paths on Windows.
    auto bytes = readFileBytes(path);
    if (!bytes)
        return bytes.error();
    if (bytes->size() > static_cast<std::size_t>(64) * 1024 * 1024)
        return makeError(ErrorCode::LimitExceeded, "MIDI file is too large: " + pathToUtf8(path));
    auto messages = std::make_unique<MidiMessages>();
    messages->first = tml_load_memory(bytes->data(), static_cast<int>(bytes->size()));
    if (messages->first == nullptr)
        return makeError(ErrorCode::ParseError, "not a valid MIDI file: " + pathToUtf8(path));
    return messages;
}

std::mutex& cacheMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, std::weak_ptr<const SoundFont>>& cache()
{
    static std::map<std::string, std::weak_ptr<const SoundFont>> fonts;
    return fonts;
}

} // namespace

struct SoundFont::Impl {
    tsf* master = nullptr;
    // tsf_copy/tsf_close update a shared, non-atomic reference count.
    mutable std::mutex copyMutex;

    tsf* copy() const
    {
        std::lock_guard lock(copyMutex);
        return tsf_copy(master);
    }

    void close(tsf* instance) const
    {
        std::lock_guard lock(copyMutex);
        tsf_close(instance);
    }
};

SoundFont::SoundFont() : impl(std::make_unique<Impl>()) {}

SoundFont::~SoundFont()
{
    if (impl && impl->master != nullptr)
        tsf_close(impl->master);
}

int SoundFont::presetCount() const noexcept
{
    return tsf_get_presetcount(impl->master);
}

bool isMidiFile(const std::filesystem::path& path)
{
    std::string ext = pathToUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".mid" || ext == ".midi";
}

Result<MidiFileInfo> probeMidiFile(const std::filesystem::path& path)
{
    auto messages = loadMessages(path);
    if (!messages)
        return messages.error();

    MidiFileInfo info;
    unsigned int channels = 0;
    double quarters = 0.0;
    double tempoMicros = 500000.0; // 120 BPM until the first tempo event
    unsigned int lastTime = 0, tempoTime = 0;
    for (tml_message* m = (*messages)->first; m != nullptr; m = m->next) {
        lastTime = std::max(lastTime, m->time);
        if (m->type == TML_SET_TEMPO) {
            quarters += (m->time - tempoTime) * 1000.0 / tempoMicros;
            tempoTime = m->time;
            const int value = tml_get_tempo_value(m);
            if (value > 0)
                tempoMicros = value;
        } else if (m->type == TML_NOTE_ON && m->velocity > 0) {
            ++info.noteCount;
            channels |= 1u << (m->channel & 15);
            info.usesDrums = info.usesDrums || m->channel == kDrumChannel;
        }
    }
    quarters += (lastTime - tempoTime) * 1000.0 / tempoMicros;
    info.durationSeconds = lastTime / 1000.0;
    if (lastTime > 0)
        info.averageBpm = quarters / (info.durationSeconds / 60.0);
    for (unsigned int c = channels; c != 0; c &= c - 1)
        ++info.channelCount;
    if (info.noteCount == 0)
        return makeError(ErrorCode::UnsupportedFormat, "the MIDI file has no notes: " + pathToUtf8(path));
    return info;
}

std::int64_t midiRenderFrames(const MidiFileInfo& info, double sampleRate)
{
    return static_cast<std::int64_t>(std::ceil((info.durationSeconds + kMidiTailSeconds) * sampleRate));
}

Result<std::shared_ptr<const SoundFont>> loadSoundFont(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::string key = pathToUtf8(std::filesystem::weakly_canonical(path, ec));
    std::lock_guard lock(cacheMutex());
    if (auto existing = cache()[key].lock())
        return existing;

    auto bytes = readFileBytes(path);
    if (!bytes)
        return bytes.error();
    std::shared_ptr<SoundFont> font(new SoundFont());
    font->impl->master = tsf_load_memory(bytes->data(), static_cast<int>(std::min<std::size_t>(bytes->size(), INT32_MAX)));
    if (font->impl->master == nullptr || tsf_get_presetcount(font->impl->master) == 0)
        return makeError(ErrorCode::UnsupportedFormat, "not a usable SoundFont (SF2): " + pathToUtf8(path));
    cache()[key] = font;
    return std::shared_ptr<const SoundFont>(font);
}

Result<AudioAssetPtr> renderMidiFile(const std::filesystem::path& path, const SoundFont& soundFont,
                                     const MidiRenderOptions& options, const CancellationToken& cancel,
                                     const std::function<void(double)>& progress)
{
    const double rate = options.sampleRate;
    if (!(rate >= 8000.0 && rate <= 384000.0))
        return makeError(ErrorCode::InvalidArgument, "unsupported render sample rate");
    auto info = probeMidiFile(path);
    if (!info)
        return info.error();
    auto messages = loadMessages(path);
    if (!messages)
        return messages.error();

    tsf* synth = soundFont.impl->copy();
    if (synth == nullptr)
        return makeError(ErrorCode::Unknown, "could not create a synthesizer instance");
    struct Closer {
        const SoundFont& font;
        tsf* synth;
        ~Closer() { font.impl->close(synth); }
    } closer{soundFont, synth};

    tsf_set_output(synth, TSF_STEREO_INTERLEAVED, static_cast<int>(std::lround(rate)), options.gainDb);
    for (int channel = 0; channel < 16; ++channel)
        tsf_channel_set_presetnumber(synth, channel, 0, channel == kDrumChannel ? 1 : 0);

    const std::int64_t total = midiRenderFrames(*info, rate);
    auto asset = AudioAsset::create(2, total, rate);
    std::vector<float> interleaved(2 * 4096);
    std::int64_t position = 0;
    std::int64_t lastProgress = 0;

    const auto renderTo = [&](std::int64_t end) -> bool {
        end = std::min(end, total);
        while (position < end) {
            if (cancel.isCancelled())
                return false;
            const int n = static_cast<int>(std::min<std::int64_t>(4096, end - position));
            tsf_render_float(synth, interleaved.data(), n, 0);
            float* left = asset->channels[0].data() + position;
            float* right = asset->channels[1].data() + position;
            for (int i = 0; i < n; ++i) {
                left[i] = interleaved[static_cast<std::size_t>(2 * i)];
                right[i] = interleaved[static_cast<std::size_t>(2 * i + 1)];
            }
            position += n;
            if (progress && position - lastProgress >= static_cast<std::int64_t>(rate * 10.0)) {
                lastProgress = position;
                progress(static_cast<double>(position) / static_cast<double>(total));
            }
        }
        return true;
    };

    for (tml_message* m = (*messages)->first; m != nullptr; m = m->next) {
        if (!renderTo(static_cast<std::int64_t>(std::llround(m->time * rate / 1000.0))))
            return makeError(ErrorCode::Cancelled, "MIDI render cancelled");
        const int channel = m->channel & 15;
        switch (m->type) {
        case TML_PROGRAM_CHANGE:
            tsf_channel_set_presetnumber(synth, channel, static_cast<unsigned char>(m->program), channel == kDrumChannel ? 1 : 0);
            break;
        case TML_NOTE_ON:
            if (m->velocity > 0)
                tsf_channel_note_on(synth, channel, static_cast<unsigned char>(m->key), static_cast<unsigned char>(m->velocity) / 127.0f);
            else
                tsf_channel_note_off(synth, channel, static_cast<unsigned char>(m->key));
            break;
        case TML_NOTE_OFF:
            tsf_channel_note_off(synth, channel, static_cast<unsigned char>(m->key));
            break;
        case TML_PITCH_BEND:
            tsf_channel_set_pitchwheel(synth, channel, m->pitch_bend);
            break;
        case TML_CONTROL_CHANGE:
            tsf_channel_midi_control(synth, channel, static_cast<unsigned char>(m->control),
                                     static_cast<unsigned char>(m->control_value));
            break;
        default:
            break;
        }
    }
    if (!renderTo(total))
        return makeError(ErrorCode::Cancelled, "MIDI render cancelled");
    if (progress)
        progress(1.0);
    asset->sourcePath = pathToUtf8(path);
    return AudioAssetPtr(std::move(asset));
}

} // namespace daw
