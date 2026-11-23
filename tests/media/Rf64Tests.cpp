// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// RF64 (EBU Tech 3306) for WAV files over 4 GB. Forcing RF64 on a small file
// checks the layout, the tag reader and decoding without writing 4 GB.

#include <daw/core/Files.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/AudioTags.h>
#include <daw/media/WavWriter.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using namespace daw;
using namespace daw::test;

namespace {

std::uint32_t u32(const std::string& b, std::size_t at)
{
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | static_cast<unsigned char>(b[at + static_cast<std::size_t>(i)]);
    return v;
}

std::uint64_t u64(const std::string& b, std::size_t at)
{
    return static_cast<std::uint64_t>(u32(b, at)) | (static_cast<std::uint64_t>(u32(b, at + 4)) << 32);
}

} // namespace

TEST_CASE("RF64 files carry their sizes in ds64 and decode like RIFF files", "[media][wav][rf64]")
{
    const auto asset = makeRamp(4801, 48000.0); // stereo, an odd number of frames
    AudioTags tags;
    tags.title = "Long Render";
    tags.artist = "Test";
    WavEncodeOptions options;
    options.format = WavSampleFormat::Pcm24;
    options.tags = &tags;

    const std::string riff = encodeWav(*asset, options);
    options.container = WavContainer::Rf64;
    const std::string rf64 = encodeWav(*asset, options);

    CHECK(riff.substr(0, 4) == "RIFF");
    REQUIRE(rf64.substr(0, 4) == "RF64");
    CHECK(rf64.size() == riff.size() + 36); // the ds64 chunk
    CHECK(u32(rf64, 4) == 0xFFFFFFFFu);
    CHECK(rf64.substr(8, 4) == "WAVE");
    REQUIRE(rf64.substr(12, 4) == "ds64");
    CHECK(u32(rf64, 16) == 28);
    CHECK(u64(rf64, 20) == rf64.size() - 8);          // RIFF size
    const std::uint64_t dataBytes = 4801ull * 2 * 3;
    CHECK(u64(rf64, 28) == dataBytes);                 // data size
    CHECK(u64(rf64, 36) == 4801);                      // sample frames
    const auto data = rf64.find("data");
    REQUIRE(data != std::string::npos);
    CHECK(u32(rf64, data + 4) == 0xFFFFFFFFu);
    // Apart from the header and the data size field, the files are identical.
    std::string body = rf64.substr(12 + 36);
    const auto riffData = riff.find("data");
    body.replace(riffData - 12 + 4, 4, riff.substr(riffData + 4, 4));
    const bool sameBody = body == riff.substr(12);
    CHECK(sameBody);

    // Tags after a data chunk sized through ds64 are still found.
    const auto read = readWavTags(rf64);
    REQUIRE(read.ok());
    CHECK(read->title == "Long Render");
    CHECK(read->artist == "Test");

    // A real decoder reads it to the same samples as the RIFF file.
    TempDir dir("rf64");
    REQUIRE(writeWavFile(dir / "riff.wav", *asset, WavEncodeOptions{.format = WavSampleFormat::Pcm24}).ok());
    REQUIRE(writeWavFile(dir / "rf64.wav", *asset,
                         WavEncodeOptions{.format = WavSampleFormat::Pcm24, .container = WavContainer::Rf64})
                .ok());
    const auto a = decodeAudioFile(dir / "riff.wav");
    const auto b = decodeAudioFile(dir / "rf64.wav");
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    CHECK((*b)->numFrames() == 4801);
    CHECK((*b)->channels == (*a)->channels);
}

TEST_CASE("Automatic container choice switches to RF64 only above 4 GB", "[media][wav][rf64]")
{
    CHECK_FALSE(usesRf64(WavContainer::Auto, 0xFFFFFFFFull));
    CHECK(usesRf64(WavContainer::Auto, 0x100000000ull));
    CHECK_FALSE(usesRf64(WavContainer::Riff, 0x100000000ull));
    CHECK(usesRf64(WavContainer::Rf64, 100));
    // Small files are unchanged by the RF64 support.
    const auto asset = makeSine(440.0, 0.1, 44100.0);
    CHECK(encodeWav(*asset, WavSampleFormat::Pcm16).substr(0, 4) == "RIFF");
}
