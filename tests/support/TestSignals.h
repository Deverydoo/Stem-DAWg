// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <daw/core/AudioAsset.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace daw::test {

std::shared_ptr<AudioAsset> makeSine(double frequency, double seconds, double sampleRate, int channels = 2,
                                     float amplitude = 0.5f);

/// Deterministic, sample-unique signal: channel c, frame i = ((i % 9973) / 9973) * (c == 0 ? 1 : -1) * 0.9.
std::shared_ptr<AudioAsset> makeRamp(std::int64_t frames, double sampleRate, int channels = 2);

std::shared_ptr<AudioAsset> makeSilence(std::int64_t frames, double sampleRate, int channels = 2);

/// Deterministic pseudo-random noise in [-amplitude, amplitude].
std::shared_ptr<AudioAsset> makeNoise(std::int64_t frames, double sampleRate, int channels, float amplitude,
                                      std::uint32_t seed = 1);

/// Low noise floor with short decaying noise bursts starting at `onsetSeconds`.
std::shared_ptr<AudioAsset> makeClickTrack(double seconds, double sampleRate, const std::vector<double>& onsetSeconds,
                                           float burstAmplitude = 0.8f, float noiseFloor = 1e-4f);

/// Suno-like drum stem with bleed: a sustained 55/82.5 Hz drone (peak ~0.28)
/// under pitched-down kick hits with a noisy attack, one hit every `interval`
/// seconds starting at 0. Mono or stereo.
std::shared_ptr<AudioAsset> makeDroneWithKicks(double seconds, double sampleRate, double interval, int channels = 2,
                                               std::uint32_t seed = 3);

/// Goertzel power of `frequency` in samples [start, start + count).
double goertzelPower(const float* samples, std::int64_t count, double frequency, double sampleRate);

} // namespace daw::test
