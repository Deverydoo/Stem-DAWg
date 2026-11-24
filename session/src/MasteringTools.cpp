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
    double clippedShare = 0.0;

    bool withinBudget() const noexcept
    {
        return maxReductionDb >= -kMaxLimiterReductionDb && clippedShare <= kMaxClippedShare;
    }
};

// The master chain as the project has it (its multiband compressor runs
// after the fader, as in playback), with the limiter on and the clipper as asked.
MasterParams trialParams(const MasterSettings& master, bool clipper)
{
    MasterParams p;
    p.limiter = true;
    p.ceilingDbtp = static_cast<float>(master.ceilingDbtp);
    p.releaseMs = static_cast<float>(master.releaseMs);
    p.multiband.enabled = master.multiband;
    for (std::size_t b = 0; b < kMultibandBands; ++b) {
        p.multiband.thresholdDb[b] = static_cast<float>(master.multibandThresholdDb[b]);
        p.multiband.ratio[b] = static_cast<float>(master.multibandRatio[b]);
    }
    p.clipper = clipper;
    p.clipBelowCeilingDb = static_cast<float>(master.clipBelowCeilingDb);
    return p;
}

// The mastered mix at a master fader of `gainDb`: the master chain and a
// loudness meter over the rendered mix (which has the EQ already).
Trial runTrial(const AudioAsset& mix, double gainDb, const MasterParams& params, const CancellationToken& cancel, bool& cancelled)
{
    constexpr int block = 4096;
    const double rate = mix.sampleRate;
    MasterChain chain;
    chain.prepare(rate);
    chain.setParams(params);
    chain.reset();
    LoudnessMeter meter;
    meter.prepare(rate, 2, true);
    std::vector<float> left(block), right(block);
    const auto gain = static_cast<float>(std::pow(10.0, gainDb / 20.0));
    const std::int64_t frames = mix.numFrames();
    const std::int64_t total = frames + chain.latency(); // flush the chain's delay
    Trial t;
    t.gainDb = gainDb;
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
        t.maxReductionDb = std::min(t.maxReductionDb, static_cast<double>(chain.takeMinGainDb()));
        // The first latency() frames out of the chain are its empty delay.
        const int skip = static_cast<int>(std::clamp<std::int64_t>(chain.latency() - done, 0, n));
        const float* outs[2] = {left.data() + skip, right.data() + skip};
        meter.process(outs, 2, n - skip);
    }
    t.clippedShare = chain.takeClippedFraction();
    const LoudnessReading& r = meter.reading();
    t.lufs = r.integratedLufs;
    t.truePeakDbtp = r.truePeakDbtp;
    t.loudnessRangeLu = r.loudnessRangeLu;
    return t;
}

struct Search {
    std::optional<Trial> best; // within the budgets, closest to the target
    bool limited = false;      // short of the target because of the budgets
    int trials = 0;
};

