// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// track-beats: the beats and bars of a song without a drum stem, from a full
// mix or from several stems played together (summed), and optionally a score
// against reference beats (for example those drums-to-midi --dump-beats
// tracks on the song's drum stem).
//
//   track-beats <audio.wav> [more.wav ...] [--reference=beats.txt] [--dump=beats.txt]
//
// Beats files hold one beat per line: its time in seconds, then 1 on a bar
// line, else 0.

#include <daw/core/Files.h>
#include <daw/dsp/BeatTracker.h>
#include <daw/media/AudioFileDecoder.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace daw;

namespace {

std::string option(int argc, char** argv, const std::string& name, const std::string& fallback = {})
{
    const std::string prefix = "--" + name + "=";
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]).rfind(prefix, 0) == 0)
            return std::string(argv[i]).substr(prefix.size());
    return fallback;
}

/// F-measure of found times against reference times (one-to-one, within the
/// tolerance), counting only found times within the reference's span; and
/// the mean offset of the matches.
double fMeasure(const std::vector<double>& found, const std::vector<double>& reference, double tolerance, double* meanOffset = nullptr)
{
    if (found.empty() || reference.empty())
        return 0.0;
    std::vector<bool> used(reference.size(), false);
    int counted = 0, matched = 0;
    double offsets = 0.0;
    for (double t : found) {
        if (t < reference.front() - tolerance || t > reference.back() + tolerance)
            continue;
        ++counted;
        const auto j = static_cast<std::size_t>(std::lower_bound(reference.begin(), reference.end(), t) - reference.begin());
        int best = -1;
        for (std::size_t k : {j - 1, j})
            if (k < reference.size() && !used[k] && std::abs(reference[k] - t) <= tolerance
                && (best < 0 || std::abs(reference[k] - t) < std::abs(reference[static_cast<std::size_t>(best)] - t)))
                best = static_cast<int>(k);
        if (best >= 0) {
            used[static_cast<std::size_t>(best)] = true;
            ++matched;
            offsets += t - reference[static_cast<std::size_t>(best)];
        }
    }
    if (meanOffset != nullptr)
        *meanOffset = matched > 0 ? offsets / matched : 0.0;
    const double p = matched / std::max(1.0, static_cast<double>(counted));
    const double r = matched / static_cast<double>(reference.size());
    return p + r > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]).rfind("--", 0) != 0)
            inputs.emplace_back(argv[i]);
    if (inputs.empty()) {
        std::fprintf(stderr, "usage: track-beats <audio.wav> [more.wav ...] [--reference=beats.txt] [--dump=beats.txt]\n");
        return 2;
    }

    // The inputs played together: summed at the first file's rate.
    std::vector<float> left, right;
    double rate = 0.0;
    for (const std::string& input : inputs) {
        auto audio = decodeAudioFile(utf8ToPath(input));
        if (!audio) {
            std::fprintf(stderr, "%s: %s\n", input.c_str(), audio.error().message.c_str());
            return 1;
        }
        const AudioAsset& a = **audio;
        if (rate == 0.0)
            rate = a.sampleRate;
        else if (a.sampleRate != rate) {
            std::fprintf(stderr, "%s: %.0f Hz, the first file is %.0f Hz\n", input.c_str(), a.sampleRate, rate);
            return 1;
        }
        const auto frames = static_cast<std::size_t>(a.numFrames());
        if (frames > left.size()) {
            left.resize(frames, 0.0f);
            right.resize(frames, 0.0f);
        }
        const float* l = a.channelData(0);
        const float* r = a.channelData(a.numChannels() > 1 ? 1 : 0);
        for (std::size_t i = 0; i < frames; ++i) {
            left[i] += l[i];
            right[i] += r[i];
        }
    }

    const auto started = std::chrono::steady_clock::now();
    const BeatTrack beats = trackBeatsInAudio(left.data(), right.data(), static_cast<std::int64_t>(left.size()), rate);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (beats.beats.empty()) {
        std::printf("too little rhythm to track beats in\n");
        return 1;
    }
    std::printf("  beats %8zu, %.2f BPM median, %zu bars, first downbeat at %.3f s (%.1f s)\n", beats.beats.size(), beats.bpm,
                beats.downbeats.size(), beats.downbeats.empty() ? 0.0 : beats.beats[beats.downbeats.front()], seconds);

    if (const std::string dump = option(argc, argv, "dump"); !dump.empty()) {
        std::FILE* f = std::fopen(dump.c_str(), "w");
        for (std::size_t i = 0; i < beats.beats.size(); ++i)
            std::fprintf(f, "%.4f %d\n", beats.beats[i], std::binary_search(beats.downbeats.begin(), beats.downbeats.end(), i) ? 1 : 0);
        std::fclose(f);
    }
    if (const std::string reference = option(argc, argv, "reference"); !reference.empty()) {
        std::FILE* f = std::fopen(reference.c_str(), "r");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot read %s\n", reference.c_str());
            return 1;
        }
        std::vector<double> refBeats, refBars;
        double t = 0.0;
        int bar = 0;
        while (std::fscanf(f, "%lf %d", &t, &bar) == 2) {
            refBeats.push_back(t);
            if (bar != 0)
                refBars.push_back(t);
        }
        std::fclose(f);
        std::vector<double> bars;
        for (std::size_t i : beats.downbeats)
            bars.push_back(beats.beats[i]);
        double offset = 0.0;
        const double fBeats = fMeasure(beats.beats, refBeats, 0.07, &offset);
        const double fBars = fMeasure(bars, refBars, 0.07);
        std::printf("  score beats F %.3f (mean offset %+.1f ms), downbeats F %.3f (within 70 ms)\n", fBeats, 1000.0 * offset, fBars);
    }
    return 0;
}
