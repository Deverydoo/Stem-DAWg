// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Multi-resolution min/max peak data for waveform drawing.
//
// Level 0 holds one min/max pair per `baseSamplesPerBucket` frames (default
// 256). Each further level merges `factor` (default 4) buckets of the level
// below, until a level has <= `minBuckets` buckets. Drawing a pixel column
// picks the coarsest level whose bucket size does not exceed the column's
// frame span, so cost is independent of zoom level and file length.

#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

#include <cstdint>
#include <vector>

namespace daw {

struct PeakBucket {
    float min = 0.0f;
    float max = 0.0f;

    friend bool operator==(const PeakBucket&, const PeakBucket&) = default;
};

struct PeakLevel {
    std::int64_t samplesPerBucket = 0;
    std::int64_t numBuckets = 0;
    std::vector<PeakBucket> data; // channel-major: data[channel * numBuckets + bucket]

    const PeakBucket* channel(int index) const noexcept { return data.data() + static_cast<std::int64_t>(index) * numBuckets; }

    friend bool operator==(const PeakLevel&, const PeakLevel&) = default;
};

struct PeakPyramidParams {
    std::int64_t baseSamplesPerBucket = 256;
    int factor = 4;
    std::int64_t minBuckets = 16;

    friend bool operator==(const PeakPyramidParams&, const PeakPyramidParams&) = default;
};

class PeakPyramid {
public:
    PeakPyramid() = default;

    static Result<PeakPyramid> build(const AudioAsset& asset, const PeakPyramidParams& params = {},
                                     const CancellationToken& cancel = {});

    /// Reassembles a pyramid from stored levels (used by the peak cache). Validates shape.
    static Result<PeakPyramid> fromLevels(int numChannels, std::int64_t numFrames, double sampleRate,
                                          const PeakPyramidParams& params, std::vector<PeakLevel> levels);

    int numChannels() const noexcept { return numChannels_; }
    std::int64_t numFrames() const noexcept { return numFrames_; }
    double sampleRate() const noexcept { return sampleRate_; }
    const PeakPyramidParams& params() const noexcept { return params_; }
    const std::vector<PeakLevel>& levels() const noexcept { return levels_; }
    bool empty() const noexcept { return levels_.empty(); }

    /// Coarsest level with samplesPerBucket <= framesPerPixel (finest if none).
    const PeakLevel& levelForResolution(double framesPerPixel) const;

    /// Min/max envelope of frames [startFrame, endFrame) using the level chosen
    /// for the span. Exact when both ends lie on bucket boundaries of that
    /// level, otherwise conservative (never narrower than the true envelope).
    PeakBucket query(int channel, std::int64_t startFrame, std::int64_t endFrame) const;
    PeakBucket query(int channel, std::int64_t startFrame, std::int64_t endFrame, const PeakLevel& level) const;

    friend bool operator==(const PeakPyramid&, const PeakPyramid&) = default;

private:
    int numChannels_ = 0;
    std::int64_t numFrames_ = 0;
    double sampleRate_ = 0.0;
    PeakPyramidParams params_;
    std::vector<PeakLevel> levels_;
};

} // namespace daw
