// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "TestSignals.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw::test {

namespace {
struct Lcg {
    std::uint32_t state;
    float next() // [-1, 1)
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / static_cast<float>(1u << 23) - 1.0f;
    }
};
} // namespace

std::shared_ptr<AudioAsset> makeSine(double frequency, double seconds, double sampleRate, int channels, float amplitude)
{
    const auto frames = static_cast<std::int64_t>(std::llround(seconds * sampleRate));
    auto asset = AudioAsset::create(channels, frames, sampleRate);
    for (int c = 0; c < channels; ++c)
        for (std::int64_t i = 0; i < frames; ++i)
            asset->channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] =
                amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i) / sampleRate));
    return asset;
}

std::shared_ptr<AudioAsset> makeRamp(std::int64_t frames, double sampleRate, int channels)
{
    auto asset = AudioAsset::create(channels, frames, sampleRate);
    for (int c = 0; c < channels; ++c)
        for (std::int64_t i = 0; i < frames; ++i)
            asset->channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] =
                static_cast<float>(i % 9973) / 9973.0f * (c == 0 ? 0.9f : -0.9f);
    return asset;
}

std::shared_ptr<AudioAsset> makeSilence(std::int64_t frames, double sampleRate, int channels)
{
    return AudioAsset::create(channels, frames, sampleRate);
}

std::shared_ptr<AudioAsset> makeNoise(std::int64_t frames, double sampleRate, int channels, float amplitude, std::uint32_t seed)
{
    auto asset = AudioAsset::create(channels, frames, sampleRate);
    Lcg rng{seed};
    for (int c = 0; c < channels; ++c)
        for (std::int64_t i = 0; i < frames; ++i)
            asset->channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = amplitude * rng.next();
    return asset;
}

std::shared_ptr<AudioAsset> makeClickTrack(double seconds, double sampleRate, const std::vector<double>& onsetSeconds,
                                           float burstAmplitude, float noiseFloor)
{
    auto asset = makeNoise(static_cast<std::int64_t>(std::llround(seconds * sampleRate)), sampleRate, 1, noiseFloor, 7);
    auto& x = asset->channels[0];
    Lcg rng{12345};
    const auto burstLength = static_cast<std::int64_t>(0.010 * sampleRate);
    for (double onset : onsetSeconds) {
        const auto start = static_cast<std::int64_t>(std::llround(onset * sampleRate));
        for (std::int64_t i = 0; i < burstLength && start + i < static_cast<std::int64_t>(x.size()); ++i) {
            const float envelope = std::exp(-5.0f * static_cast<float>(i) / static_cast<float>(burstLength));
            x[static_cast<std::size_t>(start + i)] += burstAmplitude * envelope * rng.next();
        }
    }
    return asset;
}

std::shared_ptr<AudioAsset> makeDroneWithKicks(double seconds, double sampleRate, double interval, int channels,
                                               std::uint32_t seed)
{
    const auto frames = static_cast<std::int64_t>(std::llround(seconds * sampleRate));
    const auto period = static_cast<std::int64_t>(std::llround(interval * sampleRate));
    auto asset = AudioAsset::create(channels, frames, sampleRate);
    Lcg rng{seed};
    for (std::int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double drone = 0.18 * std::sin(2.0 * std::numbers::pi * 55.0 * t)
                             + 0.10 * std::sin(2.0 * std::numbers::pi * 82.5 * t + 0.3);
        const std::int64_t k = i % period;
        double hit = 0.0;
        if (k < 4000) {
            const double kk = static_cast<double>(k);
            const double envelope = std::exp(-kk / 600.0);
            const double pitch = 60.0 + 120.0 * std::exp(-kk / 300.0);
            hit = envelope * (0.6 * std::sin(2.0 * std::numbers::pi * pitch * kk / sampleRate) + 0.25 * rng.next());
        }
        for (int c = 0; c < channels; ++c) {
            const double v = (c == 0 ? drone : 0.9 * drone) + hit;
            asset->channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = static_cast<float>(std::clamp(v, -1.0, 1.0));
        }
    }
    return asset;
}

double goertzelPower(const float* samples, std::int64_t count, double frequency, double sampleRate)
{
    const double w = 2.0 * std::numbers::pi * frequency / sampleRate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::int64_t i = 0; i < count; ++i) {
        const double s = samples[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return (s1 * s1 + s2 * s2 - coeff * s1 * s2) / (static_cast<double>(count) * static_cast<double>(count));
}

} // namespace daw::test
