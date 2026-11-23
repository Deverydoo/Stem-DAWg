// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// The transient shaper inside the clean-up chain: transparent at 0 dB,
// sharpens slow rises without raising peaks or lifting what comes before a
// hit, shapes the decay, leaves steady tones (even a 40 Hz bass) alone, and
// stays real-time safe.

#include <daw/dsp/CleanupChain.h>

#include "AllocationGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace daw;

namespace {

constexpr double kRate = 48000.0;
constexpr std::size_t kHitSpacing = 24000; // 0.5 s
constexpr std::size_t kFirstHit = 12000;
constexpr int kFrame = 24;                 // 0.5 ms RMS frames for envelope measurements

struct Stereo {
    std::vector<float> left, right;
    explicit Stereo(std::size_t frames = 0) : left(frames, 0.0f), right(frames, 0.0f) {}
    std::size_t size() const { return left.size(); }
};

int latency()
{
    CleanupChain chain;
    chain.prepare(kRate);
    return chain.latency();
}

std::size_t ms(double milliseconds)
{
    return static_cast<std::size_t>(milliseconds * 0.001 * kRate);
}

std::size_t onsetOf(int hit)
{
    return kFirstHit + kHitSpacing * static_cast<std::size_t>(hit);
}

/// Hits that rise linearly (in amplitude) over `riseMs` to 0.5 and decay with
/// a 50 ms time constant: noise, or a 1 kHz tone. `bed` adds steady noise at
/// that amplitude underneath. Followed by silence as long as the latency.
Stereo hits(int count, double riseMs, bool tone = false, double bed = 0.0)
{
    Stereo s(kFirstHit + kHitSpacing * static_cast<std::size_t>(count) + static_cast<std::size_t>(latency()));
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const double rise = riseMs * 0.001 * kRate;
    for (std::size_t i = 0; i < s.size(); ++i) {
        s.left[i] = static_cast<float>(bed) * noise(rng);
        s.right[i] = static_cast<float>(bed) * noise(rng);
    }
    for (int h = 0; h < count; ++h)
        for (std::size_t i = 0; i < kHitSpacing; ++i) {
            const auto t = static_cast<double>(i);
            const double envelope = 0.5 * (t < rise ? t / rise : std::exp(-(t - rise) / (0.05 * kRate)));
            const std::size_t at = onsetOf(h) + i;
            if (tone) {
                s.left[at] = s.right[at] = static_cast<float>(envelope * std::sin(2.0 * std::numbers::pi * 1000.0 * t / kRate));
            } else {
                s.left[at] += static_cast<float>(envelope) * noise(rng);
                s.right[at] += static_cast<float>(envelope) * noise(rng);
            }
        }
    return s;
}

Stereo sine(double hz, double seconds, double amplitude)
{
    Stereo s(static_cast<std::size_t>(seconds * kRate));
    for (std::size_t i = 0; i < s.size(); ++i)
        s.left[i] = s.right[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    return s;
}

CleanupParams shaper(float attackDb, float sustainDb)
{
    CleanupParams p;
    p.transient = true;
    p.transientAttackDb = attackDb;
    p.transientSustainDb = sustainDb;
    return p;
}

/// The chain's output, moved back by its latency so it lines up with the input.
Stereo run(const CleanupParams& params, Stereo input, int block = 512)
{
    CleanupChain chain;
    chain.prepare(kRate);
    chain.setParams(params);
    for (std::size_t at = 0; at < input.size(); at += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), input.size() - at));
        chain.process(input.left.data() + at, input.right.data() + at, n);
    }
    const auto delay = static_cast<std::ptrdiff_t>(chain.latency());
    input.left.erase(input.left.begin(), input.left.begin() + delay);
    input.right.erase(input.right.begin(), input.right.begin() + delay);
    return input;
}

/// Output level relative to the input over [from, to), in dB.
double gainDb(const Stereo& in, const Stereo& out, std::size_t from, std::size_t to)
{
    double a = 0.0, b = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        a += static_cast<double>(in.left[i]) * in.left[i] + static_cast<double>(in.right[i]) * in.right[i];
        b += static_cast<double>(out.left[i]) * out.left[i] + static_cast<double>(out.right[i]) * out.right[i];
    }
    return 10.0 * std::log10(b / a);
}

