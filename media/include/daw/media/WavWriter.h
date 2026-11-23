// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Dependency-free RIFF/WAVE writer used for test fixtures, renders and
// exports. Optionally embeds music metadata (RIFF INFO + ID3v2.3, see
// AudioTags.h) and TPDF-dithers when reducing to PCM. Writes atomically.
//
// Files over 4 GB are written as RF64 (EBU Tech 3306): the same layout with
// "RF64" in place of "RIFF", a "ds64" chunk holding the 64-bit sizes, and
// 0xFFFFFFFF in the 32-bit size fields.

#include <daw/core/AudioAsset.h>
#include <daw/core/Result.h>
#include <daw/media/AudioTags.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace daw {

enum class WavSampleFormat { Pcm16, Pcm24, Float32 };

enum class WavContainer {
    Auto, // RIFF, or RF64 when the file would exceed 4 GB
    Riff,
    Rf64, // always RF64 (for tools that expect it, and for tests)
};

struct WavEncodeOptions {
    WavSampleFormat format = WavSampleFormat::Pcm24;
    /// Triangular (TPDF) dither of +-1 LSB before rounding to PCM. Recommended
    /// for 16-bit exports; ignored for Float32.
    bool dither = false;
    std::uint32_t ditherSeed = 0x2545F491u; // deterministic output for a given seed
    const AudioTags* tags = nullptr;        // LIST/INFO before the data, id3 after it
    WavContainer container = WavContainer::Auto;
};

/// Encodes the asset as an in-memory WAV file image. Samples beyond +-1.0
/// are clipped for PCM formats (Float32 keeps them).
std::string encodeWav(const AudioAsset& asset, const WavEncodeOptions& options);
std::string encodeWav(const AudioAsset& asset, WavSampleFormat format);

/// Whether encodeWav writes RF64 for these sizes: forced, or a RIFF size field
/// would overflow 32 bits (`Riff` then fails in writeWavFile).
bool usesRf64(WavContainer container, std::uint64_t riffSize) noexcept;

Status writeWavFile(const std::filesystem::path& path, const AudioAsset& asset, const WavEncodeOptions& options);
Status writeWavFile(const std::filesystem::path& path, const AudioAsset& asset,
                    WavSampleFormat format = WavSampleFormat::Pcm24);

} // namespace daw
