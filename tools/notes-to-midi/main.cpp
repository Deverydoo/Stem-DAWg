// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// notes-to-midi: transcribes the notes of a pitched stem (bass, guitar, keys,
// synth, voice) with Basic Pitch to a MIDI file, and optionally scores them
// against a reference MIDI file: precision, recall and F-measure of notes
// whose start lies within a tolerance and whose key matches (also counted
// regardless of octave), and the timing offset of the matched notes.
//
//   notes-to-midi <stem.wav | part.mid --soundfont=file.sf2> [--reference=ref.mid] [--out=file.mid]
//                 [--beats=drums.wav] [--grid=16] [--tolerance=ms] [--onset=0.5] [--frame=0.3]
//                 [--min-note=ms] [--lowest=key] [--highest=key] [--shift=ms | --lags=ms,ms | --lag-file=drum-lags.txt] [--dump=notes.txt]
//
// A MIDI input is rendered through the SoundFont and transcribed back against
// itself, so the reference is exact. Otherwise the reference may be offset
// from the audio (Suno's MIDI is): each 10 s window is aligned by the lag that
// matches the most notes, unless --shift gives one.

#include <daw/ai/NoteTranscriber.h>
#include <daw/core/Files.h>
#include <daw/dsp/BeatTracker.h>
#include <daw/dsp/DrumTranscriber.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/Midi.h>
#include <daw/media/MidiFile.h>
#include <daw/session/NoteMidi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace daw;

