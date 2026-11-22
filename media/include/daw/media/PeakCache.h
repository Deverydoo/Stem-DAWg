// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Binary on-disk cache for PeakPyramids (Cache/waveforms/<mediaId>.peaks).
//
// Layout (little-endian), version 1:
//   char[4]  magic "IDPK"
//   u32      format version (1)
//   u64      source key (see computeSourceKey)
//   f64      sample rate
//   u32      channels
//   i64      frames
//   i64      base samples per bucket
//   u32      factor
//   i64      min buckets
//   u32      level count
//   per level: i64 samplesPerBucket, i64 numBuckets
//   per level: numBuckets*channels pairs of f32 (min, max), channel-major
//   u64      FNV-1a 64 checksum of every preceding byte
//
// A cache is rejected (and rebuilt by the caller) when the magic/version is
// unknown, the checksum fails, the file is truncated, or the source key no
// longer matches the media file.

#include <daw/core/Result.h>
#include <daw/media/PeakPyramid.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace daw {

inline constexpr std::uint32_t kPeakCacheVersion = 1;

/// Cheap identity of a media file: hash of filename, size and last-write time.
Result<std::uint64_t> computeSourceKey(const std::filesystem::path& mediaFile);

std::string encodePeakCache(const PeakPyramid& pyramid, std::uint64_t sourceKey);
Result<PeakPyramid> decodePeakCache(std::string_view bytes, std::uint64_t expectedSourceKey);

Status writePeakCache(const std::filesystem::path& file, const PeakPyramid& pyramid, std::uint64_t sourceKey);
Result<PeakPyramid> readPeakCache(const std::filesystem::path& file, std::uint64_t expectedSourceKey);

} // namespace daw