/// RMS amplitude of the 0.5 ms frame starting at `at`.
double frameLevel(const Stereo& s, std::size_t at)
{
    double sum = 0.0;
    for (std::size_t i = at; i < at + kFrame; ++i)
        sum += 0.5 * (static_cast<double>(s.left[i]) * s.left[i] + static_cast<double>(s.right[i]) * s.right[i]);
    return std::sqrt(sum / kFrame);
}

/// Time from 10 % to 90 % of the hit's top, in ms (frames from the onset, so
/// the bed and the previous hit do not count). The top is the loudest frame
/// averaged with its neighbours: on noise a single frame is an extreme.
double riseMs(const Stereo& s, std::size_t onset)
{
    std::vector<double> level;
    for (std::size_t at = onset; at < onset + ms(20); at += kFrame)
        level.push_back(frameLevel(s, at));
    const auto peak = static_cast<std::size_t>(std::max_element(level.begin() + 1, level.end() - 1) - level.begin());
    const double top = (level[peak - 1] + level[peak] + level[peak + 1]) / 3.0;
    const auto cross = [&](double fraction) {
        const double line = fraction * top;
        std::size_t k = 0;
        while (k < peak && level[k] < line)
            ++k;
        if (k == 0)
            return 0.0;
        return static_cast<double>(k - 1) + (line - level[k - 1]) / (level[k] - level[k - 1]);
    };
    return (cross(0.9) - cross(0.1)) * 0.5;
}

double peakLevel(const Stereo& s, std::size_t onset)
{
    double peak = 0.0;
    for (std::size_t at = onset; at < onset + ms(20); at += kFrame)
        peak = std::max(peak, frameLevel(s, at));
    return peak;
}

} // namespace

TEST_CASE("At 0 dB the transient shaper passes the audio through unchanged", "[dsp][transient]")
{
    const Stereo in = hits(3, 5.0);
    const Stereo out = run(shaper(0.0f, 0.0f), in);
    for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out.left[i] == in.left[i]);
        REQUIRE(out.right[i] == in.right[i]);
    }
}

TEST_CASE("Attack sharpens a slow rise without raising the peak", "[dsp][transient]")
{
    const Stereo in = hits(4, 5.0, false, 0.005); // a bed 40 dB under the hits
    const Stereo sharp = run(shaper(6.0f, 0.0f), in);
    const Stereo soft = run(shaper(-6.0f, 0.0f), in);
    const Stereo softSharp = run(shaper(-6.0f, 0.0f), hits(4, 0.0));
    const Stereo sharpHits = hits(4, 0.0);
    for (int h = 1; h < 4; ++h) {
        const std::size_t onset = onsetOf(h);
        const double before = riseMs(in, onset);
        CHECK(before > 3.5);
        CHECK(riseMs(sharp, onset) < 0.7 * before);
        // Softening turns the hit's start down.
        CHECK(20.0 * std::log10(peakLevel(softSharp, onset) / peakLevel(sharpHits, onset)) < -3.0);
        CHECK(gainDb(in, soft, onset, onset + ms(5)) < -3.0);
        // The hit arrives sooner but no louder.
        CHECK(20.0 * std::log10(peakLevel(sharp, onset) / peakLevel(in, onset)) < 0.3);
        // What comes before the hit is not lifted (no pre-echo) ...
        CHECK(std::abs(gainDb(in, sharp, onset - ms(20), onset - ms(1))) < 0.3);
        // ... and the decay is left alone.
        CHECK(std::abs(gainDb(in, sharp, onset + ms(60), onset + ms(150))) < 0.3);
    }
}

