// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/MasteringTools.h>

#include <daw/core/Files.h>
#include <daw/dsp/LoudnessMeter.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/dsp/MasterChain.h>
#include <daw/session/Renderer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <vector>

namespace daw {

namespace {

constexpr double kToleranceLu = 0.05;
constexpr int kMaxTrials = 12;
constexpr double kGainResolutionDb = 0.05;
constexpr double kMinGainDb = -40.0;
constexpr double kMaxGainDb = 12.0; // the master fader's range in the app

struct Trial {
    double gainDb = 0.0;
    double lufs = kLoudnessFloor;
    double truePeakDbtp = kLoudnessFloor;
    double loudnessRangeLu = 0.0;
    double maxReductionDb = 0.0;
};

// The mastered mix at a master fader of `gainDb`: the limiter and a loudness
// meter over the rendered mix.
Trial runTrial(const AudioAsset& mix, double gainDb, const MasterSettings& master, const CancellationToken& cancel, bool& cancelled)
{
    constexpr int block = 4096;
    const double rate = mix.sampleRate;
    MasterChain chain;
    chain.prepare(rate);
    chain.setParams({.limiter = true,
                     .ceilingDbtp = static_cast<float>(master.ceilingDbtp),
                     .releaseMs = static_cast<float>(master.releaseMs)});
    LoudnessMeter meter;
    meter.prepare(rate, 2, true);
    std::vector<float> left(block), right(block);
    const auto gain = static_cast<float>(std::pow(10.0, gainDb / 20.0));
    const std::int64_t frames = mix.numFrames();
    const std::int64_t total = frames + chain.latency(); // flush the limiter's delay
    float reduction = 0.0f;
    for (std::int64_t done = 0; done < total; done += block) {
        if (cancel.isCancelled()) {
            cancelled = true;
            return {};
        }
        const int n = static_cast<int>(std::min<std::int64_t>(block, total - done));
        for (int i = 0; i < n; ++i) {
            const std::int64_t at = done + i;
            const bool inside = at < frames;
            left[static_cast<std::size_t>(i)] = inside ? mix.channels[0][static_cast<std::size_t>(at)] * gain : 0.0f;
            right[static_cast<std::size_t>(i)] =
                inside ? mix.channels[mix.numChannels() > 1 ? 1 : 0][static_cast<std::size_t>(at)] * gain : 0.0f;
        }
        chain.process(left.data(), right.data(), n);
        reduction = std::min(reduction, chain.takeMinGainDb());
        // The first latency() frames out of the limiter are its empty delay.
        const int skip = static_cast<int>(std::clamp<std::int64_t>(chain.latency() - done, 0, n));
        const float* outs[2] = {left.data() + skip, right.data() + skip};
        meter.process(outs, 2, n - skip);
    }
    const LoudnessReading& r = meter.reading();
    return {gainDb, r.integratedLufs, r.truePeakDbtp, r.loudnessRangeLu, reduction};
}

} // namespace

Result<LoudnessMatch> matchLoudness(const ProjectModel& model, const MediaPool& pool, double targetLufs,
                                    const MasterSettings& master, const LoudnessMatchOptions& options,
                                    const CancellationToken& cancel)
{
    const auto started = std::chrono::steady_clock::now();
    if (!std::isfinite(targetLufs))
        return makeError(ErrorCode::InvalidArgument, "the loudness target is not a number");
    MasterSettings settings = master;
    settings.targetLufs = targetLufs;
    settings.clamp();
    const double target = settings.targetLufs;

    // The mix as the tracks deliver it: master fader at 0 dB, no master bus.
    ProjectModel dry = model;
    dry.masterGainDb = 0.0;
    dry.master.limiter = false;
    RenderOptions renderOptions;
    renderOptions.progress = [&](double fraction) {
        if (options.progress)
            options.progress(0.4 * fraction);
    };
    auto rendered = renderMix(dry, pool, renderOptions, cancel);
    if (!rendered)
        return rendered.error();
    const AudioAsset& mix = *rendered->audio;

    LoudnessMatch match;
    match.mixLufs = rendered->report.loudness.integratedLufs;
    match.mixTruePeakDbtp = rendered->report.loudness.truePeakDbtp;
    if (!(match.mixLufs > -70.0))
        return makeError(ErrorCode::InvalidArgument, "the mix is too quiet to measure its loudness");

    // Secant search on the fader: loudness rises by 1 dB per dB until the
    // limiter works, then by less. Settings that would take the limiter
    // past kMaxLimiterReductionDb are out of bounds: the search then
    // bisects between the loudest setting within the limit and the softest
    // one past it.
    bool cancelled = false;
    std::optional<Trial> best;     // within the limit, closest to the target
    std::optional<Trial> previous; // the last trial within the limit
    double ceilingGain = kMaxGainDb; // the softest setting found past the limit
    double gain = std::clamp(target - match.mixLufs, kMinGainDb, kMaxGainDb);
    for (int trial = 0; trial < kMaxTrials; ++trial) {
        const Trial t = runTrial(mix, gain, settings, cancel, cancelled);
        if (cancelled)
            return makeError(ErrorCode::Cancelled, "loudness match cancelled");
        ++match.trials;
        if (options.progress)
            options.progress(0.4 + 0.6 * (trial + 1) / kMaxTrials);

        double next = 0.0;
        if (t.maxReductionDb < -kMaxLimiterReductionDb) {
            ceilingGain = std::min(ceilingGain, t.gainDb);
            next = previous ? 0.5 * (previous->gainDb + t.gainDb) : t.gainDb - 6.0;
        } else {
            if (!best || std::abs(t.lufs - target) < std::abs(best->lufs - target))
                best = t;
            if (std::abs(t.lufs - target) <= kToleranceLu)
                break;
            double slope = 1.0;
            if (previous && t.gainDb != previous->gainDb)
                slope = std::clamp((t.lufs - previous->lufs) / (t.gainDb - previous->gainDb), 0.05, 1.0);
            previous = t;
            next = t.gainDb + (target - t.lufs) / slope;
            if (next >= ceilingGain)
                next = 0.5 * (t.gainDb + ceilingGain);
        }
        next = std::clamp(next, kMinGainDb, kMaxGainDb);
        if (std::abs(next - t.gainDb) < kGainResolutionDb)
            break; // pinned against the limit or the end of the fader
        gain = next;
    }
    if (!best)
        return makeError(ErrorCode::InvalidArgument, "every master fader setting tried overloaded the limiter");

    match.masterGainDb = best->gainDb;
    match.resultLufs = best->lufs;
    match.resultTruePeakDbtp = best->truePeakDbtp;
    match.resultLoudnessRangeLu = best->loudnessRangeLu;
    match.maxReductionDb = best->maxReductionDb;
    match.reached = std::abs(best->lufs - target) <= kToleranceLu;
    match.limited = !match.reached && best->lufs < target && ceilingGain < kMaxGainDb;
    match.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (options.progress)
        options.progress(1.0);
    return match;
}

// ---- tonal balance ------------------------------------------------------------

static_assert(TonalTargetSettings::kBands == kTonalBands, "the project and the analysis must agree on the bands");
static_assert(MasterSettings::kEqBands == kMasterEqBandCount, "the project and the master chain must agree on the EQ");

namespace {

TonalProfile profileOf(const AudioAsset& audio)
{
    TonalAnalyzer analyzer(audio.sampleRate);
    const float* left = audio.channels[0].data();
    const float* right = audio.numChannels() > 1 ? audio.channels[1].data() : left;
    analyzer.process(left, right, static_cast<int>(audio.numFrames()));
    return analyzer.result();
}

} // namespace

Result<TonalProfile> tonalProfileOfFile(const std::filesystem::path& path, const CancellationToken& cancel)
{
    auto decoded = decodeAudioFile(path, {}, cancel);
    if (!decoded)
        return decoded.error();
    const TonalProfile profile = profileOf(**decoded);
    if (!profile.valid())
        return makeError(ErrorCode::InvalidArgument, "the file is silent: " + pathToUtf8(path.filename()));
    return profile;
}

Result<TonalProfile> tonalProfileOfMix(const ProjectModel& model, const MediaPool& pool, const std::function<void(double)>& progress,
                                       const CancellationToken& cancel)
{
    RenderOptions options;
    options.progress = progress;
    auto rendered = renderMix(model, pool, options, cancel);
    if (!rendered)
        return rendered.error();
    const TonalProfile profile = profileOf(*rendered->audio);
    if (!profile.valid())
        return makeError(ErrorCode::InvalidArgument, "the mix is silent");
    return profile;
}

Result<TonalTarget> tonalTargetFromFiles(const std::string& name, const std::vector<std::filesystem::path>& files,
                                         std::vector<std::string>* skipped, const std::function<void(double)>& progress,
                                         const CancellationToken& cancel)
{
    std::vector<TonalProfile> profiles;
    std::vector<std::string> references;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (cancel.isCancelled())
            return makeError(ErrorCode::Cancelled, "building the target was cancelled");
        auto profile = tonalProfileOfFile(files[i], cancel);
        if (profile) {
            profiles.push_back(*profile);
            references.push_back(pathToUtf8(files[i].filename()));
        } else if (skipped != nullptr) {
            skipped->push_back(pathToUtf8(files[i].filename()));
        }
        if (progress)
            progress(static_cast<double>(i + 1) / static_cast<double>(files.size()));
    }
    if (profiles.empty())
        return makeError(ErrorCode::InvalidArgument, "none of the reference files could be read");
    return makeTonalTarget(name, std::move(references), profiles);
}

TonalTargetSettings toSettings(const TonalTarget& target)
{
    TonalTargetSettings s;
    if (!target.valid())
        return s;
    s.name = target.name;
    s.references = target.references;
    s.meanDb.assign(target.meanDb.begin(), target.meanDb.end());
    s.spreadDb.assign(target.spreadDb.begin(), target.spreadDb.end());
    return s;
}

TonalTarget fromSettings(const TonalTargetSettings& settings)
{
    TonalTarget t;
    if (settings.meanDb.size() != kTonalBands || settings.spreadDb.size() != kTonalBands)
        return t;
    t.name = settings.name;
    t.references = settings.references.empty() ? std::vector<std::string>{settings.name} : settings.references;
    std::copy(settings.meanDb.begin(), settings.meanDb.end(), t.meanDb.begin());
    std::copy(settings.spreadDb.begin(), settings.spreadDb.end(), t.spreadDb.begin());
    return t;
}

} // namespace daw
