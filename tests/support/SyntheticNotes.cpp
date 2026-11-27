// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "SyntheticNotes.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw::test {

namespace {

/// Adds a plucked tone: eight partials (1/h), 4 ms attack, decay to -40 dB
/// over 1.5 s, released with a 15 ms fade at the note's end.
void pluck(std::vector<float>& out, double rate, double start, double end, int key, float level)
{
    const double f0 = 440.0 * std::pow(2.0, (key - 69) / 12.0);
    const auto first = static_cast<std::size_t>(start * rate);
    const auto last = std::min(out.size(), static_cast<std::size_t>((end + 0.015) * rate));
    for (std::size_t i = first; i < last; ++i) {
        const double t = static_cast<double>(i - first) / rate;
        double env = std::min(1.0, t / 0.004);
        if (start + t > end)
            env *= 1.0 - (start + t - end) / 0.015;
        double v = 0.0;
        for (int h = 1; h <= 8; ++h) {
            if (f0 * h > rate * 0.45)
                break;
            // Like a plucked string: partials fall as 1/h^2 and the upper ones
            // die away sooner (the fundamental to -40 dB in 1.5 s).
            const double decay = std::exp(-t * std::log(100.0) * h / 1.5);
            v += std::sin(2.0 * std::numbers::pi * f0 * h * t) * decay / (h * h);
        }
        out[i] += static_cast<float>(level * env * v);
    }
}

} // namespace

NotePiece makeNotePiece(int bars, double bpm, bool chords, double rate, double startSeconds)
{
    NotePiece piece;
    const double beat = 60.0 / bpm;
    const double total = startSeconds + bars * 4 * beat + 1.0;
    std::vector<float> mono(static_cast<std::size_t>(total * rate), 0.0f);

    // Per bar: a root (A2, F2, C3, G2 cycling) and its chord above.
    const int roots[] = {45, 41, 48, 43};
    const int chordsAbove[][3] = {{64, 69, 72}, {65, 69, 72}, {64, 67, 72}, {62, 67, 71}};
    // The bass rhythm in sixteenths: eighths, then a run of repeated
    // sixteenths, then an octave jump.
    const struct {
        int at, length, interval;
    } rhythm[] = {{0, 2, 0}, {2, 2, 0}, {4, 2, 7}, {6, 2, 0}, {8, 1, 0}, {9, 1, 0}, {10, 1, 0}, {11, 1, 0}, {12, 2, 12}, {14, 2, 7}};
    const double sixteenth = beat / 4.0;
    for (int bar = 0; bar < bars; ++bar) {
        const double barStart = startSeconds + bar * 4 * beat;
        const int root = roots[bar % 4];
        for (const auto& r : rhythm) {
            const double s = barStart + r.at * sixteenth;
            // Played a little short of the next, as a bass player would.
            const double e = s + r.length * sixteenth * 0.85;
            pluck(mono, rate, s, e, root + r.interval, 0.25f);
            piece.notes.push_back({s, e, root + r.interval});
        }
        if (chords)
            for (int half = 0; half < 2; ++half) {
                const double s = barStart + half * 2 * beat;
                const double e = s + 2 * beat * 0.95;
                for (int key : chordsAbove[bar % 4]) {
                    pluck(mono, rate, s, e, key, 0.12f);
                    piece.notes.push_back({s, e, key});
                }
            }
        for (int b = 0; b < 4; ++b)
            piece.beats.push_back(barStart + b * beat);
    }
    std::sort(piece.notes.begin(), piece.notes.end(),
              [](const TestNote& a, const TestNote& b) { return a.start != b.start ? a.start < b.start : a.key < b.key; });

    piece.audio = std::make_shared<AudioAsset>();
    piece.audio->sampleRate = rate;
    piece.audio->channels = {mono, mono};
    return piece;
}

} // namespace daw::test