namespace {

struct Note {
    double t;
    int key;
};

struct Score {
    int truePositives = 0;
    std::vector<double> offsets;
};

/// Greedy one-to-one matching of notes sorted by time: same key (or same
/// pitch class with `anyOctave`) within a tolerance, the nearest first.
Score match(const std::vector<Note>& found, const std::vector<Note>& reference, double tolerance, bool anyOctave)
{
    Score s;
    std::vector<bool> used(reference.size(), false);
    std::size_t start = 0;
    for (const Note& n : found) {
        while (start < reference.size() && reference[start].t < n.t - tolerance)
            ++start;
        int best = -1;
        for (std::size_t j = start; j < reference.size() && reference[j].t <= n.t + tolerance; ++j) {
            const bool same = anyOctave ? (reference[j].key - n.key) % 12 == 0 : reference[j].key == n.key;
            if (same && !used[j] &&
                (best < 0 || std::abs(reference[j].t - n.t) < std::abs(reference[static_cast<std::size_t>(best)].t - n.t)))
                best = static_cast<int>(j);
        }
        if (best >= 0) {
            used[static_cast<std::size_t>(best)] = true;
            ++s.truePositives;
            s.offsets.push_back(n.t - reference[static_cast<std::size_t>(best)].t);
        }
    }
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

void report(const char* label, const std::vector<Note>& found, const std::vector<Note>& expected, double tolerance, bool anyOctave)
{
    const Score s = match(found, expected, tolerance, anyOctave);
    const double p = s.truePositives / std::max(1.0, static_cast<double>(found.size()));
    const double r = s.truePositives / std::max(1.0, static_cast<double>(expected.size()));
    std::printf("  %-12s %6zu %6zu %6d %6.2f %6.2f %6.2f %9.1f\n", label, expected.size(), found.size(), s.truePositives, p, r,
                p + r > 0 ? 2 * p * r / (p + r) : 0.0, 1000.0 * median(s.offsets));
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: notes-to-midi <stem.wav | part.mid --soundfont=file.sf2> [--reference=ref.mid] [--out=file.mid] "
                             "[--beats=drums.wav] [--grid=16]\n");
        return 2;
    }
    const auto input = utf8ToPath(argv[1]);
    const bool synth = isMidiFile(input);
    Result<AudioAssetPtr> audio = makeError(ErrorCode::InvalidArgument, "no input");
    if (synth) {
        const std::string soundFontPath = option(argc, argv, "soundfont");
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

    auto backend = createOnnxBackend({.intraOpThreads = 4});
    if (!backend) {
        std::fprintf(stderr, "%s\n", backend.error().describe().c_str());
        return 1;
    }
    auto transcriber = NoteTranscriber::load(*backend);
    if (!transcriber) {
        std::fprintf(stderr, "%s\n", transcriber.error().describe().c_str());
        return 1;
    }

    // The song's beats, from a drum stem.
    BeatTrack beats;
    if (const std::string drumsPath = option(argc, argv, "beats"); !drumsPath.empty()) {
        auto drums = decodeAudioFile(utf8ToPath(drumsPath));
        if (!drums) {
            std::fprintf(stderr, "%s\n", drums.error().describe().c_str());
            return 1;
        }
        const AudioAsset& d = **drums;
        beats = trackBeats(transcribeDrums(d.channels[0].data(), d.channels[d.numChannels() > 1 ? 1 : 0].data(), d.numFrames(),
                                           d.sampleRate, {}));
    }

    NoteMidiOptions options;
    options.grid = std::stoi(option(argc, argv, "grid", "0"));
    // Decoding as the app does for the file's name, unless overridden.
    NoteDecodeParams& decode = options.decode.emplace(noteDecodeParamsFor(pathToUtf8(input.stem())));
    if (const std::string v = option(argc, argv, "onset"); !v.empty())
        decode.onsetThreshold = std::stof(v);
    if (const std::string v = option(argc, argv, "frame"); !v.empty())
        decode.frameThreshold = std::stof(v);
    if (const std::string v = option(argc, argv, "min-note"); !v.empty())
        decode.minNoteSeconds = std::stod(v) / 1000.0;
    if (const std::string v = option(argc, argv, "lowest"); !v.empty())
        decode.lowestKey = std::stoi(v);
    if (const std::string v = option(argc, argv, "highest"); !v.empty())
        decode.highestKey = std::stoi(v);
    const auto started = std::chrono::steady_clock::now();
    auto result = notesToMidi(**audio, **transcriber, options, beats, pathToUtf8(input.stem()));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().describe().c_str());
        return 1;
    }
    int lowest = 127, highest = 0;
    for (const PitchedNote& n : result->notes) {
        lowest = std::min(lowest, n.key);
        highest = std::max(highest, n.key);
    }
    std::printf("%s: %.1f s of audio, %zu notes (keys %d-%d) in %.2f s, program %d, onsets %.2f, min %.0f ms%s\n",
                pathToUtf8(input.filename()).c_str(), (*audio)->durationSeconds(), result->notes.size(), lowest, highest, seconds,
                result->program, decode.onsetThreshold, 1000.0 * decode.minNoteSeconds, result->onBeats ? ", on the song's beats" : "");
    if (const std::string out = option(argc, argv, "out"); !out.empty()) {
        if (auto status = writeMidiFile(utf8ToPath(out), result->song); !status) {
            std::fprintf(stderr, "%s\n", status.error().describe().c_str());
            return 1;
        }
        std::printf("  wrote %s\n", out.c_str());
    }
    if (const std::string dump = option(argc, argv, "dump"); !dump.empty()) {
        std::FILE* f = std::fopen(dump.c_str(), "w");
        for (const PitchedNote& n : result->notes)
            std::fprintf(f, "%.4f %.4f %d %.3f\n", n.start, n.end, n.key, n.amplitude);
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
    const double tolerance = std::stod(option(argc, argv, "tolerance", "50")) / 1000.0;
    std::vector<Note> found, expected;
    for (const PitchedNote& n : result->notes)
        found.push_back({n.start, n.key});
    for (const MidiNote& n : ref->notes)
        if (n.channel != kGmDrumChannel)
            expected.push_back({n.seconds, n.key});
    std::string how = "exact";
    if (const std::string shift = synth ? std::string() : option(argc, argv, "shift"); !shift.empty()) {
        for (Note& n : expected)
            n.t += std::stod(shift) / 1000.0;
        how = "shifted " + shift + " ms";
    } else if (const std::string lagFile = option(argc, argv, "lag-file"); !synth && !lagFile.empty()) {
        // Per-window lags found on the drum stem (drums-to-midi --dump-lags).
        std::FILE* f = std::fopen(lagFile.c_str(), "r");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot read %s\n", lagFile.c_str());
            return 1;
        }
        double w = 0.0, lag = 0.0;
        std::vector<std::pair<double, double>> lags;
        while (std::fscanf(f, "%lf %lf", &w, &lag) == 2)
            lags.emplace_back(w, lag);
        std::fclose(f);
        for (Note& n : expected) {
            double lagHere = lags.empty() ? 0.0 : lags.front().second;
            for (const auto& [start, l] : lags)
                if (n.t >= start)
                    lagHere = l;
            n.t += lagHere;
        }
        how = "lags from " + lagFile;
    } else if (!synth) {
        constexpr double kWindow = 10.0;
        const double end = expected.empty() ? 0.0 : expected.back().t;
        // --lags=a,b: only lags within 30 ms of these (ms the reference is
        // late by), where a regular part fits any whole step of its grid.
        std::vector<int> centres;
        for (std::string list = option(argc, argv, "lags"); !list.empty();) {
            const auto comma = list.find(',');
            centres.push_back(std::stoi(list.substr(0, comma)));
            list = comma == std::string::npos ? std::string() : list.substr(comma + 1);
        }
        const auto near = [&](int ms) {
            return centres.empty() ||
                   std::any_of(centres.begin(), centres.end(), [&](int c) { return std::abs(ms - c) <= 30; });
        };
        std::vector<double> lags;
        for (double w = 0.0; w <= end; w += kWindow) {
            std::vector<Note> inWindow;
            for (const Note& n : expected)
                if (n.t >= w && n.t < w + kWindow)
                    inWindow.push_back(n);
            if (inWindow.size() < 4)
                continue;
            int best = -1;
            double lag = 0.0;
            for (int ms = -400; ms <= 400; ms += 2) {
                if (!near(ms))
                    continue;
                std::vector<Note> shifted = inWindow;
                for (Note& n : shifted)
                    n.t += ms / 1000.0;
                const int count = match(found, shifted, 0.030, true).truePositives;
                if (count > best || (count == best && std::abs(ms) < std::abs(lag * 1000.0))) {
                    best = count;
                    lag = ms / 1000.0;
                }
            }
            lags.push_back(lag);
            for (Note& n : expected)
                if (n.t >= w && n.t < w + kWindow)
                    n.t += lag;
        }
        std::sort(lags.begin(), lags.end());
        char text[96];
        std::snprintf(text, sizeof text, "aligned per 10 s, lags %+.0f..%+.0f ms", lags.empty() ? 0.0 : 1000.0 * lags.front(),
                      lags.empty() ? 0.0 : 1000.0 * lags.back());
        how = text;
    }
    std::sort(expected.begin(), expected.end(), [](const Note& a, const Note& b) { return a.t < b.t; });
    std::sort(found.begin(), found.end(), [](const Note& a, const Note& b) { return a.t < b.t; });
    std::printf("\nagainst %s (%s, tolerance %.0f ms):\n", pathToUtf8(utf8ToPath(reference).filename()).c_str(), how.c_str(),
                tolerance * 1000.0);
    std::printf("  %-12s %6s %6s %6s %6s %6s %6s %9s\n", "", "ref", "found", "TP", "prec", "recall", "F", "offset ms");
    report("notes", found, expected, tolerance, false);
    report("any octave", found, expected, tolerance, true);
    // Octave doublings (a bass line played in two octaves at once) count once
    // on both sides.
    const auto merged = [](std::vector<Note> notes) {
        std::vector<Note> out;
        for (const Note& n : notes) {
            const auto recent = static_cast<std::ptrdiff_t>(std::min<std::size_t>(out.size(), 8));
            const bool doubled = std::any_of(out.rbegin(), out.rbegin() + recent, [&](const Note& o) {
                return std::abs(o.t - n.t) < 0.03 && (o.key - n.key) % 12 == 0;
            });
            if (!doubled)
                out.push_back(n);
        }
        return out;
    };
    report("undoubled", merged(found), merged(expected), tolerance, true);
    return 0;
}
