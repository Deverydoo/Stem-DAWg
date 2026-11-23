// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Standard MIDI files, played through a SoundFont (SF2).
//
// A MIDI file is rendered offline into an ordinary AudioAsset, so a MIDI track
// behaves exactly like a stem: fader, pan, mute, solo, metering, the stem
// report and export all work unchanged. The tempo map of the file is honoured
// (Suno's MIDI exports follow the song's tempo beat by beat).
//
// Parsing uses TinyMidiLoader and synthesis TinySoundFont (both MIT). Event
// times are resolved to the millisecond by the loader.

#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace daw {

/// .mid / .midi (case-insensitive).
bool isMidiFile(const std::filesystem::path& path);

struct MidiFileInfo {
    double durationSeconds = 0.0; // time of the last event
    double averageBpm = 120.0;    // quarter notes per minute over the whole file
    int noteCount = 0;
    int channelCount = 0;         // channels that play notes
    bool usesDrums = false;       // notes on MIDI channel 10
};

/// Release time rendered after the last event, so final notes ring out.
inline constexpr double kMidiTailSeconds = 2.0;

Result<MidiFileInfo> probeMidiFile(const std::filesystem::path& path);

/// Length of a render of the file at `sampleRate`, in frames.
std::int64_t midiRenderFrames(const MidiFileInfo& info, double sampleRate);

/// A loaded SoundFont. Loading the 148 MB General MIDI set takes a moment and
/// about 300 MB of memory, so instances are shared: loadSoundFont() returns
/// the same object for the same file while anyone still holds it.
class SoundFont {
public:
    ~SoundFont();
    SoundFont(const SoundFont&) = delete;
    SoundFont& operator=(const SoundFont&) = delete;

    int presetCount() const noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl;

private:
    friend Result<std::shared_ptr<const SoundFont>> loadSoundFont(const std::filesystem::path& path);
    SoundFont();
};

Result<std::shared_ptr<const SoundFont>> loadSoundFont(const std::filesystem::path& path);

/// General MIDI renders at 0 dB are much louder than typical stems (full
/// velocity, dry samples) and drum kits peak above full scale. Renders start
/// 12 dB down, which puts Suno's MIDI near the level of its audio stems; the
/// track fader can raise them.
inline constexpr float kMidiRenderGainDb = -12.0f;

struct MidiRenderOptions {
    double sampleRate = 48000.0;
    float gainDb = kMidiRenderGainDb;
};

/// Renders the whole file (plus kMidiTailSeconds) to stereo. Thread-safe:
/// several files may render in parallel with the same SoundFont.
Result<AudioAssetPtr> renderMidiFile(const std::filesystem::path& path, const SoundFont& soundFont,
                                     const MidiRenderOptions& options = {}, const CancellationToken& cancel = {},
                                     const std::function<void(double)>& progress = {});

/// The General MIDI SoundFont Stem DAWg offers to download: FluidR3_GM by
/// Frank Wen (MIT licence), from Debian's fluid-soundfont-gm package. The
/// package and the extracted file are verified by SHA-256.
struct SoundFontPackage {
    const char* displayName;
    const char* url;
    const char* fallbackUrl;
    std::uint64_t packageBytes;
    const char* packageSha256;
    const char* member;   // path inside the package's data archive
    const char* fileName; // installed name
    std::uint64_t fileBytes;
    const char* fileSha256;
    const char* licence;
};

inline constexpr SoundFontPackage kFluidR3Gm{
    "FluidR3_GM",
    "https://deb.debian.org/debian/pool/main/f/fluid-soundfont/fluid-soundfont-gm_3.1-5.3_all.deb",
    "https://snapshot.debian.org/file/9cf2ecd68b96de93f048708ea52728eca171f692",
    119610208,
    "6f531493ac4e4d9772fd96b2488ea1790af81c196135fbdd25997da0781fc60e",
    "usr/share/sounds/sf2/FluidR3_GM.sf2",
    "FluidR3_GM.sf2",
    148398306,
    "74594e8f4250680adf590507a306655a299935343583256f3b722c48a1bc1cb0",
    "Copyright (c) 2000-2002, 2008 Frank Wen. MIT licence.",
};

} // namespace daw
