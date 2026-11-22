// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/Files.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/WavWriter.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

using namespace daw;
using namespace daw::test;

TEST_CASE("WAV files round-trip through the decoder", "[media][decode]")
{
    TempDir dir("decode");
    const auto [format, tolerance] = GENERATE(table<WavSampleFormat, float>({
        {WavSampleFormat::Pcm16, 1.0f / 16000.0f},
        {WavSampleFormat::Pcm24, 1.0f / 4000000.0f},
        {WavSampleFormat::Float32, 0.0f},
    }));
    const int channels = GENERATE(1, 2);

    const auto source = makeSine(997.0, 0.5, 44100.0, channels, 0.8f);
    const auto path = dir / "tone.wav";
    REQUIRE(writeWavFile(path, *source, format).ok());

    const auto info = probeAudioFile(path);
    REQUIRE(info.ok());
    REQUIRE(info->sampleRate == 44100.0);
    REQUIRE(info->numChannels == channels);
    REQUIRE(info->numFrames == source->numFrames());

    float lastProgress = 0.0f;
    const auto decoded = decodeAudioFile(path, {}, {}, [&](float p) { lastProgress = p; });
    REQUIRE(decoded.ok());
    const auto& asset = **decoded;
    REQUIRE(asset.numChannels() == channels);
    REQUIRE(asset.numFrames() == source->numFrames());
    REQUIRE(asset.sampleRate == 44100.0);
    REQUIRE(lastProgress == 1.0f);

    float maxError = 0.0f;
    for (int c = 0; c < channels; ++c)
        for (std::int64_t i = 0; i < asset.numFrames(); ++i)
            maxError = std::max(maxError, std::abs(asset.channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]
                                                   - source->channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)]));
    REQUIRE(maxError <= tolerance);
}

TEST_CASE("Decoder reports missing, unsupported and over-limit files", "[media][decode]")
{
    TempDir dir("decode-errors");
    REQUIRE(decodeAudioFile(dir / "missing.wav").error().code == ErrorCode::NotFound);

    const auto bogus = dir / "bogus.wav";
    REQUIRE(writeFileAtomically(bogus, "this is not audio").ok());
    REQUIRE(decodeAudioFile(bogus).error().code == ErrorCode::UnsupportedFormat);

    const auto path = dir / "long.wav";
    REQUIRE(writeWavFile(path, *makeSilence(48000, 48000.0, 2)).ok());
    DecodeOptions limits;
    limits.maxFrames = 1000;
    REQUIRE(decodeAudioFile(path, limits).error().code == ErrorCode::LimitExceeded);
    limits = {};
    limits.maxChannels = 1;
    REQUIRE(decodeAudioFile(path, limits).error().code == ErrorCode::LimitExceeded);
}

TEST_CASE("Decoder honours cancellation", "[media][decode]")
{
    TempDir dir("decode-cancel");
    const auto path = dir / "a.wav";
    REQUIRE(writeWavFile(path, *makeSilence(200000, 48000.0, 2)).ok());
    CancellationSource cancel;
    cancel.cancel();
    REQUIRE(decodeAudioFile(path, {}, cancel.token()).error().code == ErrorCode::Cancelled);
}

TEST_CASE("Decoder opens files in Unicode directories", "[media][decode]")
{
    TempDir dir("decode-unicode");
    const auto path = dir / utf8ToPath("Stems — Ünïcødé") / utf8ToPath("drums ✓.wav");
    REQUIRE(writeWavFile(path, *makeRamp(1000, 48000.0)).ok());
    REQUIRE(decodeAudioFile(path).ok());
}
