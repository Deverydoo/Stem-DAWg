// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/PeakPyramid.h>

#include <algorithm>
#include <limits>

namespace daw {

namespace {

std::int64_t ceilDiv(std::int64_t a, std::int64_t b)
{
    return (a + b - 1) / b;
}

} // namespace

Result<PeakPyramid> PeakPyramid::build(const AudioAsset& asset, const PeakPyramidParams& params,
                                       const CancellationToken& cancel)
{
    if (params.baseSamplesPerBucket <= 0 || params.factor < 2 || params.minBuckets < 1)
        return makeError(ErrorCode::InvalidArgument, "invalid peak pyramid parameters");
    if (asset.numChannels() <= 0 || asset.numFrames() <= 0)
        return makeError(ErrorCode::InvalidArgument, "cannot build peaks for empty audio");

    PeakPyramid pyramid;
    pyramid.numChannels_ = asset.numChannels();
    pyramid.numFrames_ = asset.numFrames();
    pyramid.sampleRate_ = asset.sampleRate;
    pyramid.params_ = params;

    // Level 0 from samples.
    PeakLevel base;
    base.samplesPerBucket = params.baseSamplesPerBucket;
    base.numBuckets = ceilDiv(asset.numFrames(), params.baseSamplesPerBucket);
    base.data.resize(static_cast<std::size_t>(base.numBuckets * asset.numChannels()));
    for (int c = 0; c < asset.numChannels(); ++c) {
        const float* samples = asset.channelData(c);
        PeakBucket* out = base.data.data() + static_cast<std::int64_t>(c) * base.numBuckets;
        for (std::int64_t b = 0; b < base.numBuckets; ++b) {
            if ((b & 0x3FF) == 0 && cancel.isCancelled())
                return makeError(ErrorCode::Cancelled, "peak build cancelled");
            const std::int64_t start = b * base.samplesPerBucket;
            const std::int64_t end = std::min(start + base.samplesPerBucket, asset.numFrames());
            float lo = std::numeric_limits<float>::max();
            float hi = std::numeric_limits<float>::lowest();
            for (std::int64_t i = start; i < end; ++i) {
                lo = std::min(lo, samples[i]);
                hi = std::max(hi, samples[i]);
            }
            out[b] = PeakBucket{lo, hi};
        }
    }
    pyramid.levels_.push_back(std::move(base));

    // Coarser levels by merging.
    while (pyramid.levels_.back().numBuckets > params.minBuckets) {
        const PeakLevel& prev = pyramid.levels_.back();
        PeakLevel next;
        next.samplesPerBucket = prev.samplesPerBucket * params.factor;
        next.numBuckets = ceilDiv(prev.numBuckets, params.factor);
        next.data.resize(static_cast<std::size_t>(next.numBuckets * asset.numChannels()));
        for (int c = 0; c < asset.numChannels(); ++c) {
            const PeakBucket* in = prev.channel(c);
            PeakBucket* out = next.data.data() + static_cast<std::int64_t>(c) * next.numBuckets;
            for (std::int64_t b = 0; b < next.numBuckets; ++b) {
                const std::int64_t first = b * params.factor;
                const std::int64_t last = std::min(first + params.factor, prev.numBuckets);
                PeakBucket merged = in[first];
                for (std::int64_t i = first + 1; i < last; ++i) {
                    merged.min = std::min(merged.min, in[i].min);
                    merged.max = std::max(merged.max, in[i].max);
                }
                out[b] = merged;
            }
        }
        pyramid.levels_.push_back(std::move(next));
    }
    return pyramid;
}

Result<PeakPyramid> PeakPyramid::fromLevels(int numChannels, std::int64_t numFrames, double sampleRate,
                                            const PeakPyramidParams& params, std::vector<PeakLevel> levels)
{
    if (numChannels <= 0 || numFrames <= 0 || levels.empty())
        return makeError(ErrorCode::Corrupt, "peak data has no channels, frames or levels");
    std::int64_t expectedSpb = params.baseSamplesPerBucket;
    for (const PeakLevel& level : levels) {
        if (level.samplesPerBucket != expectedSpb || level.numBuckets != ceilDiv(numFrames, expectedSpb)
            || level.data.size() != static_cast<std::size_t>(level.numBuckets * numChannels))
            return makeError(ErrorCode::Corrupt, "peak level shape mismatch");
        expectedSpb *= params.factor;
    }
    PeakPyramid pyramid;
    pyramid.numChannels_ = numChannels;
    pyramid.numFrames_ = numFrames;
    pyramid.sampleRate_ = sampleRate;
    pyramid.params_ = params;
    pyramid.levels_ = std::move(levels);
    return pyramid;
}

const PeakLevel& PeakPyramid::levelForResolution(double framesPerPixel) const
{
    const PeakLevel* chosen = &levels_.front();
    for (const PeakLevel& level : levels_) {
        if (static_cast<double>(level.samplesPerBucket) <= framesPerPixel)
            chosen = &level;
        else
            break;
    }
    return *chosen;
}

PeakBucket PeakPyramid::query(int channel, std::int64_t startFrame, std::int64_t endFrame) const
{
    if (levels_.empty())
        return {};
    return query(channel, startFrame, endFrame, levelForResolution(static_cast<double>(endFrame - startFrame)));
}

PeakBucket PeakPyramid::query(int channel, std::int64_t startFrame, std::int64_t endFrame, const PeakLevel& level) const
{
    startFrame = std::max<std::int64_t>(startFrame, 0);
    endFrame = std::min(endFrame, numFrames_);
    if (channel < 0 || channel >= numChannels_ || endFrame <= startFrame || level.numBuckets == 0)
        return {};
    const std::int64_t first = startFrame / level.samplesPerBucket;
    const std::int64_t last = std::min((endFrame - 1) / level.samplesPerBucket, level.numBuckets - 1);
    const PeakBucket* data = level.channel(channel);
    PeakBucket result = data[first];
    for (std::int64_t b = first + 1; b <= last; ++b) {
        result.min = std::min(result.min, data[b].min);
        result.max = std::max(result.max, data[b].max);
    }
    return result;
}

} // namespace daw