TEST_CASE("Attack leaves a hit that already rises instantly alone", "[dsp][transient]")
{
    const Stereo in = hits(3, 0.0);
    const Stereo out = run(shaper(12.0f, 0.0f), in);
    for (int h = 1; h < 3; ++h) {
        const std::size_t onset = onsetOf(h);
        CHECK(std::abs(gainDb(in, out, onset, onset + ms(5))) < 1.0);
        CHECK(20.0 * std::log10(peakLevel(out, onset) / peakLevel(in, onset)) < 0.3);
    }
}

TEST_CASE("Sustain gain shapes the decay, not the hit", "[dsp][transient]")
{
    const Stereo in = hits(4, 0.0);
    const Stereo tight = run(shaper(0.0f, -6.0f), in);
    const Stereo ringing = run(shaper(0.0f, 6.0f), in);
    for (int h = 1; h < 4; ++h) {
        const std::size_t onset = onsetOf(h);
        // The previous hit's tail was turned down; the new hit is not.
        CHECK(std::abs(gainDb(in, tight, onset, onset + ms(5))) < 0.3);
        const double tail = gainDb(in, tight, onset + ms(100), onset + ms(250));
        CHECK(tail < -5.0);
        CHECK(tail > -6.05);
        CHECK(gainDb(in, ringing, onset + ms(100), onset + ms(250)) > 5.0);
    }
}

TEST_CASE("Steady tones, even a 40 Hz bass note, are not mistaken for hits", "[dsp][transient]")
{
    for (const double hz : {40.0, 100.0, 1000.0}) {
        const Stereo in = sine(hz, 2.0, 0.25);
        const Stereo out = run(shaper(12.0f, 12.0f), in);
        // After the note's own start (1 s in), the level stays put.
        CHECK(std::abs(gainDb(in, out, 48000, 48000 + 36000)) < 0.1);
        float inPeak = 0.0f, outPeak = 0.0f;
        for (std::size_t i = 48000; i < 48000 + 36000; ++i) {
            inPeak = std::max(inPeak, std::abs(in.left[i]));
            outPeak = std::max(outPeak, std::abs(out.left[i]));
        }
        CHECK(std::abs(20.0 * std::log10(outPeak / inPeak)) < 0.2);
    }
}

TEST_CASE("Transient shaping does not depend on the block size", "[dsp][transient]")
{
    const Stereo in = hits(2, 5.0);
    const CleanupParams p = shaper(5.0f, -4.0f);
    const Stereo a = run(p, in, 64);
    const Stereo b = run(p, in, 1000);
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a.left[i] == b.left[i]);
        REQUIRE(a.right[i] == b.right[i]);
    }
}

TEST_CASE("Switching the transient shaper off fades its gain out", "[dsp][transient]")
{
    CleanupChain chain;
    chain.prepare(kRate);
    chain.setParams(shaper(12.0f, 0.0f));
    Stereo x = hits(1, 5.0, true);
    // Switch off while the sharpened rise is heard.
    const std::size_t stop = kFirstHit + ms(1) + static_cast<std::size_t>(chain.latency());
    chain.process(x.left.data(), x.right.data(), static_cast<int>(stop));
    CleanupParams off = shaper(12.0f, 0.0f);
    off.transient = false;
    off.dcRemoval = true; // keep the chain running
    chain.setParams(off);
    chain.process(x.left.data() + stop, x.right.data() + stop, static_cast<int>(x.size() - stop));
    // A 1 kHz tone at amplitude 0.5 moves at most 0.066 per sample; dropping
    // a 12 dB gain at once would jump far more.
    float worst = 0.0f;
    for (std::size_t i = stop - 10; i < stop + 200; ++i)
        worst = std::max(worst, std::abs(x.left[i + 1] - x.left[i]));
    CHECK(worst < 0.1f);
}

TEST_CASE("The transient shaper never touches the heap", "[dsp][transient][rt]")
{
    CleanupChain chain;
    chain.prepare(kRate);
    Stereo x = hits(1, 5.0);
    {
        test::AllocationGuard guard;
        chain.setParams(shaper(6.0f, -3.0f));
        chain.process(x.left.data(), x.right.data(), static_cast<int>(x.size()));
        chain.setParams(shaper(0.0f, 0.0f));
        chain.reset();
        CHECK(guard.allocations() == 0);
    }
}
