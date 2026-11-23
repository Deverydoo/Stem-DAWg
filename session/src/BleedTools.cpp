// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/BleedTools.h>

#include "BlockWorkers.h"
#include "ProcessedMedia.h"

#include <daw/core/Files.h>
#include <daw/dsp/BleedSuppressor.h>
#include <daw/media/Midi.h>
#include <daw/media/Resampler.h>
#include <daw/session/VoiceTools.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace fs = std::filesystem;

namespace daw {

namespace {

/// One clip of a taking-part track, on the timeline (project frames).
struct Segment {
    std::int64_t start = 0;
    std::int64_t length = 0;
    std::int64_t sourceStart = 0;
    MediaId media; // the original
    const AudioAsset* audio = nullptr; // the original at the project rate
};

/// A processed copy being rendered: one per original a target track plays.
struct Unit {
    const MediaRef* original = nullptr;
    std::shared_ptr<AudioAsset> output; // starts as the original at the project rate
};

struct Participant {
    TrackId id;
    std::vector<Segment> segments;
    bool stereo = false;
    bool target = false;
    std::vector<Unit> units;
    std::vector<float> in[2];
    std::vector<float> out[2];
};

bool playsRecordedAudio(const ProjectModel& model, const Track& track)
{
    for (const Clip& clip : track.clips)
        if (const MediaRef* original = originalMediaOf(model, clip.media))
            if (!isMidiFile(utf8ToPath(original->path)))
                return true;
    return false;
}

/// Sums the participant's clips over timeline frames [from, from + count).
void fillInput(Participant& p, std::int64_t from, std::int64_t count)
{
    for (auto& channel : p.in)
        std::fill(channel.begin(), channel.end(), 0.0f);
    for (const Segment& seg : p.segments) {
        const std::int64_t begin = std::max(from, seg.start);
        const std::int64_t end = std::min(from + count, seg.start + seg.length);
        if (begin >= end)
            continue;
        const AudioAsset& audio = *seg.audio;
        const int channels = audio.numChannels();
        for (int c = 0; c < (p.stereo ? 2 : 1); ++c) {
            const float* src = audio.channelData(std::min(c, channels - 1));
            float* dst = p.in[c].data();
            for (std::int64_t t = begin; t < end; ++t) {
                const std::int64_t frame = seg.sourceStart + (t - seg.start);
                if (frame >= 0 && frame < audio.numFrames())
                    dst[t - from] += src[frame];
            }
        }
    }
}

/// Copies processed timeline frames [from, from + count) into the units'
/// media frames, within the clips and the processed span [spanStart, spanEnd).
void routeOutput(Participant& p, std::int64_t from, std::int64_t count, std::int64_t spanStart, std::int64_t spanEnd)
{
    for (Unit& unit : p.units) {
        AudioAsset& out = *unit.output;
        for (const Segment& seg : p.segments) {
            if (seg.media != unit.original->id)
                continue;
            const std::int64_t begin = std::max({from, seg.start, spanStart});
            const std::int64_t end = std::min({from + count, seg.start + seg.length, spanEnd});
            for (int c = 0; c < std::min(out.numChannels(), 2); ++c) {
                const float* src = p.out[p.stereo ? c : 0].data();
                float* dst = out.channels[static_cast<std::size_t>(c)].data();
                for (std::int64_t t = begin; t < end; ++t) {
                    const std::int64_t frame = seg.sourceStart + (t - seg.start);
                    if (frame >= 0 && frame < out.numFrames())
                        dst[frame] = src[t - from];
                }
            }
        }
    }
}

} // namespace

const BleedStrength& bleedStrengthFor(double reductionDb)
{
    const BleedStrength* best = &kBleedStrengths[0];
    for (const BleedStrength& s : kBleedStrengths)
        if (std::abs(s.reductionDb - reductionDb) < std::abs(best->reductionDb - reductionDb))
            best = &s;
    return *best;
}

std::vector<TrackId> bleedTracks(const ProjectModel& model)
{
    std::vector<TrackId> out;
    for (const Track& track : model.tracks)
        if (playsRecordedAudio(model, track))
            out.push_back(track.id);
    return out;
}

std::optional<MediaDerivation> trackBleedSuppression(const ProjectModel& model, const TrackId& track)
{
    auto processing = trackProcessing(model, track);
    if (processing && processing->process != kBleedSuppressProcess)
        return std::nullopt;
    return processing;
}

Result<std::vector<BleedTrackOutcome>> suppressBleed(const fs::path& projectRoot, const ProjectModel& model,
                                                     const OriginalAudioLookup& originals, const std::vector<TrackId>& targets,
                                                     const BleedStrength& strength, double engineRate,
                                                     const CancellationToken& cancel, const std::function<void(double)>& progress)
{
    const double rate = model.sampleRate;
    const std::vector<TrackId> taking = bleedTracks(model);
    for (const TrackId& id : targets) {
        if (std::find(taking.begin(), taking.end(), id) == taking.end()) {
            const Track* track = model.findTrack(id);
            return makeError(ErrorCode::InvalidArgument,
                             (track ? track->name : std::string("the track")) + " plays no recorded audio");
        }
    }
    if (targets.empty())
        return std::vector<BleedTrackOutcome>{};
    if (taking.size() < 2)
        return makeError(ErrorCode::InvalidArgument,
                         "bleed suppression compares stems: the project needs at least two tracks of recorded audio");

    // The originals at the project rate (resampled only when they differ).
    std::map<MediaId, AudioAssetPtr> atRate;
    std::vector<Participant> parts;
    for (const TrackId& id : taking) {
        const Track& track = *model.findTrack(id);
        Participant p;
        p.id = id;
        p.target = std::find(targets.begin(), targets.end(), id) != targets.end();
        for (const Clip& clip : track.clips) {
            const MediaRef* original = originalMediaOf(model, clip.media);
            if (original == nullptr || isMidiFile(utf8ToPath(original->path)) || clip.length <= 0)
                continue;
            auto& audio = atRate[original->id];
            if (!audio) {
                AudioAssetPtr decoded = originals ? originals(original->id) : nullptr;
                if (!decoded || decoded->numFrames() <= 0 || decoded->numChannels() <= 0)
                    return makeError(ErrorCode::Unavailable, track.name + " is still loading");
                if (decoded->sampleRate != rate) {
                    auto resampled = resampleAudio(*decoded, rate, {}, cancel);
                    if (!resampled)
                        return resampled.error();
                    decoded = *resampled;
                }
                audio = decoded;
            }
            p.stereo = p.stereo || audio->numChannels() >= 2;
            p.segments.push_back({clip.timelineStart, clip.length, clip.sourceStart, original->id, audio.get()});
            if (p.target && std::none_of(p.units.begin(), p.units.end(), [&](const Unit& u) { return u.original->id == original->id; })) {
                auto copy = AudioAsset::create(audio->numChannels(), audio->numFrames(), rate);
                copy->channels = audio->channels;
                p.units.push_back({original, std::move(copy)});
            }
        }
        parts.push_back(std::move(p));
    }

    // The span the targets' clips cover.
    std::int64_t spanStart = std::numeric_limits<std::int64_t>::max();
    std::int64_t spanEnd = std::numeric_limits<std::int64_t>::min();
    for (const Participant& p : parts)
        if (p.target)
            for (const Segment& seg : p.segments) {
                spanStart = std::min(spanStart, seg.start);
                spanEnd = std::max(spanEnd, seg.start + seg.length);
            }

    constexpr std::int64_t block = static_cast<std::int64_t>(BleedSuppressor::kMaxHops) * BleedSuppressor::kHop;
    constexpr std::int64_t latency = BleedSuppressor::latency();
    for (Participant& p : parts)
        for (int c = 0; c < 2; ++c) {
            p.in[c].assign(static_cast<std::size_t>(block), 0.0f);
            p.out[c].assign(static_cast<std::size_t>(block), 0.0f);
        }

    BleedSuppressor suppressor(static_cast<int>(parts.size()));
    suppressor.setParams({strength.exponent, strength.reductionDb});
    detail::BlockWorkers workers(parts.size());
    const std::int64_t total = spanEnd - spanStart + latency;
    const std::int64_t blocks = (total + block - 1) / block;
    for (std::int64_t b = 0; b < blocks; ++b) {
        if (cancel.isCancelled())
            return makeError(ErrorCode::Cancelled, "bleed suppression cancelled");
        const std::int64_t from = spanStart + b * block;
        workers.run(parts.size(), [&](std::size_t i) {
            Participant& p = parts[i];
            fillInput(p, from, block);
            suppressor.analyse(static_cast<int>(i), p.in[0].data(), p.stereo ? p.in[1].data() : nullptr,
                               BleedSuppressor::kMaxHops);
        });
        suppressor.computeGains();
        workers.run(parts.size(), [&](std::size_t i) {
            Participant& p = parts[i];
            if (!p.target)
                return;
            suppressor.synthesise(static_cast<int>(i), p.out[0].data(), p.stereo ? p.out[1].data() : nullptr);
            routeOutput(p, from - latency, block, spanStart, spanEnd);
        });
        if (progress)
            progress(0.85 * static_cast<double>(b + 1) / static_cast<double>(blocks));
    }

    // Measure, write and load the copies.
    std::vector<BleedTrackOutcome> outcomes;
    std::size_t done = 0, units = 0;
    for (const Participant& p : parts)
        units += p.units.size();
    for (Participant& p : parts) {
        if (!p.target)
            continue;
        BleedTrackOutcome outcome;
        outcome.track = p.id;
        std::vector<double> before, after;
        for (Unit& unit : p.units) {
            const auto b = detail::blockPowers(*atRate[unit.original->id]);
            const auto a = detail::blockPowers(*unit.output);
            before.insert(before.end(), b.begin(), b.end());
            after.insert(after.end(), a.begin(), a.end());
            auto copy = detail::writeProcessedCopy(projectRoot, *unit.original, *unit.output, "bleed", kBleedSuppressProcess,
                                                   strength.reductionDb, engineRate, cancel);
            if (!copy)
                return copy.error();
            unit.output.reset();
            outcome.media.push_back(std::move(copy->media));
            outcome.loaded.push_back(std::move(copy->loaded));
            if (progress)
                progress(0.85 + 0.15 * static_cast<double>(++done) / static_cast<double>(std::max<std::size_t>(units, 1)));
        }
        outcome.musicChangeDb = detail::levelChange(before, after, 0.5, 1.0);
        outcome.bleedChangeDb = detail::levelChange(before, after, 0.0, 0.1);
        outcomes.push_back(std::move(outcome));
    }
    return outcomes;
}

} // namespace daw
