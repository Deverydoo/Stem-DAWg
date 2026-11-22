// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/Fft.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace daw;

TEST_CASE("FFT of an impulse is flat", "[ai][fft]")
{
    const Fft fft(64);
    std::vector<float> input(64, 0.0f), power;
    std::vector<std::complex<float>> scratch;
    input[0] = 1.0f;
    fft.powerSpectrum(input.data(), power, scratch);
    REQUIRE(power.size() == 33);
    for (float p : power)
        REQUIRE(std::abs(p - 1.0f) < 1e-5f);
}

TEST_CASE("FFT places a bin-centred sine in its bin", "[ai][fft]")
{
    constexpr int n = 1024;
    const Fft fft(n);
    std::vector<float> input(n), power;
    std::vector<std::complex<float>> scratch;
    for (int i = 0; i < n; ++i)
        input[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 37.0 * i / n));
    fft.powerSpectrum(input.data(), power, scratch);
    const auto peak = std::max_element(power.begin(), power.end()) - power.begin();
    REQUIRE(peak == 37);
    REQUIRE(std::abs(power[37] - (n / 2.0f) * (n / 2.0f)) / power[37] < 1e-3f);
}

TEST_CASE("FFT rejects non power-of-two sizes", "[ai][fft]")
{
    REQUIRE_THROWS(Fft(1000));
}