// Secant search on the fader: loudness rises by 1 dB per dB until the
// limiter (or clipper) works, then by less. Settings past the budgets are
// out of bounds: the search then bisects between the loudest setting within
// them and the softest one past them.
// With `giveUpOutOfReach`, a setting past the budgets that is still short of
// the target ends the search at once: louder settings only need more, so the
// target is out of reach (the caller then tries the clipper).
Search searchFader(const AudioAsset& mix, double startDb, double target, const MasterParams& params,
                   const std::function<void()>& tick, const CancellationToken& cancel, bool& cancelled,
                   double ceilingGain = kMaxGainDb, // the softest setting known to be past the budgets
                   bool giveUpOutOfReach = false)
{
    Search search;
    std::optional<Trial> previous; // the last trial within the budgets
    double gain = std::clamp(startDb, kMinGainDb, kMaxGainDb);
    for (int trial = 0; trial < kMaxTrials; ++trial) {
        const Trial t = runTrial(mix, gain, params, cancel, cancelled);
        if (cancelled)
            return search;
        ++search.trials;
        if (tick)
            tick();
        double next = 0.0;
        if (!t.withinBudget() && giveUpOutOfReach && t.lufs < target - kToleranceLu) {
            search.limited = true;
            return search;
        }
        if (!t.withinBudget()) {
            ceilingGain = std::min(ceilingGain, t.gainDb);
            next = previous ? 0.5 * (previous->gainDb + t.gainDb) : t.gainDb - 6.0;
        } else {
            if (!search.best || std::abs(t.lufs - target) < std::abs(search.best->lufs - target))
                search.best = t;
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
            break; // pinned against a budget or the end of the fader
        gain = next;
    }
    search.limited = search.best && std::abs(search.best->lufs - target) > kToleranceLu && search.best->lufs < target
                     && ceilingGain < kMaxGainDb;
    return search;
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

    // The mix as the tracks and the master EQ deliver it: master fader at
    // 0 dB, nothing after the EQ (the trials add it).
    ProjectModel dry = model;
    dry.masterGainDb = 0.0;
    dry.master.limiter = false;
    dry.master.multiband = false;
    dry.master.clipper = false;
    RenderOptions renderOptions;
    renderOptions.progress = [&](double fraction) {
        if (options.progress)
            options.progress(0.3 * fraction);
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

    // The limiter alone first; the clipper only when that falls short.
    bool cancelled = false;
    int ticks = 0;
    const auto tick = [&] {
        if (options.progress)
            options.progress(0.3 + 0.7 * std::min(1.0, static_cast<double>(++ticks) / (2 * kMaxTrials)));
    };
    Search search = searchFader(mix, target - match.mixLufs, target, trialParams(settings, false), tick, cancel, cancelled,
                                kMaxGainDb, true);
    bool clipper = false;
    if (!cancelled && search.limited) {
        // Where the clipping budget runs out is known from the mix itself:
        // the fader at which kMaxClippedShare of its oversampled samples
        // reach the clipper's knee. The search starts there.
        const double knee = settings.ceilingDbtp - settings.clipBelowCeilingDb - SoftClipper::kKneeDb;
        const double budget = knee - SoftClipper::levelExceededBy(mix.channels[0].data(),
                                                                   mix.channels[mix.numChannels() > 1 ? 1 : 0].data(),
                                                                   mix.numFrames(), kMaxClippedShare);
        const double start = std::min(target - match.mixLufs, budget - kGainResolutionDb);
        Search clipped = searchFader(mix, start, target, trialParams(settings, true), tick, cancel, cancelled,
                                     std::min(kMaxGainDb, budget + 0.1));
        clipped.trials += search.trials;
        if (clipped.best && (!search.best || clipped.best->lufs > search.best->lufs + kToleranceLu)) {
            search = clipped;
            clipper = true;
        } else {
            search.trials = clipped.trials;
        }
    }
    if (cancelled)
        return makeError(ErrorCode::Cancelled, "loudness match cancelled");
    match.trials = search.trials;
    if (!search.best)
        return makeError(ErrorCode::InvalidArgument, "every master fader setting tried overloaded the limiter");

    const Trial& best = *search.best;
    match.masterGainDb = best.gainDb;
    match.resultLufs = best.lufs;
    match.resultTruePeakDbtp = best.truePeakDbtp;
    match.resultLoudnessRangeLu = best.loudnessRangeLu;
    match.maxReductionDb = best.maxReductionDb;
    match.clipper = clipper;
    match.clippedShare = clipper ? best.clippedShare : 0.0;
    match.reached = std::abs(best.lufs - target) <= kToleranceLu;
    match.limited = !match.reached && search.limited;
    match.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (options.progress)
        options.progress(1.0);
    return match;
}

MasterSettings applyLoudnessMatch(MasterSettings master, const LoudnessMatch& match, double targetLufs)
{
    master.limiter = true;
    master.targetLufs = targetLufs;
    master.clipper = match.clipper;
    master.clamp();
    return master;
}

static_assert(MasterSettings::kMultibandBands == kMultibandBands, "the project and the compressor must agree on the bands");

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
