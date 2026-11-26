// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// drums-to-midi: transcribes a drum stem to a General MIDI drum file, and
// optionally scores the hits against a reference MIDI file (for example
// Suno's own drum MIDI): precision, recall and F-measure per instrument
// within a tolerance, and the timing offset of the matched hits.
//
//   drums-to-midi <drums.wav> [--reference=ref.mid] [--tolerance=ms] [--sensitivity=x]

#include <daw/core/Files.h>
#include <daw/dsp/BeatTracker.h>
#include <daw/dsp/DrumTranscriber.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/Midi.h>
#include <daw/media/MidiFile.h>
#include <daw/session/DrumMidi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace daw;

namespace {

enum Group { kKickG, kSnareG, kHatG, kCymbalG, kTomG, kOtherG, kGroups };
const char* kGroupNames[] = {"kick", "snare", "hat", "cymbal", "tom", "other"};

/// `snareFamily`: side stick (37) and clap (39) count as snare. True for a
/// SoundFont render, where they are distinct snare-like sounds; not for
/// Suno's MIDI, which stamps 37 on most hats and kicks of a song.
Group groupOfKey(int key, bool snareFamily)
{
    if (snareFamily && (key == 37 || key == 39))
        return kSnareG;
    switch (key) {
    case 35: case 36: return kKickG;
    case 38: case 40: return kSnareG;
    case 42: case 44: case 46: return kHatG;
    case 49: case 51: case 52: case 53: case 55: case 57: case 59: return kCymbalG;
    case 41: case 43: case 45: case 47: case 48: case 50: return kTomG;
    default: return kOtherG;
    }
}

Group groupOfKind(DrumKind kind)
{
    switch (kind) {
    case DrumKind::Kick: return kKickG;
    case DrumKind::Snare: return kSnareG;
    case DrumKind::ClosedHat:
    case DrumKind::OpenHat: return kHatG;
    case DrumKind::Crash: return kCymbalG;
    }
    return kOtherG;
}

struct Score {
    int truePositives = 0, falsePositives = 0, falseNegatives = 0;
    std::vector<double> offsets;
};

/// Greedy one-to-one matching of sorted times within a tolerance.
Score match(const std::vector<double>& found, const std::vector<double>& reference, double tolerance)
{
    Score s;
    std::vector<bool> used(reference.size(), false);
    std::size_t start = 0;
    for (double t : found) {
        while (start < reference.size() && reference[start] < t - tolerance)
            ++start;
        int best = -1;
        for (std::size_t j = start; j < reference.size() && reference[j] <= t + tolerance; ++j)
            if (!used[j] && (best < 0 || std::abs(reference[j] - t) < std::abs(reference[static_cast<std::size_t>(best)] - t)))
                best = static_cast<int>(j);
        if (best >= 0) {
            used[static_cast<std::size_t>(best)] = true;
            ++s.truePositives;
            s.offsets.push_back(t - reference[static_cast<std::size_t>(best)]);
        } else {
            ++s.falsePositives;
        }
    }
    s.falseNegatives = static_cast<int>(reference.size()) - s.truePositives;
    return s;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::string option(int argc, char** argv, const std::string& name, const std::string& fallback = {})
{
    const std::string prefix = "--" + name + "=";
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]).rfind(prefix, 0) == 0)
            return std::string(argv[i]).substr(prefix.size());
    return fallback;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: drums-to-midi <drums.wav> [--reference=ref.mid] [--tolerance=ms] [--sensitivity=x]\n");
        return 2;
    }
    // --synth: the input is a MIDI file, rendered through a SoundFont and
    // transcribed back, so the reference is exact (no offset to find).
    const auto input = utf8ToPath(argv[1]);
    const std::string soundFontPath = option(argc, argv, "soundfont");
    const bool synth = isMidiFile(input);
    Result<AudioAssetPtr> audio = makeError(ErrorCode::InvalidArgument, "no input");
    if (synth) {
        if (soundFontPath.empty()) {
            std::fprintf(stderr, "a MIDI input needs --soundfont=file.sf2\n");
            return 2;
        }
        auto font = loadSoundFont(utf8ToPath(soundFontPath));
        if (!font) {
            std::fprintf(stderr, "%s\n", font.error().describe().c_str());
            return 1;
        }
        audio = renderMidiFile(input, **font);
    } else {
        audio = decodeAudioFile(input);
    }
    if (!audio) {
        std::fprintf(stderr, "%s\n", audio.error().describe().c_str());
        return 1;
    }
    const AudioAsset& a = **audio;
    DrumTranscriberParams params;
    params.sensitivity = std::stof(option(argc, argv, "sensitivity", "1"));
    const auto started = std::chrono::steady_clock::now();
    const DrumTranscription t =
        transcribeDrums(a.channels[0].data(), a.channels[a.numChannels() > 1 ? 1 : 0].data(), a.numFrames(), a.sampleRate, params);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    int counts[kDrumKindCount] = {};
    for (const DrumHit& h : t.hits)
        ++counts[static_cast<int>(h.kind)];
    std::printf("%s: %.1f s of audio, %zu hits in %.2f s\n", pathToUtf8(input.filename()).c_str(), a.numFrames() / a.sampleRate,
                t.hits.size(), seconds);
    for (int k = 0; k < kDrumKindCount; ++k)
        std::printf("  %-10s %d\n", drumKindName(static_cast<DrumKind>(k)), counts[k]);

    const BeatTrack beats = trackBeats(t);
    if (!beats.beats.empty()) {
        std::printf("  beats      %zu, %.2f BPM median, %zu bars, first downbeat at %.3f s\n", beats.beats.size(), beats.bpm,
                    beats.downbeats.size(), beats.downbeats.empty() ? 0.0 : beats.beats[beats.downbeats.front()]);
    }
    if (const std::string out = option(argc, argv, "out"); !out.empty()) {
        const int grid = std::stoi(option(argc, argv, "grid", "16"));
        const MidiSong song = composeDrumMidi(t.hits, beats, grid, pathToUtf8(input.stem()));
        if (auto status = writeMidiFile(utf8ToPath(out), song); !status) {
            std::fprintf(stderr, "%s\n", status.error().describe().c_str());
            return 1;
        }
        std::printf("  wrote %s: %zu notes, grid %d\n", out.c_str(), song.tracks.front().notes.size(), grid);
    }
    if (const std::string dump = option(argc, argv, "dump-beats"); !dump.empty()) {
        std::FILE* f = std::fopen(dump.c_str(), "w");
        for (std::size_t i = 0; i < beats.beats.size(); ++i)
            std::fprintf(f, "%.4f %d\n", beats.beats[i],
                         std::binary_search(beats.downbeats.begin(), beats.downbeats.end(), i) ? 1 : 0);
        std::fclose(f);
    }
    if (const std::string dump = option(argc, argv, "dump"); !dump.empty()) {
        std::FILE* f = std::fopen(dump.c_str(), "w");
        for (const DrumHit& h : t.hits)
            std::fprintf(f, "%.4f %d %.3f\n", h.seconds, static_cast<int>(h.kind), h.strength);
        std::fclose(f);
    }

    const std::string reference = synth ? std::string(argv[1]) : option(argc, argv, "reference");
    if (reference.empty())
        return 0;
    auto ref = readMidiFile(utf8ToPath(reference));
    if (!ref) {
        std::fprintf(stderr, "%s\n", ref.error().describe().c_str());
        return 1;
    }
    if (const std::string dump = option(argc, argv, "dump-reference"); !dump.empty()) {
        std::FILE* f = std::fopen(dump.c_str(), "w");
        for (const MidiNote& n : ref->notes)
            std::fprintf(f, "%.4f %d %d\n", n.seconds, n.key, n.velocity);
        std::fclose(f);
    }
    const double tolerance = std::stod(option(argc, argv, "tolerance", "50")) / 1000.0;
    std::vector<double> found[kGroups], expected[kGroups];
    for (const DrumHit& h : t.hits)
        found[groupOfKind(h.kind)].push_back(h.seconds);
    for (const MidiNote& n : ref->notes)
        if (n.channel == kGmDrumChannel)
            expected[groupOfKey(n.key, synth)].push_back(n.seconds);
    // The reference may be offset from the audio, and not by the same amount
    // throughout: Suno's drum MIDI runs about 230 ms early against its own
    // stem for most of a song and about 17 ms early elsewhere. So each 10 s
    // window of the reference is shifted by the lag (within +-400 ms) that
    // matches the most kicks within 20 ms. --shift=ms uses one fixed lag.
    std::string how;
    if (const std::string shift = synth ? std::string("0") : option(argc, argv, "shift"); !shift.empty()) {
        const double lag = std::stod(shift) / 1000.0;
        for (auto& times : expected)
            for (double& x : times)
                x += lag;
        how = "shifted " + shift + " ms";
    } else {
        constexpr double kWindow = 10.0;
        double end = 0.0;
        for (const auto& times : expected)
            if (!times.empty())
                end = std::max(end, times.back());
        std::vector<double> lags;
        for (double w = 0.0; w <= end; w += kWindow) {
            std::vector<double> kicks;
            for (double x : expected[kKickG])
                if (x >= w && x < w + kWindow)
                    kicks.push_back(x);
            if (kicks.empty())
                continue;
            int best = -1;
            double lag = 0.0;
            for (int ms = -400; ms <= 400; ms += 2) {
                std::vector<double> shifted = kicks;
                for (double& x : shifted)
                    x += ms / 1000.0;
                const int count = match(found[kKickG], shifted, 0.020).truePositives;
                if (count > best || (count == best && std::abs(ms) < std::abs(lag * 1000.0))) {
                    best = count;
                    lag = ms / 1000.0;
                }
            }
            lags.push_back(lag);
            for (auto& times : expected)
                for (double& x : times)
                    if (x >= w && x < w + kWindow)
                        x += lag;
        }
        for (auto& times : expected)
            std::sort(times.begin(), times.end());
        std::sort(lags.begin(), lags.end());
        char text[96];
        std::snprintf(text, sizeof text, "aligned per 10 s, lags %+.0f..%+.0f ms", lags.empty() ? 0.0 : 1000.0 * lags.front(),
                      lags.empty() ? 0.0 : 1000.0 * lags.back());
        how = text;
    }
    std::printf("\nagainst %s (%s, tolerance %.0f ms):\n", pathToUtf8(utf8ToPath(reference).filename()).c_str(), how.c_str(),
                tolerance * 1000.0);
    std::printf("  %-7s %6s %6s %6s %6s %6s %6s %9s\n", "", "ref", "found", "TP", "prec", "recall", "F", "offset ms");
    for (int g = 0; g < kGroups; ++g) {
        std::sort(expected[g].begin(), expected[g].end());
        expected[g].erase(std::unique(expected[g].begin(), expected[g].end(),
                                      [](double x, double y) { return std::abs(x - y) < 0.002; }),
                          expected[g].end());
        if (expected[g].empty() && found[g].empty())
            continue;
        const Score s = match(found[g], expected[g], tolerance);
        const double p = s.truePositives / std::max(1.0, static_cast<double>(found[g].size()));
        const double r = s.truePositives / std::max(1.0, static_cast<double>(expected[g].size()));
        std::printf("  %-7s %6zu %6zu %6d %6.2f %6.2f %6.2f %9.1f\n", kGroupNames[g], expected[g].size(), found[g].size(),
                    s.truePositives, p, r, p + r > 0 ? 2 * p * r / (p + r) : 0.0, 1000.0 * median(s.offsets));
    }
    return 0;
}
