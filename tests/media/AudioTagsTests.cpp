// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/Files.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/AudioTags.h>
#include <daw/media/WavWriter.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>

using namespace daw;
using namespace daw::test;

namespace {

// A tiny valid-looking PNG header followed by payload; enough for sniffing.
std::string fakePng()
{
    std::string png("\x89PNG\r\n\x1A\n", 8);
    for (int i = 0; i < 3001; ++i) // odd size exercises chunk padding
        png.push_back(static_cast<char>(i * 7));
    return png;
}

AudioTags sampleTags(bool unicode)
{
    AudioTags t;
    t.title = unicode ? "T\xC3\xABst S\xC3\xB8ng \xE2\x9C\x93" : "Test Song";
    t.artist = unicode ? "\xC3\x9Cn\xC3\xAF Artist" : "Example Artist";
    t.albumArtist = t.artist;
    t.album = "Rust Belt Liturgy";
    t.trackNumber = "3/12";
    t.year = "2026";
    t.genre = "Industrial";
    t.composer = "A. Composer";
    t.publisher = "Example Records";
    t.copyright = "\xC2\xA9 2026 Example Artist";
    t.comment = unicode ? "Cleaned with care \xF0\x9F\x8E\xB8" : "Cleaned with care";
    t.isrc = "QZXXX2600001";
    t.url = "https://example.com/artist";
    t.software = "Stem DAWg 0.1.0";
    t.artwork = fakePng();
    t.artworkMimeType = "image/png";
    return t;
}

} // namespace

TEST_CASE("WAV metadata round-trips through RIFF INFO and ID3v2.3", "[media][tags]")
{
    for (bool unicode : {false, true}) {
        const AudioTags tags = sampleTags(unicode);
        const auto audio = makeSine(440.0, 0.1, 48000.0);
        WavEncodeOptions options;
        options.tags = &tags;
        const std::string wav = encodeWav(*audio, options);

        const auto read = readWavTags(wav);
        REQUIRE(read.ok());
        REQUIRE(*read == tags);

        // Both containers are present and the RIFF size is consistent.
        REQUIRE(wav.find("LIST") != std::string::npos);
        REQUIRE(wav.find("INFO") != std::string::npos);
        REQUIRE(wav.find("id3 ") != std::string::npos);
        const auto riffSize = static_cast<std::uint32_t>(static_cast<unsigned char>(wav[4]))
                              | static_cast<std::uint32_t>(static_cast<unsigned char>(wav[5])) << 8
                              | static_cast<std::uint32_t>(static_cast<unsigned char>(wav[6])) << 16
                              | static_cast<std::uint32_t>(static_cast<unsigned char>(wav[7])) << 24;
        REQUIRE(riffSize + 8 == wav.size());
        REQUIRE(wav.size() % 2 == 0);
    }
}

TEST_CASE("RIFF INFO alone is readable and ID3 copyright omits the symbol", "[media][tags]")
{
    AudioTags tags;
    tags.title = "Only Title";
    tags.copyright = "\xC2\xA9 2026 Example Artist";
    const std::string info = encodeRiffInfoChunk(tags);
    REQUIRE(info.substr(0, 4) == "LIST");
    REQUIRE(info.size() % 2 == 0);

    const std::string id3 = encodeId3v23(tags);
    REQUIRE(id3.substr(0, 3) == "ID3");
    REQUIRE(id3.find("2026 Example Artist") != std::string::npos);
    REQUIRE(id3.find("\xC2\xA9") == std::string::npos); // TCOP starts with the year

    REQUIRE(encodeRiffInfoChunk(AudioTags{}).empty());
    REQUIRE(encodeId3Chunk(AudioTags{}).empty());
    REQUIRE(AudioTags{}.empty());
    REQUIRE_FALSE(tags.empty());
}

TEST_CASE("Tagged WAV files decode to the same audio", "[media][tags][decode]")
{
    TempDir dir("tags");
    const AudioTags tags = sampleTags(true);
    const auto audio = makeSine(997.0, 0.25, 44100.0, 2, 0.5f);
    WavEncodeOptions options;
    options.format = WavSampleFormat::Float32;
    options.tags = &tags;
    const auto path = dir / "tagged.wav";
    REQUIRE(writeWavFile(path, *audio, options).ok());

    const auto decoded = decodeAudioFile(path);
    REQUIRE(decoded.ok());
    REQUIRE((*decoded)->numFrames() == audio->numFrames());
    for (int c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < audio->channels[0].size(); ++i)
            REQUIRE((*decoded)->channels[static_cast<std::size_t>(c)][i] == audio->channels[static_cast<std::size_t>(c)][i]);

    const auto bytes = readFileBytes(path);
    REQUIRE(bytes.ok());
    REQUIRE(readWavTags(*bytes)->title == tags.title);
}

TEST_CASE("16-bit export uses TPDF dither: deterministic, unbiased, at most 1 LSB", "[media][dither]")
{
    auto silence = makeSilence(48000, 48000.0, 1);
    WavEncodeOptions options;
    options.format = WavSampleFormat::Pcm16;
    options.dither = true;
    const std::string a = encodeWav(*silence, options);
    const std::string b = encodeWav(*silence, options);
    REQUIRE(a == b);

    const std::size_t data = a.find("data") + 8;
    std::set<int> values;
    double sum = 0.0;
    for (std::size_t i = data; i + 1 < a.size(); i += 2) {
        const auto v = static_cast<std::int16_t>(static_cast<unsigned char>(a[i]) | (static_cast<unsigned char>(a[i + 1]) << 8));
        values.insert(v);
        sum += v;
    }
    REQUIRE(values.size() > 1); // dither present
    for (int v : values)
        REQUIRE(std::abs(v) <= 1);
    REQUIRE(std::abs(sum / 48000.0) < 0.02);

    options.dither = false;
    const std::string plain = encodeWav(*silence, options);
    for (std::size_t i = data; i < plain.size(); ++i)
        REQUIRE(plain[i] == '\0');
}

TEST_CASE("Image type sniffing", "[media][tags]")
{
    REQUIRE(sniffImageMimeType(fakePng()) == "image/png");
    REQUIRE(sniffImageMimeType(std::string("\xFF\xD8\xFF\xE0", 4)) == "image/jpeg");
    REQUIRE(sniffImageMimeType("GIF89a").empty());
    REQUIRE_FALSE(readWavTags("not a wav").ok());
}
