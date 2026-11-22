// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/WavWriter.h>

#include <daw/core/Files.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace daw {

namespace {

void putU16(std::string& out, std::uint16_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void putU32(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void putTag(std::string& out, const char* tag)
{
    out.append(tag, 4);
}

/// xorshift32: small, fast, deterministic.
struct DitherSource {
    std::uint32_t state;
    float next() noexcept // uniform in [-0.5, 0.5)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state) * (1.0f / 4294967296.0f) - 0.5f;
    }
    float tpdf() noexcept { return next() + next(); } // triangular in (-1, 1) LSB
};

} // namespace

std::string encodeWav(const AudioAsset& asset, const WavEncodeOptions& options)
{
    const WavSampleFormat format = options.format;
    const int channels = asset.numChannels();
    const auto frames = static_cast<std::uint64_t>(asset.numFrames());
    const std::uint16_t bits = format == WavSampleFormat::Pcm16 ? 16 : format == WavSampleFormat::Pcm24 ? 24 : 32;
    const std::uint16_t formatTag = format == WavSampleFormat::Float32 ? 3 : 1;
    const std::uint32_t bytesPerSample = bits / 8u;
    const std::uint32_t blockAlign = bytesPerSample * static_cast<std::uint32_t>(channels);
    const std::uint64_t dataBytes = frames * blockAlign;
    const auto sampleRate = static_cast<std::uint32_t>(std::lround(asset.sampleRate));

    const std::string info = options.tags != nullptr ? encodeRiffInfoChunk(*options.tags) : std::string();
    const std::string id3 = options.tags != nullptr ? encodeId3Chunk(*options.tags) : std::string();
    const std::uint64_t dataPad = dataBytes & 1;
    const std::uint64_t riffSize = 4 + (8 + 16) + info.size() + (8 + dataBytes + dataPad) + id3.size();

    std::string out;
    out.reserve(static_cast<std::size_t>(8 + riffSize));
    putTag(out, "RIFF");
    putU32(out, static_cast<std::uint32_t>(riffSize));
    putTag(out, "WAVE");
    putTag(out, "fmt ");
    putU32(out, 16);
    putU16(out, formatTag);
    putU16(out, static_cast<std::uint16_t>(channels));
    putU32(out, sampleRate);
    putU32(out, sampleRate * blockAlign);
    putU16(out, static_cast<std::uint16_t>(blockAlign));
    putU16(out, bits);
    out += info;
    putTag(out, "data");
    putU32(out, static_cast<std::uint32_t>(dataBytes));

    const std::size_t dataStart = out.size();
    out.resize(dataStart + static_cast<std::size_t>(dataBytes));
    auto* dst = reinterpret_cast<unsigned char*>(out.data() + dataStart);
    DitherSource dither{options.ditherSeed != 0 ? options.ditherSeed : 1u};
    const bool useDither = options.dither && format != WavSampleFormat::Float32;

    for (std::uint64_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            const float sample = asset.channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)];
            switch (format) {
            case WavSampleFormat::Pcm16: {
                float scaled = sample * 32767.0f + (useDither ? dither.tpdf() : 0.0f);
                const auto v = static_cast<std::int32_t>(std::lrint(std::clamp(scaled, -32768.0f, 32767.0f)));
                *dst++ = static_cast<unsigned char>(v & 0xFF);
                *dst++ = static_cast<unsigned char>((v >> 8) & 0xFF);
                break;
            }
            case WavSampleFormat::Pcm24: {
                float scaled = sample * 8388607.0f + (useDither ? dither.tpdf() : 0.0f);
                const auto v = static_cast<std::int32_t>(std::lrint(std::clamp(scaled, -8388608.0f, 8388607.0f)));
                *dst++ = static_cast<unsigned char>(v & 0xFF);
                *dst++ = static_cast<unsigned char>((v >> 8) & 0xFF);
                *dst++ = static_cast<unsigned char>((v >> 16) & 0xFF);
                break;
            }
            case WavSampleFormat::Float32: {
                std::uint32_t bitsValue = 0;
                std::memcpy(&bitsValue, &sample, sizeof(bitsValue));
                for (int b = 0; b < 4; ++b)
                    *dst++ = static_cast<unsigned char>((bitsValue >> (8 * b)) & 0xFF);
                break;
            }
            }
        }
    }
    if (dataPad)
        out.push_back('\0');
    out += id3;
    return out;
}

std::string encodeWav(const AudioAsset& asset, WavSampleFormat format)
{
    WavEncodeOptions options;
    options.format = format;
    return encodeWav(asset, options);
}

Status writeWavFile(const std::filesystem::path& path, const AudioAsset& asset, const WavEncodeOptions& options)
{
    if (asset.numChannels() <= 0 || asset.sampleRate <= 0.0)
        return makeError(ErrorCode::InvalidArgument, "cannot write WAV: asset has no channels or sample rate");
    const std::uint64_t dataBytes = static_cast<std::uint64_t>(asset.numFrames()) * static_cast<std::uint64_t>(asset.numChannels())
                                    * (options.format == WavSampleFormat::Pcm16 ? 2u : options.format == WavSampleFormat::Pcm24 ? 3u : 4u);
    if (dataBytes > 0xFFFFFFFFull - (1u << 26))
        return makeError(ErrorCode::InvalidArgument, "render is too long for a WAV file (4 GB limit); RF64 export is planned");
    return writeFileAtomically(path, encodeWav(asset, options));
}

Status writeWavFile(const std::filesystem::path& path, const AudioAsset& asset, WavSampleFormat format)
{
    WavEncodeOptions options;
    options.format = format;
    return writeWavFile(path, asset, options);
}

} // namespace daw
