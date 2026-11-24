// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/SnapshotBuilder.h>

#include <daw/core/Hash.h>
#include <daw/dsp/SpectralDenoiser.h>

#include <algorithm>
#include <numbers>

namespace daw {

ChannelGains trackChannelGains(const ProjectModel& model, const Track& track) noexcept
{
    if (!model.isAudible(track))
        return {0.0f, 0.0f};
    const double gain = dbToGain(track.gainDb);
    const double pan = std::clamp(track.pan, -1.0, 1.0);
    // cos(pi/2) is not exactly 0 in floating point; hard pan must fully mute the far side.
    const double opposite = std::abs(pan) >= 1.0 ? 0.0 : std::cos(std::abs(pan) * std::numbers::pi / 2.0);
    return {static_cast<float>(gain * (pan > 0.0 ? opposite : 1.0)), static_cast<float>(gain * (pan < 0.0 ? opposite : 1.0))};
}

std::uint64_t trackPlaybackKey(const TrackId& track) noexcept
{
    return fnv1a64(track.str()) | 1u; // never 0
}

CleanupParams cleanupParams(const TrackCleanup& cleanup) noexcept
{
    CleanupParams p;
    if (!cleanup.active())
        return p;
    TrackCleanup c = cleanup;
    c.clamp();
    p.dcRemoval = c.dcRemoval;
    p.highPass = c.highPass.enabled;
    p.highPassHz = static_cast<float>(c.highPass.frequencyHz);
    p.highPassOrder = c.highPass.slopeDbPerOctave / 6;
    p.expander = c.expander.enabled;
    p.thresholdDb = static_cast<float>(c.expander.thresholdDb);
    p.ratio = static_cast<float>(c.expander.ratio);
    p.rangeDb = static_cast<float>(c.expander.rangeDb);
    p.releaseMs = static_cast<float>(c.expander.releaseMs);
    p.hum = c.hum.enabled;
    p.humHz = static_cast<float>(c.hum.frequencyHz);
    p.humHarmonics = c.hum.harmonics;
    p.denoise = c.denoise.enabled;
    p.denoiseReductionDb = static_cast<float>(c.denoise.reductionDb);
    p.denoiseSensitivityDb = static_cast<float>(c.denoise.sensitivityDb);
    p.transient = c.transient.enabled;
    p.transientAttackDb = static_cast<float>(c.transient.attackDb);
    p.transientSustainDb = static_cast<float>(c.transient.sustainDb);
    p.resonance = c.resonance.enabled;
    p.resonanceHarshDb = static_cast<float>(c.resonance.harshDb);
    p.resonanceShimmerDb = static_cast<float>(c.resonance.shimmerDb);
    p.stereo = c.stereo.enabled;
    p.stereoMonoBass = c.stereo.monoBass;
    p.stereoMonoBelowHz = static_cast<float>(c.stereo.monoBelowHz);
    p.stereoWidth = static_cast<float>(c.stereo.widthPercent / 100.0);
    p.stereoPhaseAlign = c.stereo.phaseAlign;
    return p;
}

std::vector<float> noiseProfileForEngine(const NoiseProfile& profile, double engineRate)
{
    if (profile.empty() || !(engineRate > 0.0))
        return {};
    constexpr int kSize = SpectralDenoiser::kFftSize;
    // Noise power per bin grows with the frame length (same window shape).
    const double sizeDb = 10.0 * std::log10(static_cast<double>(kSize) / profile.fftSize);
    const double profileBinHz = profile.sampleRate / profile.fftSize;
    const auto last = static_cast<double>(profile.powerDb.size() - 1);
    std::vector<float> power(SpectralDenoiser::kBins);
    for (int k = 0; k < SpectralDenoiser::kBins; ++k) {
        const double hz = k * engineRate / kSize;
        const double x = std::min(hz / profileBinHz, last);
        const auto i = static_cast<std::size_t>(x);
        const std::size_t j = std::min(i + 1, profile.powerDb.size() - 1);
        const double frac = x - static_cast<double>(i);
        const double db = profile.powerDb[i] + (profile.powerDb[j] - profile.powerDb[i]) * frac + sizeDb;
        power[static_cast<std::size_t>(k)] = static_cast<float>(std::pow(10.0, db / 10.0));
    }
    return power;
}

std::unique_ptr<PlaybackSnapshot> buildPlaybackSnapshot(const ProjectModel& model, const MediaPool& pool,
                                                        double engineSampleRate, std::uint64_t version,
                                                        SnapshotBuildReport* report, const SnapshotBuildOptions& options)
{
    const TimeMapper map{model.sampleRate, engineSampleRate};
    auto snapshot = std::make_unique<PlaybackSnapshot>();
    snapshot->version = version;
    snapshot->sampleRate = engineSampleRate;
    // The master fader is folded into each track's gains so the engine ramps
    // master changes exactly like track changes (no clicks).
    const auto master = static_cast<float>(dbToGain(model.masterGainDb));
    snapshot->masterGain = 1.0f;
    {
        MasterSettings m = model.master;
        m.clamp();
        snapshot->master = {.limiter = m.limiter,
                            .ceilingDbtp = static_cast<float>(m.ceilingDbtp),
                            .releaseMs = static_cast<float>(m.releaseMs),
                            .eq = m.eq};
        for (std::size_t b = 0; b < m.eqGainDb.size(); ++b)
            snapshot->master.eqGainDb[b] = static_cast<float>(m.eqGainDb[b]);
        snapshot->master.multiband.enabled = m.multiband;
        snapshot->master.clipper = m.clipper;
        snapshot->master.clipBelowCeilingDb = static_cast<float>(m.clipBelowCeilingDb);
        for (std::size_t b = 0; b < m.multibandRatio.size(); ++b) {
            snapshot->master.multiband.thresholdDb[b] = static_cast<float>(m.multibandThresholdDb[b]);
            snapshot->master.multiband.ratio[b] = static_cast<float>(m.multibandRatio[b]);
        }
    }

    for (const Track& track : model.tracks) {
        TrackPlayback playback;
        const ChannelGains gains = trackChannelGains(model, track);
        playback.gainLeft = gains.left * master;
        playback.gainRight = gains.right * master;
        playback.key = trackPlaybackKey(track.id);
        if (!options.bypassCleanup) {
            playback.cleanup = cleanupParams(track.cleanup);
            if (playback.cleanup.denoise && !track.cleanup.denoise.profile.empty()) {
                auto power = std::make_shared<const std::vector<float>>(
                    noiseProfileForEngine(track.cleanup.denoise.profile, engineSampleRate));
                playback.cleanup.noiseProfile = power->data();
                snapshot->noiseProfiles.push_back(std::move(power));
            }
        }
        if (options.listenRemoved) {
            if (playback.cleanup.active())
                playback.cleanup.listenRemoved = true;
            else
                playback.gainLeft = playback.gainRight = 0.0f; // nothing removed: nothing to hear
        }

        for (const Clip& clip : track.clips) {
            const auto loaded = pool.get(clip.media);
            if (!loaded || !loaded->rendition || loaded->renditionRate != engineSampleRate) {
                if (report != nullptr)
                    report->missingMedia.push_back(clip.media);
                continue;
            }
            ClipPlayback cp;
            cp.asset = loaded->rendition.get();
            cp.timelineStart = map.toEngine(clip.timelineStart);
            cp.length = map.toEngine(clip.timelineStart + clip.length) - cp.timelineStart;
            cp.sourceStart = map.toEngine(clip.sourceStart);
            cp.gain = static_cast<float>(dbToGain(clip.gainDb));
            snapshot->endFrame = std::max(snapshot->endFrame, cp.timelineStart + cp.length);
            if (std::find(snapshot->keepAlive.begin(), snapshot->keepAlive.end(), loaded->rendition)
                == snapshot->keepAlive.end())
                snapshot->keepAlive.push_back(loaded->rendition);
            playback.clips.push_back(cp);
        }
        std::sort(playback.clips.begin(), playback.clips.end(),
                  [](const ClipPlayback& a, const ClipPlayback& b) { return a.timelineStart < b.timelineStart; });
        snapshot->tracks.push_back(std::move(playback));
    }
    return snapshot;
}

} // namespace daw
