// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/Resampler.h>

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

using namespace daw;
using namespace daw::test;

TEST_CASE("Resampler preserves duration, frequency and amplitude", "[media][resample]")
{
    const auto [fromRate, toRate] = GENERATE(table<double, double>({{44100.0, 48000.0}, {48000.0, 44100.0}, {48000.0, 96000.0}}));
    const auto input = makeSine(1000.0, 1.0, fromRate, 2, 0.5f);
    const auto output = resampleAudio(*input, toRate);
    REQUIRE(output.ok());
    const auto& out = **output;

    REQUIRE(out.sampleRate == toRate);
    REQUIRE(out.numChannels() == 2);
    REQUIRE(out.numFrames() == static_cast<std::int64_t>(std::llround(input->numFrames() * toRate / fromRate)));

    // Away from the edges the result is a clean 1 kHz sine of the same amplitude.
    const std::int64_t margin = static_cast<std::int64_t>(toRate * 0.05);
    const std::int64_t count = out.numFrames() - 2 * margin;
    const float* x = out.channelData(0) + margin;
    float peak = 0.0f;
    for (std::int64_t i = 0; i < count; ++i)
        peak = std::max(peak, std::abs(x[i]));
    REQUIRE(std::abs(peak - 0.5f) < 0.005f);

    const double signal = goertzelPower(x, count, 1000.0, toRate);
    const double offFrequency = goertzelPower(x, count, 1500.0, toRate);
    REQUIRE(signal > offFrequency * 1e4);

    // Sample-accurate timing: output frame n corresponds to input time n / toRate.
    const std::int64_t n = out.numFrames() / 2;
    const double expected = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(n) / toRate);
    REQUIRE(std::abs(out.channels[0][static_cast<std::size_t>(n)] - expected) < 0.002);
}

TEST_CASE("Resampler attenuates content above the new Nyquist when downsampling", "[media][resample]")
{
    const auto input = makeSine(23000.0, 0.5, 48000.0, 1, 0.5f); // above 22.05 kHz
    const auto output = resampleAudio(*input, 44100.0);
    REQUIRE(output.ok());
    float peak = 0.0f;
    const auto& x = (*output)->channels[0];
    for (std::size_t i = 4410; i < x.size() - 4410; ++i)
        peak = std::max(peak, std::abs(x[i]));
    REQUIRE(peak < 0.01f); // > 34 dB rejection of the alias
}

TEST_CASE("Resampler at identical rates copies and honours cancellation", "[media][resample]")
{
    const auto input = makeRamp(1000, 48000.0);
    const auto same = resampleAudio(*input, 48000.0);
    REQUIRE(same.ok());
    REQUIRE((*same)->channels == input->channels);

    CancellationSource cancel;
    cancel.cancel();
    REQUIRE(resampleAudio(*makeRamp(100000, 44100.0), 48000.0, {}, cancel.token()).error().code == ErrorCode::Cancelled);
    REQUIRE(resampleAudio(*input, 0.0).error().code == ErrorCode::InvalidArgument);
}
