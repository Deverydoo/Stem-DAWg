// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/StemReport.h>

#include "BlockWorkers.h"

#include <daw/dsp/Biquad.h>
#include <daw/dsp/Fft.h>
#include <daw/dsp/ResonanceSuppressor.h>
#include <daw/dsp/SpectralDenoiser.h>
#include <daw/engine/AudioEngine.h>
#include <daw/project/ProjectSerializer.h>
#include <daw/session/Renderer.h>
#include <daw/session/SnapshotBuilder.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <memory>

namespace daw {

namespace {

constexpr int kBlock = 4096;            // render block and FFT frame (Hann, no overlap)
constexpr double kFrameSeconds = 0.05;  // RMS frames for noise floor and activity
constexpr float kDigitalSilence = -999.0f;

// Advice thresholds. Tuned on real Suno stem sets; see docs/ROADMAP.md (M2).
constexpr double kCeilingDbtp = -1.0;          // streaming true-peak ceiling
constexpr double kResidueBelowLoudestLu = 15.0; // "very quiet" relative to the loudest stem
constexpr double kResidueCorrelation = 0.30;
constexpr double kNoiseBedMaxDbfs = -40.0;      // a quieter bed than this is reported
constexpr double kNoiseBedMinSpreadDb = 20.0;   // music must rise this far above the bed
constexpr double kNoiseBedAudibleDbfs = -66.0;  // beds below this are left alone
// Expander threshold above the measured bed: the same line the report draws
// between "playing" and "bed" (see activeFraction). Tuned on real Suno stems:
// a 6 dB margin left most of the bed untouched, because Suno beds are not
// steady and the gaps between notes are short.
constexpr double kExpanderMarginDb = 10.0;
constexpr int kLowEndSlope = 24;                 // dB/octave of suggested high-passes
constexpr double kLowEndCorrelation = 0.50;
constexpr double kLowEndMinShare = 0.15;
constexpr double kBleedCorrelation = 0.35;
constexpr double kExtraLowEndShare = 0.50;     // a small stem that is mostly low end
constexpr double kExtraLowEndMaxLevel = 0.05;
constexpr double kOtherMaxLevel = 0.10;
constexpr double kDcLimit = 0.002;              // about -54 dBFS
constexpr double kOutOfPhase = -0.2;
constexpr double kMonoCorrelation = 0.999;
constexpr double kAudibleShare = 0.01;          // below 1 % of the mix a stem's problems are minor

// Attack measurement: 0.5 ms RMS frames of the mid signal above kAttackBandHz.
constexpr double kAttackFrameMs = 0.5;
constexpr int kAttackRing = 64;           // frames kept (32 ms)
constexpr int kAttackPeakBefore = 20;     // a hit's peak is the highest frame 10 ms before ...
constexpr int kAttackPeakAfter = 10;      // ... and 5 ms after it
constexpr int kAttackFootFrames = 30;     // its foot is the lowest frame in the 15 ms before the peak
constexpr double kAttackMinJumpDb = 15.0;
constexpr int kAttackMinSpacing = 60;     // frames (30 ms) between hits
constexpr double kAttackLoudRangeDb = 12.0; // hits this close to the loudest count
constexpr std::size_t kAttackMinHits = 20;
constexpr float kAttackSilenceDb = -200.0f;
// Drum stems whose hits rise slower than this get the transient shaper
// suggested. Tuned on real Suno drum stems (about 3 ms); see docs/milestone-4.
constexpr double kSoftAttackMs = 2.0;
constexpr double kSoftAttackGainDb = 6.0;
// Resonances: measured with the chain's detector on 2048-point frames. The
// chain takes one every 512 samples; the report, which only needs the
// detector's statistics over seconds, one every 2048 (its time constants are
// in milliseconds, so the result is the same). A stem whose strongest peak in
// a range would be cut by kResonanceAdviceDb on average gets the suppressor
// suggested. Tuned on 24 Suno songs and a 9-stem export: flagged songs
// measure 2.0-3.9 dB, the others up to 1.9 dB, bright synthetic 1 s notes
// 1.8 dB; see docs/milestone-4.
constexpr int kResonanceHop = SpectralDenoiser::kFftSize;
constexpr double kResonanceAdviceDb = 2.0;
constexpr double kResonanceActiveDbfs = -70.0; // frames quieter than this are not counted
constexpr double kResonanceMinSeconds = 5.0;   // of counted frames
// A stem that supplies less of the mix's energy in the band around the peak
// than this is masked there by the others (on the 9-stem export, the bass's
// 2 kHz peak sits in 4 % of that band).
constexpr double kResonanceMinBandShare = 0.10;

std::string format(const char* pattern, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(buffer, sizeof buffer, pattern, args);
    va_end(args);
    return buffer;
}

bool containsWord(std::string text, const std::string& word)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
        const bool startOk = at == 0 || !std::isalpha(static_cast<unsigned char>(text[at - 1]));
        const std::size_t end = at + word.size();
        const bool endOk = end == text.size() || !std::isalpha(static_cast<unsigned char>(text[end]));
        if (startOk && endOk)
            return true;
    }
    return false;
}

// Stems named for drums or percussion (Suno names its stems; a measured
// attack time alone cannot tell drums from consonants or plucks).
bool isPercussionStem(const std::string& name)
{
    for (const char* word : {"drum", "drums", "percussion", "perc", "kick", "kicks", "snare", "snares", "hat", "hats", "hihat",
                             "hihats", "cymbal", "cymbals", "toms", "beat", "beats"})
        if (containsWord(name, word))
            return true;
    return false;
}

double toDb(double power)
{
    return power > 0.0 ? 10.0 * std::log10(power) : kLoudnessFloor;
}

double percentile(std::vector<float> values, double p)
{
    if (values.empty())
        return kLoudnessFloor;
    const auto index = static_cast<std::size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}

struct TrackState {
    std::unique_ptr<AudioEngine> engine;
    LoudnessMeter meter;
    std::vector<float> left, right;
    double energy = 0.0;
    double sumL = 0.0, sumR = 0.0, sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;
    double midEnergy = 0.0, sideEnergy = 0.0;
    std::vector<float> frameDb;
    double framePower = 0.0;
    int framePosition = 0;
    std::vector<double> binPower;               // accumulated |X|^2 per bin
    std::vector<std::complex<float>> spectrum;  // current frame
    bool frameHasSignal = false;

    // Resonances.
    ResonanceSuppressor resonance;
    std::array<std::vector<float>, 2> resonanceInput; // last kFftSize samples per channel
    int resonanceFill = 0;
    std::vector<float> resonancePower;
    std::vector<std::complex<float>> resonanceScratch;
    std::vector<double> resonanceCut;        // summed per bin over counted frames
    std::vector<double> resonanceRingExcess; // excess summed over the frames that cut
    std::vector<std::int64_t> resonanceRings; // frames that cut, per bin
    std::int64_t resonanceFrames = 0;
    void addResonanceFrame(const Fft& fft, const float* window);

    // Attacks.
    Biquad attackFilter;
    double attackPower = 0.0;
    int attackPosition = 0;
    std::array<float, kAttackRing> attackDb{}; // ring of frame levels, indexed by frame number
    std::int64_t attackFrames = 0;
    std::int64_t lastHit = -kAttackMinSpacing;
    std::vector<std::array<float, 2>> hits;   // {rise ms, peak dB}

    float levelAt(std::int64_t frame) const { return attackDb[static_cast<std::size_t>(frame % kAttackRing)]; }
    void addAttackFrame(float db);
};

double amplitude(float db)
{
    return db <= kAttackSilenceDb ? 0.0 : std::pow(10.0, db / 20.0);
}

// Called for every 0.5 ms frame; looks for a hit peaking kAttackPeakAfter frames ago.
void TrackState::addAttackFrame(float db)
{
    attackDb[static_cast<std::size_t>(attackFrames % kAttackRing)] = db;
    ++attackFrames;
    const std::int64_t peak = attackFrames - 1 - kAttackPeakAfter;
    if (peak - kAttackFootFrames < 0 || peak - lastHit < kAttackMinSpacing)
        return;
    const float top = levelAt(peak);
    for (std::int64_t k = peak - kAttackPeakBefore; k < peak; ++k)
        if (levelAt(k) >= top)
            return;
    for (std::int64_t k = peak + 1; k <= peak + kAttackPeakAfter; ++k)
        if (levelAt(k) > top)
            return;
    std::int64_t foot = peak;
    for (std::int64_t k = peak - kAttackFootFrames; k < peak; ++k)
        if (levelAt(k) < levelAt(foot))
            foot = k;
    if (top - levelAt(foot) < kAttackMinJumpDb)
        return;

    // 10 % and 90 % of the jump, in amplitude, interpolated between frames.
    // The top is the peak frame averaged with its neighbours: on noisy hits
    // (snares, hats) a single frame is an extreme the rise never really reaches.
    const double low = amplitude(levelAt(foot));
    const double high = (amplitude(levelAt(peak - 1)) + amplitude(top) + amplitude(levelAt(peak + 1))) / 3.0;
    const double at10 = low + 0.1 * (high - low);
    const double at90 = low + 0.9 * (high - low);
    std::int64_t k90 = foot + 1;
    while (k90 < peak && amplitude(levelAt(k90)) < at90)
        ++k90;
    const double a90 = amplitude(levelAt(k90)), b90 = amplitude(levelAt(k90 - 1));
    const double t90 = static_cast<double>(k90 - 1) + (a90 > b90 ? (at90 - b90) / (a90 - b90) : 1.0);
    std::int64_t k10 = k90 - 1;
    while (k10 > foot && amplitude(levelAt(k10)) >= at10)
        --k10;
    const double a10 = amplitude(levelAt(k10)), b10 = amplitude(levelAt(k10 + 1));
    const double t10 = static_cast<double>(k10) + (b10 > a10 ? (at10 - a10) / (b10 - a10) : 0.0);
    hits.push_back({static_cast<float>(std::max(0.0, t90 - t10) * kAttackFrameMs), top});
    lastHit = peak;
}

// Called every kResonanceHop samples with the last kFftSize in resonanceInput.
void TrackState::addResonanceFrame(const Fft& fft, const float* window)
{
    constexpr int size = SpectralDenoiser::kFftSize;
    double meanSquare = 0.0;
    for (int i = 0; i < size; ++i) {
        const double l = resonanceInput[0][static_cast<std::size_t>(i)];
        const double r = resonanceInput[1][static_cast<std::size_t>(i)];
        meanSquare += 0.5 * (l * l + r * r);
    }
    meanSquare /= size;
    SpectralDenoiser::framePower(resonanceInput[0].data(), resonanceInput[1].data(), resonancePower.data(),
                                 resonanceScratch.data(), fft, window);
    resonance.analyse(resonancePower.data());
    if (meanSquare > std::pow(10.0, kResonanceActiveDbfs / 10.0)) {
        for (int k = 0; k < resonance.bins(); ++k) {
            const float cut = resonance.cutDb(k);
            if (cut <= 0.0f)
                continue;
            const auto i = static_cast<std::size_t>(k);
            resonanceCut[i] += cut;
            resonanceRingExcess[i] += resonance.excessDb(k);
            ++resonanceRings[i];
        }
        ++resonanceFrames;
    }
    for (auto& channel : resonanceInput)
        std::copy(channel.begin() + kResonanceHop, channel.end(), channel.begin());
}

// The track's share of all tracks' energy in the StemBand that holds `hz`.
double bandShareOfMix(const StemReport& report, std::size_t track, double hz)
{
    std::size_t band = kStemBandCount - 1;
    while (band > 0 && hz < kStemBandEdgesHz[band])
        --band;
    double total = 0.0;
    for (const TrackStemReport& t : report.tracks)
        total += t.levelShare * t.bandShare[band];
    const TrackStemReport& t = report.tracks[track];
    return total > 0.0 ? t.levelShare * t.bandShare[band] / total : 0.0;
}

std::unique_ptr<AudioEngine> makeEngine(const ProjectModel& model, const MediaPool& media, double rate)
{
    auto engine = std::make_unique<AudioEngine>();
    engine->prepare({.sampleRate = rate,
                     .maxBlockSize = kBlock,
                     .numOutputChannels = 2,
                     .meterLoudness = false,
                     .maxCleanupTracks = static_cast<int>(model.tracks.size())});
    engine->publishSnapshot(buildPlaybackSnapshot(model, media, rate, 1));
    engine->setStopAtEnd(false);
    engine->seek(0);
    engine->play();
    return engine;
}

std::int64_t countSourceOvers(const Track& track, const MediaPool& media)
{
    std::int64_t overs = 0;
    for (const Clip& clip : track.clips) {
        const auto loaded = media.get(clip.media);
        if (!loaded || !loaded->rendition)
            continue;
        const AudioAsset& asset = *loaded->rendition;
        const std::int64_t begin = std::clamp<std::int64_t>(clip.sourceStart, 0, asset.numFrames());
        const std::int64_t end = std::clamp<std::int64_t>(clip.sourceStart + clip.length, begin, asset.numFrames());
        for (int c = 0; c < asset.numChannels(); ++c) {
            const float* data = asset.channelData(c);
            for (std::int64_t i = begin; i < end; ++i)
                overs += std::abs(data[i]) >= 0.9999f ? 1 : 0;
        }
    }
    return overs;
}

} // namespace

const char* toString(StemBand band) noexcept
{
    switch (band) {
    case StemBand::Sub: return "sub";
    case StemBand::Bass: return "bass";
    case StemBand::LowMid: return "low-mid";
    case StemBand::Mid: return "mid";
    case StemBand::Presence: return "presence";
    case StemBand::Air: return "air";
    case StemBand::Count: break;
    }
    return "?";
}

const char* toString(AdviceSeverity severity) noexcept
{
    switch (severity) {
    case AdviceSeverity::Warning: return "warning";
    case AdviceSeverity::Suggestion: return "suggestion";
    case AdviceSeverity::Info: return "info";
    }
    return "?";
}

TrackCleanup CleanupFix::applyTo(TrackCleanup cleanup) const
{
    cleanup.bypassed = false;
    if (dcRemoval)
        cleanup.dcRemoval = *dcRemoval;
    if (highPass)
        cleanup.highPass = *highPass;
    if (expander)
        cleanup.expander = *expander;
    if (transient)
        cleanup.transient = *transient;
    if (resonance) {
        if (cleanup.resonance.enabled) {
            cleanup.resonance.harshDb = std::max(cleanup.resonance.harshDb, resonance->harshDb);
            cleanup.resonance.shimmerDb = std::max(cleanup.resonance.shimmerDb, resonance->shimmerDb);
        } else {
            cleanup.resonance = *resonance;
        }
    }
    cleanup.clamp();
    return cleanup;
}

const StemPairReport* StemReport::pair(std::size_t a, std::size_t b) const noexcept
{
    if (a > b)
        std::swap(a, b);
    for (const StemPairReport& p : pairs)
        if (p.a == a && p.b == b)
            return &p;
    return nullptr;
}

Result<StemReport> analyzeStems(const ProjectModel& model, const MediaPool& pool, const StemReportOptions& options,
                                const CancellationToken& cancel)
{
    const auto started = std::chrono::steady_clock::now();
    const double rate = model.sampleRate;
    if (!(rate > 0.0))
        return makeError(ErrorCode::InvalidArgument, "project has no sample rate");
    if (model.tracks.empty())
        return makeError(ErrorCode::InvalidArgument, "nothing to analyse: the project has no tracks");
    const std::int64_t total = model.contentEnd();
    if (total <= 0)
        return makeError(ErrorCode::InvalidArgument, "nothing to analyse: the project has no audio");

    auto prepared = prepareMediaAtRate(model, pool, rate, cancel);
    if (!prepared)
        return prepared.error();
    const MediaPool& media = **prepared;

    // One private engine per track renders the stem exactly as it plays, with
    // its fader and pan, but never muted (the report covers every stem).
    const std::size_t n = model.tracks.size();
    std::vector<TrackState> tracks(n);
    for (std::size_t i = 0; i < n; ++i) {
        ProjectModel single = model;
        single.tracks = {model.tracks[i]};
        single.tracks[0].muted = false;
        single.tracks[0].soloed = false;
        single.masterGainDb = 0.0;
        TrackState& t = tracks[i];
        t.engine = makeEngine(single, media, rate);
        t.meter.prepare(rate, 2, false);
        t.left.resize(kBlock);
        t.right.resize(kBlock);
        t.binPower.assign(kBlock / 2 + 1, 0.0);
        t.spectrum.resize(kBlock);
        t.frameDb.reserve(static_cast<std::size_t>(static_cast<double>(total) / (kFrameSeconds * rate)) + 2);
        t.attackFilter.setCoefficients(BiquadCoefficients::highPass(std::min(kAttackBandHz, 0.4 * rate), 0.7071, rate));
        t.attackDb.fill(kAttackSilenceDb);
        t.resonance.prepare(rate, SpectralDenoiser::kFftSize, kResonanceHop);
        t.resonance.setParams({static_cast<float>(kResonanceProbeDb), static_cast<float>(kResonanceProbeDb)});
        for (auto& channel : t.resonanceInput)
            channel.assign(SpectralDenoiser::kFftSize, 0.0f);
        t.resonancePower.assign(SpectralDenoiser::kBins, 0.0f);
        t.resonanceScratch.assign(SpectralDenoiser::kFftSize, {});
        t.resonanceCut.assign(SpectralDenoiser::kBins, 0.0);
        t.resonanceRingExcess.assign(SpectralDenoiser::kBins, 0.0);
        t.resonanceRings.assign(SpectralDenoiser::kBins, 0);
    }
    const Fft resonanceFft(SpectralDenoiser::kFftSize);
    const std::vector<float> resonanceWindow = SpectralDenoiser::window();
    const int attackFrameLength = std::max(1, static_cast<int>(std::lround(kAttackFrameMs * 0.001 * rate)));
    auto mixEngine = makeEngine(model, media, rate);
    LoudnessMeter mixMeter;
    mixMeter.prepare(rate, 2, true);
    std::vector<float> mixLeft(kBlock), mixRight(kBlock);

    const Fft fft(kBlock);
    const std::vector<float> window = hannWindow(kBlock);
    const int frameLength = std::max(1, static_cast<int>(std::lround(kFrameSeconds * rate)));

    // FFT bins per compared band (DC excluded).
    std::array<std::pair<int, int>, 3> pairBins{};
    {
        const auto bin = [&](double hz) {
            return std::clamp(static_cast<int>(std::ceil(hz * kBlock / rate)), 1, kBlock / 2 + 1);
        };
        pairBins[0] = {1, bin(kLowEndHz)};
        pairBins[1] = {bin(kLowEndHz), bin(kHighBandHz)};
        pairBins[2] = {bin(kHighBandHz), kBlock / 2 + 1};
    }
    std::vector<double> cross(n * (n - 1) / 2 * 3, 0.0);

    StemReport report;
    report.sampleRate = rate;
    report.frames = total;
    std::int64_t lastProgress = 0;

    // Each block: the mix and every stem are measured in parallel (task n is
    // the mix), then the stems are compared pairwise.
    detail::BlockWorkers workers(n + 1);
    int count = 0;
    const std::function<void(std::size_t)> measure = [&](std::size_t task) {
        if (task == n) {
            float* outs[2] = {mixLeft.data(), mixRight.data()};
            mixEngine->process(outs, 2, count);
            mixMeter.process(outs, 2, count);
            for (float* channel : outs)
                for (int i = 0; i < count; ++i)
                    report.mixOvers += std::abs(channel[i]) > 1.0f ? 1 : 0;
            return;
        }
        TrackState& t = tracks[task];
        float* outs[2] = {t.left.data(), t.right.data()};
        t.engine->process(outs, 2, count);
        t.meter.process(outs, 2, count);

        double frameEnergy = 0.0;
        for (int i = 0; i < count; ++i) {
            const double l = t.left[static_cast<std::size_t>(i)];
            const double r = t.right[static_cast<std::size_t>(i)];
            t.sumL += l;
            t.sumR += r;
            t.sumLL += l * l;
            t.sumRR += r * r;
            t.sumLR += l * r;
            const double mid = 0.5 * (l + r);
            const double side = 0.5 * (l - r);
            t.midEnergy += mid * mid;
            t.sideEnergy += side * side;
            frameEnergy += l * l + r * r;

            t.framePower += 0.5 * (l * l + r * r);
            if (++t.framePosition == frameLength) {
                t.frameDb.push_back(t.framePower > 0.0 ? static_cast<float>(toDb(t.framePower / frameLength)) : kDigitalSilence);
                t.framePower = 0.0;
                t.framePosition = 0;
            }
            t.spectrum[static_cast<std::size_t>(i)] = {static_cast<float>(mid) * window[static_cast<std::size_t>(i)], 0.0f};

            const double high = t.attackFilter.process(mid);
            t.attackPower += high * high;
            if (++t.attackPosition == attackFrameLength) {
                const double mean = t.attackPower / attackFrameLength;
                t.addAttackFrame(mean > 1e-20 ? static_cast<float>(toDb(mean)) : kAttackSilenceDb);
                t.attackPower = 0.0;
                t.attackPosition = 0;
            }

            const auto slot = static_cast<std::size_t>(SpectralDenoiser::kFftSize - kResonanceHop + t.resonanceFill);
            t.resonanceInput[0][slot] = static_cast<float>(l);
            t.resonanceInput[1][slot] = static_cast<float>(r);
            if (++t.resonanceFill == kResonanceHop) {
                t.addResonanceFrame(resonanceFft, resonanceWindow.data());
                t.resonanceFill = 0;
            }
        }
        t.energy += frameEnergy;
        t.frameHasSignal = frameEnergy > 0.0;
        if (!t.frameHasSignal)
            return;
        std::fill(t.spectrum.begin() + count, t.spectrum.end(), std::complex<float>{});
        fft.forward(t.spectrum);
        for (int k = 1; k <= kBlock / 2; ++k)
            t.binPower[static_cast<std::size_t>(k)] += std::norm(t.spectrum[static_cast<std::size_t>(k)]);
    };

    // Cross-spectra: the in-phase part of what two stems share, per band.
    // Task a handles the pairs (a, b > a); each pair writes only its own slots.
    const std::function<void(std::size_t)> compare = [&](std::size_t a) {
        for (std::size_t b = a + 1; b < n; ++b) {
            if (!tracks[a].frameHasSignal || !tracks[b].frameHasSignal)
                continue;
            const std::size_t p = a * n - a * (a + 1) / 2 + (b - a - 1);
            const auto& xa = tracks[a].spectrum;
            const auto& xb = tracks[b].spectrum;
            for (std::size_t band = 0; band < 3; ++band) {
                float sum = 0.0f;
                for (int k = pairBins[band].first; k < pairBins[band].second; ++k) {
                    const auto& u = xa[static_cast<std::size_t>(k)];
                    const auto& v = xb[static_cast<std::size_t>(k)];
                    sum += u.real() * v.real() + u.imag() * v.imag();
                }
                cross[p * 3 + band] += sum;
            }
        }
    };

    for (std::int64_t done = 0; done < total;) {
        if (cancel.isCancelled())
            return makeError(ErrorCode::Cancelled, "stem analysis cancelled");
        count = static_cast<int>(std::min<std::int64_t>(kBlock, total - done));
        workers.run(n + 1, measure);
        workers.run(n, compare);

        done += count;
        if (options.progress && (done - lastProgress >= static_cast<std::int64_t>(rate * 5.0) || done == total)) {
            lastProgress = done;
            options.progress(static_cast<double>(done) / static_cast<double>(total));
        }
    }

    // ---- per-track results ----------------------------------------------------
    double energySum = 0.0;
    for (const TrackState& t : tracks)
        energySum += t.energy;
    const double binHz = rate / kBlock;
    std::vector<std::array<double, 3>> pairBandPower(n);

    for (std::size_t i = 0; i < n; ++i) {
        const Track& track = model.tracks[i];
        TrackState& t = tracks[i];
        TrackStemReport r;
        r.track = track.id;
        r.name = track.name;
        r.muted = track.muted;
        r.soloed = track.soloed;
        r.cleanup = track.cleanup;
        r.loudness = t.meter.reading();
        r.silent = t.energy <= 0.0;
        r.levelShare = energySum > 0.0 ? t.energy / energySum : 0.0;
        r.sourceOvers = countSourceOvers(track, media);
        r.dcOffset = (t.sumL + t.sumR) / (2.0 * static_cast<double>(total));
        if (t.sumLL > 0.0 && t.sumRR > 0.0)
            r.stereoCorrelation = std::clamp(t.sumLR / std::sqrt(t.sumLL * t.sumRR), -1.0, 1.0);
        r.sideRatio = t.midEnergy + t.sideEnergy > 0.0 ? t.sideEnergy / (t.midEnergy + t.sideEnergy) : 0.0;

        // Noise floor and activity from the 50 ms RMS frames.
        std::vector<float> sounding;
        sounding.reserve(t.frameDb.size());
        for (float db : t.frameDb)
            if (db > kDigitalSilence)
                sounding.push_back(db);
        const double frames = std::max<double>(1.0, static_cast<double>(t.frameDb.size()));
        r.silentFraction = static_cast<double>(t.frameDb.size() - sounding.size()) / frames;
        if (!sounding.empty()) {
            const double quiet = percentile(sounding, 0.10);
            const double loud = percentile(sounding, 0.95);
            if (quiet < kNoiseBedMaxDbfs && loud - quiet >= kNoiseBedMinSpreadDb)
                r.noiseFloorDbfs = quiet;
            // Above the bed is "playing". Once an expander runs, its threshold
            // is that line (the measured bed is then the expanded one).
            const bool expanding = track.cleanup.active() && track.cleanup.expander.enabled;
            const double threshold = expanding           ? std::max(track.cleanup.expander.thresholdDb, -70.0)
                                     : r.noiseFloorDbfs ? std::max(*r.noiseFloorDbfs + kExpanderMarginDb, -70.0)
                                                        : -60.0;
            const auto active = std::count_if(sounding.begin(), sounding.end(), [&](float db) { return db > threshold; });
            r.activeFraction = static_cast<double>(active) / frames;
        }

        // Spectrum.
        double power = 0.0, weighted = 0.0, low = 0.0;
        for (int k = 1; k <= kBlock / 2; ++k) {
            const double hz = k * binHz;
            const double pk = t.binPower[static_cast<std::size_t>(k)];
            power += pk;
            weighted += pk * hz;
            if (hz < kLowEndHz)
                low += pk;
            std::size_t band = kStemBandCount - 1;
            while (band > 0 && hz < kStemBandEdgesHz[band])
                --band;
            r.bandShare[band] += pk;
        }
        if (power > 0.0) {
            for (double& share : r.bandShare)
                share /= power;
            r.lowEndShare = low / power;
            r.centroidHz = weighted / power;
        }
        for (std::size_t band = 0; band < 3; ++band)
            for (int k = pairBins[band].first; k < pairBins[band].second; ++k)
                pairBandPower[i][band] += t.binPower[static_cast<std::size_t>(k)];

        // Attacks: the loud hits only (quiet ones are ghost notes and bleed).
        if (!t.hits.empty()) {
            float loudest = kAttackSilenceDb;
            for (const auto& hit : t.hits)
                loudest = std::max(loudest, hit[1]);
            std::vector<float> rises;
            for (const auto& hit : t.hits)
                if (hit[1] >= loudest - kAttackLoudRangeDb)
                    rises.push_back(hit[0]);
            r.hitsPerMinute = static_cast<double>(rises.size()) / (static_cast<double>(total) / rate / 60.0);
            if (rises.size() >= kAttackMinHits)
                r.attackMs = percentile(rises, 0.5);
        }

        // Resonances: the bin with the largest mean cut in each range (a bin
        // in both tapers belongs to the range that weighs it more).
        if (static_cast<double>(t.resonanceFrames) * kResonanceHop / rate >= kResonanceMinSeconds) {
            const double counted = static_cast<double>(t.resonanceFrames);
            for (int k = 0; k < t.resonance.bins(); ++k) {
                const float harsh = t.resonance.harshWeight(k);
                const float shimmer = t.resonance.shimmerWeight(k);
                if (harsh <= 0.0f && shimmer <= 0.0f)
                    continue;
                ResonanceFinding& finding = harsh >= shimmer ? r.harshness : r.shimmer;
                const auto bin = static_cast<std::size_t>(k);
                const double cut = t.resonanceCut[bin] / counted;
                if (cut > finding.cutDb || finding.hz == 0.0) {
                    const auto rings = static_cast<double>(t.resonanceRings[bin]);
                    finding = {k * t.resonance.binHz(), cut, rings / counted,
                               rings > 0.0 ? t.resonanceRingExcess[bin] / rings : 0.0};
                }
            }
        }
        report.tracks.push_back(std::move(r));
    }

    // ---- pairs ------------------------------------------------------------------
    std::size_t p = 0;
    for (std::size_t a = 0; a < n; ++a)
        for (std::size_t b = a + 1; b < n; ++b, ++p) {
            StemPairReport pr;
            pr.a = a;
            pr.b = b;
            double crossFull = 0.0, powerA = 0.0, powerB = 0.0;
            for (std::size_t band = 0; band < 3; ++band) {
                const double pa = pairBandPower[a][band];
                const double pb = pairBandPower[b][band];
                const double c = cross[p * 3 + band];
                pr.correlation[band] = pa > 0.0 && pb > 0.0 ? std::clamp(c / std::sqrt(pa * pb), -1.0, 1.0) : 0.0;
                crossFull += c;
                powerA += pa;
                powerB += pb;
            }
            pr.correlation[static_cast<std::size_t>(PairBand::Full)] =
                powerA > 0.0 && powerB > 0.0 ? std::clamp(crossFull / std::sqrt(powerA * powerB), -1.0, 1.0) : 0.0;
            report.pairs.push_back(pr);
        }

    report.mix = mixMeter.reading();
    report.advice = adviseStems(report);
    report.analysisSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return report;
}

std::vector<StemAdvice> adviseStems(const StemReport& report)
{
    std::vector<StemAdvice> advice;
    const auto& tracks = report.tracks;
    const auto name = [&](std::size_t i) { return tracks[i].name.c_str(); };
    const auto add = [&](AdviceSeverity severity, const char* code, std::optional<std::size_t> track,
                         std::optional<std::size_t> other, std::string message) {
        advice.push_back({severity, code, track, other, std::move(message)});
    };

    // ---- the mix --------------------------------------------------------------
    const double tp = report.mix.truePeakDbtp;
    if (tp > 0.0 || report.mixOvers > 0) {
        add(AdviceSeverity::Warning, "mix-clipping", std::nullopt, std::nullopt,
            format("The mix peaks at %+.1f dBTP and clips. Lower the Master by at least %.1f dB to reach the %.0f dBTP ceiling.",
                   tp, std::ceil((tp - kCeilingDbtp) * 10.0) / 10.0, kCeilingDbtp));
    } else if (tp > kCeilingDbtp) {
        add(AdviceSeverity::Suggestion, "mix-ceiling", std::nullopt, std::nullopt,
            format("The mix peaks at %+.1f dBTP, above the %.0f dBTP ceiling streaming services ask for. Lower the Master by %.1f dB.",
                   tp, kCeilingDbtp, std::ceil((tp - kCeilingDbtp) * 10.0) / 10.0));
    }
    if (report.mix.integratedLufs > -70.0)
        add(AdviceSeverity::Info, "mix-loudness", std::nullopt, std::nullopt,
            format("The mix measures %.1f LUFS integrated with a loudness range of %.1f LU. Streaming services play songs at "
                   "about -14 LUFS; club and techno masters are often between -9 and -6 LUFS.",
                   report.mix.integratedLufs, report.mix.loudnessRangeLu));

    // ---- each stem ------------------------------------------------------------
    std::optional<std::size_t> loudest;
    for (std::size_t i = 0; i < tracks.size(); ++i)
        if (!tracks[i].silent && (!loudest || tracks[i].loudness.integratedLufs > tracks[*loudest].loudness.integratedLufs))
            loudest = i;

    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const TrackStemReport& t = tracks[i];
        if (t.silent || t.loudness.integratedLufs <= -70.0) {
            add(AdviceSeverity::Suggestion, "silent", i, std::nullopt,
                format("%s is silent or nearly silent. It can be removed.", name(i)));
            continue;
        }

        if (loudest && *loudest != i) {
            const double below = tracks[*loudest].loudness.integratedLufs - t.loudness.integratedLufs;
            if (below >= kResidueBelowLoudestLu) {
                std::optional<std::size_t> twin;
                double twinCorrelation = 0.0;
                for (std::size_t j = 0; j < tracks.size(); ++j) {
                    const StemPairReport* pr = j != i ? report.pair(i, j) : nullptr;
                    const double c = pr != nullptr ? pr->correlation[static_cast<std::size_t>(PairBand::Full)] : 0.0;
                    if (c > twinCorrelation) {
                        twinCorrelation = c;
                        twin = j;
                    }
                }
                if (twin && twinCorrelation >= kResidueCorrelation)
                    add(AdviceSeverity::Suggestion, "residue", i, twin,
                        format("%s looks like separation leftovers: it is %.0f LU quieter than %s and mostly repeats what %s "
                               "plays (correlation %.2f). Mute it and listen whether anything is missing.",
                               name(i), below, name(*loudest), name(*twin), twinCorrelation));
                else
                    add(AdviceSeverity::Info, "quiet", i, std::nullopt,
                        format("%s is very quiet: %.0f LU below %s (%.1f%% of the total energy). Check that it adds something.",
                               name(i), below, name(*loudest), t.levelShare * 100.0));
            }
        }

        // Suno names the stem of whatever its separation could not assign "Other".
        const bool reportedQuiet = !advice.empty() && advice.back().track == i
                                   && (advice.back().code == "residue" || advice.back().code == "quiet");
        if (!reportedQuiet && containsWord(t.name, "other") && t.levelShare < kOtherMaxLevel)
            add(AdviceSeverity::Suggestion, "other-stem", i, std::nullopt,
                format("%s holds what the separation could not assign to another stem (%.1f%% of the mix, centred around "
                       "%.1f kHz). It is often residue and artifacts: mute it and listen whether anything is missing.",
                       name(i), t.levelShare * 100.0, t.centroidHz / 1000.0));

        // With an expander already running, the measured bed is the expanded
        // one: suggesting a threshold from it would chase its own tail.
        const bool expanding = t.cleanup.active() && t.cleanup.expander.enabled;
        if (!expanding && t.noiseFloorDbfs && *t.noiseFloorDbfs > kNoiseBedAudibleDbfs && t.activeFraction < 0.95) {
            const double threshold = std::round(*t.noiseFloorDbfs + kExpanderMarginDb);
            add(*t.noiseFloorDbfs > -60.0 ? AdviceSeverity::Suggestion : AdviceSeverity::Info, "noise-bed", i, std::nullopt,
                format("%s has a noise bed at %.0f dBFS between its notes (it plays %.0f%% of the time). A downward expander "
                       "with its threshold near %.0f dB will clean the gaps.",
                       name(i), *t.noiseFloorDbfs, t.activeFraction * 100.0, threshold));
            ExpanderSettings expander;
            expander.enabled = true;
            expander.thresholdDb = threshold;
            // Steep enough that the bed itself (kExpanderMarginDb under the
            // threshold) drops by the full range.
            expander.ratio = 1.0 + expander.rangeDb / kExpanderMarginDb;
            advice.back().fixes.push_back({.track = i, .expander = expander});
        }

        // AI-generated drums rise to their peak slowly: soft, smeared hits.
        const bool shaping = t.cleanup.active() && t.cleanup.transient.enabled;
        if (!shaping && isPercussionStem(t.name) && t.attackMs && *t.attackMs >= kSoftAttackMs && t.levelShare >= kAudibleShare) {
            add(AdviceSeverity::Suggestion, "soft-attacks", i, std::nullopt,
                format("%s: its hits take %.1f ms to rise, where a sampled drum takes about 1 ms, so they sound soft and "
                       "smeared. The transient shaper with %+.0f dB of attack brings the snap back.",
                       name(i), *t.attackMs, kSoftAttackGainDb));
            TransientSettings transient;
            transient.enabled = true;
            transient.attackDb = kSoftAttackGainDb;
            transient.sustainDb = 0.0;
            advice.back().fixes.push_back({.track = i, .transient = transient});
        }

        // Narrow peaks that ring through the song (2-6 kHz) and steady
        // generator tones (8-17.5 kHz).
        if (t.levelShare >= kAudibleShare) {
            const ResonanceSettings& current = t.cleanup.resonance;
            const bool suppressing = t.cleanup.active() && current.enabled;
            const auto heard = [&](const ResonanceFinding& f) {
                return f.cutDb >= kResonanceAdviceDb && bandShareOfMix(report, i, f.hz) >= kResonanceMinBandShare;
            };
            if (heard(t.harshness) && !(suppressing && current.harshDb > 0.0)) {
                add(AdviceSeverity::Suggestion, "harsh-resonance", i, std::nullopt,
                    format("%s: a resonance at %.1f kHz rings about %.0f dB above the sound around it during %.0f %% of "
                           "the song, which sounds harsh. Harshness control turns it down while it rings and leaves the "
                           "sound around it alone.",
                           name(i), t.harshness.hz / 1000.0, t.harshness.excessDb, t.harshness.ringingFraction * 100.0));
                ResonanceSettings fix;
                fix.enabled = true;
                fix.harshDb = kResonanceProbeDb;
                fix.shimmerDb = 0.0;
                advice.back().fixes.push_back({.track = i, .resonance = fix});
            }
            if (heard(t.shimmer) && !(suppressing && current.shimmerDb > 0.0)) {
                add(AdviceSeverity::Suggestion, "shimmer-tones", i, std::nullopt,
                    format("%s: a narrow tone at %.1f kHz stands about %.0f dB above the high end around it during "
                           "%.0f %% of the song, heard as a whine or a metallic shimmer. AI generators leave such tones "
                           "(often near 16 kHz). Shimmer control turns them down to the level around them.",
                           name(i), t.shimmer.hz / 1000.0, t.shimmer.excessDb, t.shimmer.ringingFraction * 100.0));
                ResonanceSettings fix;
                fix.enabled = true;
                fix.harshDb = 0.0;
                fix.shimmerDb = kResonanceProbeDb;
                advice.back().fixes.push_back({.track = i, .resonance = fix});
            }
        }

        if (t.sourceOvers > 0)
            add(AdviceSeverity::Info, "source-clipping", i, std::nullopt,
                format("%s has %lld samples at full scale in its source file: it was clipped when it was rendered.", name(i),
                       static_cast<long long>(t.sourceOvers)));

        if (std::abs(t.dcOffset) > kDcLimit)
            add(AdviceSeverity::Suggestion, "dc-offset", i, std::nullopt,
                format("%s has a DC offset (%.1f dBFS). It wastes headroom; DC removal fixes it.", name(i),
                       20.0 * std::log10(std::abs(t.dcOffset))));
        if (!advice.empty() && advice.back().code == "dc-offset" && advice.back().track == i)
            advice.back().fixes.push_back({.track = i, .dcRemoval = true});

        if (t.stereoCorrelation && *t.stereoCorrelation < kOutOfPhase)
            add(t.levelShare >= kAudibleShare ? AdviceSeverity::Warning : AdviceSeverity::Info, "out-of-phase", i, std::nullopt,
                format("%s has its left and right channels out of phase (correlation %.2f). It partly cancels on mono speakers.",
                       name(i), *t.stereoCorrelation));
        else if (t.stereoCorrelation && *t.stereoCorrelation > kMonoCorrelation)
            add(AdviceSeverity::Info, "mono", i, std::nullopt,
                format("%s is mono: both channels carry the same signal.", name(i)));
    }

    // ---- pairs ----------------------------------------------------------------
    // Stems sharing the same low end form groups (Suno may split one bass
    // line over "Bass", "Bass Guitar" and "Synth Bass"): one advice per group.
    std::vector<std::size_t> group(tracks.size());
    for (std::size_t i = 0; i < group.size(); ++i)
        group[i] = i;
    const auto root = [&](std::size_t i) {
        while (group[i] != i)
            i = group[i] = group[group[i]];
        return i;
    };
    std::vector<double> groupCorrelation(tracks.size(), 1.0); // weakest link per group root
    // A stem whose chain already high-passes near the low-end edge has given
    // up its low end: what is left is a filtered remainder, not a doubling.
    const auto lowEndRemoved = [&](std::size_t i) {
        const TrackCleanup& c = tracks[i].cleanup;
        return c.active() && c.highPass.enabled && c.highPass.frequencyHz >= kLowEndHz * 0.75;
    };
    const auto sharesLowEnd = [&](const StemPairReport& pr) {
        const TrackStemReport& a = tracks[pr.a];
        const TrackStemReport& b = tracks[pr.b];
        return !a.silent && !b.silent && !lowEndRemoved(pr.a) && !lowEndRemoved(pr.b) && pr.correlation[static_cast<std::size_t>(PairBand::Low)] >= kLowEndCorrelation
               && a.lowEndShare >= kLowEndMinShare && b.lowEndShare >= kLowEndMinShare;
    };
    for (const StemPairReport& pr : report.pairs)
        if (sharesLowEnd(pr)) {
            const std::size_t ra = root(pr.a), rb = root(pr.b);
            const double weakest = std::min({groupCorrelation[ra], groupCorrelation[rb],
                                             pr.correlation[static_cast<std::size_t>(PairBand::Low)]});
            group[std::max(ra, rb)] = std::min(ra, rb);
            groupCorrelation[std::min(ra, rb)] = weakest;
        }
    const auto lowEndCut = [] {
        HighPassSettings hp;
        hp.enabled = true;
        hp.frequencyHz = kLowEndHz;
        hp.slopeDbPerOctave = kLowEndSlope;
        return hp;
    };
    std::vector<bool> inLowEndGroup(tracks.size(), false);
    for (std::size_t r = 0; r < tracks.size(); ++r) {
        std::vector<std::size_t> members;
        for (std::size_t i = 0; i < tracks.size(); ++i)
            if (root(i) == r)
                members.push_back(i);
        if (members.size() < 2)
            continue;
        const std::size_t owner = *std::max_element(members.begin(), members.end(), [&](std::size_t x, std::size_t y) {
            return tracks[x].lowEndShare * tracks[x].levelShare < tracks[y].lowEndShare * tracks[y].levelShare;
        });
        std::vector<std::size_t> others;
        for (std::size_t m : members) {
            inLowEndGroup[m] = true;
            if (m != owner)
                others.push_back(m);
        }
        const auto list = [&](const std::vector<std::size_t>& items) {
            std::string text;
            for (std::size_t k = 0; k < items.size(); ++k)
                text += (k == 0 ? "" : k + 1 == items.size() ? " and " : ", ") + tracks[items[k]].name;
            return text;
        };
        add(AdviceSeverity::Suggestion, "shared-low-end", others.front(), owner,
            format("%s play the same low end (correlation %.2f or more below %.0f Hz): the kick or bass is doubled. Keep it "
                   "in %s and high-pass %s around %.0f Hz.",
                   list(members).c_str(), groupCorrelation[r], kLowEndHz, name(owner), list(others).c_str(), kLowEndHz));
        for (std::size_t m : others)
            advice.back().fixes.push_back({.track = m, .highPass = lowEndCut()});
    }

    for (const StemPairReport& pr : report.pairs) {
        if (tracks[pr.a].silent || tracks[pr.b].silent)
            continue;
        // Stems already reported as one doubled part need no bleed note.
        if (inLowEndGroup[pr.a] && root(pr.a) == root(pr.b))
            continue;
        const double mid = pr.correlation[static_cast<std::size_t>(PairBand::Mid)];
        if (mid >= kBleedCorrelation)
            add(AdviceSeverity::Info, "bleed", pr.a, pr.b,
                format("%s and %s overlap (correlation %.2f between %.0f Hz and %.0f kHz): parts of one leaked into the other "
                       "during separation, or they play the same part. Solo each to hear it; bleed suppression (Clean-up panel) "
                       "turns the leaked part down.",
                       name(pr.a), name(pr.b), mid, kLowEndHz, kHighBandHz / 1000.0));
    }

    // A small stem that is mostly low end under a bigger low-end stem usually
    // carries a second kick (Suno often splits the kick across Drums and
    // Percussion). Not covered by the correlation rule when the hits do not
    // line up in phase.
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const TrackStemReport& t = tracks[i];
        if (t.silent || t.lowEndShare < kExtraLowEndShare || t.levelShare >= kExtraLowEndMaxLevel || lowEndRemoved(i))
            continue;
        const bool covered = inLowEndGroup[i];
        std::optional<std::size_t> owner;
        for (std::size_t j = 0; j < tracks.size(); ++j)
            if (j != i && tracks[j].lowEndShare >= kLowEndMinShare * 2.0 && tracks[j].levelShare > 3.0 * t.levelShare
                && (!owner || tracks[j].lowEndShare * tracks[j].levelShare > tracks[*owner].lowEndShare * tracks[*owner].levelShare))
                owner = j;
        if (owner && !covered) {
            add(AdviceSeverity::Suggestion, "extra-low-end", i, owner,
                format("%s is mostly low end (%.0f%% below %.0f Hz) but adds only %.1f%% of the mix, under %s: it probably "
                       "carries a second kick or bass. High-pass it around %.0f Hz, or mute it and compare.",
                       name(i), t.lowEndShare * 100.0, kLowEndHz, t.levelShare * 100.0, name(*owner), kLowEndHz));
            advice.back().fixes.push_back({.track = i, .highPass = lowEndCut()});
        }
    }

    std::stable_sort(advice.begin(), advice.end(),
                     [](const StemAdvice& x, const StemAdvice& y) { return x.severity < y.severity; });
    return advice;
}

std::vector<CleanupFix> allCleanupFixes(const StemReport& report)
{
    std::vector<CleanupFix> fixes;
    for (const StemAdvice& a : report.advice)
        fixes.insert(fixes.end(), a.fixes.begin(), a.fixes.end());
    return fixes;
}

std::size_t applyCleanupFixes(ProjectModel& model, const StemReport& report, const std::vector<CleanupFix>& fixes)
{
    std::vector<TrackId> changed;
    for (const CleanupFix& fix : fixes) {
        if (fix.track >= report.tracks.size())
            continue;
        Track* track = model.findTrack(report.tracks[fix.track].track);
        if (track == nullptr)
            continue;
        const TrackCleanup next = fix.applyTo(track->cleanup);
        if (next == track->cleanup)
            continue;
        track->cleanup = next;
        if (std::find(changed.begin(), changed.end(), track->id) == changed.end())
            changed.push_back(track->id);
    }
    return changed.size();
}

std::string describeCleanup(const TrackCleanup& cleanup)
{
    std::string text;
    const auto append = [&](const std::string& part) { text += (text.empty() ? "" : ", ") + part; };
    if (cleanup.highPass.enabled)
        append(format("HP %.0f Hz %d dB/oct", cleanup.highPass.frequencyHz, cleanup.highPass.slopeDbPerOctave));
    if (cleanup.hum.enabled)
        append(format("hum %.0f Hz", cleanup.hum.frequencyHz));
    if (cleanup.denoise.enabled)
        append(cleanup.denoise.profile.empty() ? std::string("denoise (no profile)")
                                               : format("denoise %.0f dB", cleanup.denoise.reductionDb));
    if (cleanup.expander.enabled)
        append(format("expander %.0f dB", cleanup.expander.thresholdDb));
    if (cleanup.resonance.enabled) {
        if (cleanup.resonance.harshDb > 0.0)
            append(format("harshness %.0f dB", cleanup.resonance.harshDb));
        if (cleanup.resonance.shimmerDb > 0.0)
            append(format("shimmer %.0f dB", cleanup.resonance.shimmerDb));
    }
    if (cleanup.transient.enabled)
        append(format("transients %+.0f/%+.0f dB", cleanup.transient.attackDb, cleanup.transient.sustainDb));
    if (cleanup.dcRemoval)
        append("DC");
    if (!text.empty() && cleanup.bypassed)
        text += " (bypassed)";
    return text;
}

nlohmann::json toJson(const StemReport& report)
{
    using nlohmann::json;
    auto loudness = [](const LoudnessReading& r) {
        return json{{"integratedLufs", r.integratedLufs},   {"loudnessRangeLu", r.loudnessRangeLu},
                    {"maxMomentaryLufs", r.maxMomentaryLufs}, {"maxShortTermLufs", r.maxShortTermLufs},
                    {"truePeakDbtp", r.truePeakDbtp},       {"samplePeakDbfs", r.samplePeakDbfs}};
    };
    auto resonance = [](const ResonanceFinding& f) {
        return f.hz > 0.0 ? json{{"hz", f.hz}, {"cutDb", f.cutDb}, {"ringingFraction", f.ringingFraction},
                                 {"excessDb", f.excessDb}}
                          : json(nullptr);
    };
    json tracks = json::array();
    for (const TrackStemReport& t : report.tracks) {
        json bands = json::object();
        for (std::size_t b = 0; b < kStemBandCount; ++b)
            bands[toString(static_cast<StemBand>(b))] = t.bandShare[b];
        tracks.push_back({{"trackId", t.track.str()},
                          {"name", t.name},
                          {"muted", t.muted},
                          {"soloed", t.soloed},
                          {"silent", t.silent},
                          {"loudness", loudness(t.loudness)},
                          {"levelShare", t.levelShare},
                          {"activeFraction", t.activeFraction},
                          {"silentFraction", t.silentFraction},
                          {"noiseFloorDbfs", t.noiseFloorDbfs ? json(*t.noiseFloorDbfs) : json(nullptr)},
                          {"sourceOvers", t.sourceOvers},
                          {"dcOffset", t.dcOffset},
                          {"stereoCorrelation", t.stereoCorrelation ? json(*t.stereoCorrelation) : json(nullptr)},
                          {"hitsPerMinute", t.hitsPerMinute},
                          {"attackMs", t.attackMs ? json(*t.attackMs) : json(nullptr)},
                          {"harshness", resonance(t.harshness)},
                          {"shimmer", resonance(t.shimmer)},
                          {"sideRatio", t.sideRatio},
                          {"bandShare", bands},
                          {"lowEndShare", t.lowEndShare},
                          {"centroidHz", t.centroidHz},
                          {"cleanup", cleanupToJson(t.cleanup)}});
    }
    json pairs = json::array();
    for (const StemPairReport& p : report.pairs)
        pairs.push_back({{"a", p.a},
                         {"b", p.b},
                         {"low", p.correlation[0]},
                         {"mid", p.correlation[1]},
                         {"high", p.correlation[2]},
                         {"full", p.correlation[3]}});
    json advice = json::array();
    for (const StemAdvice& a : report.advice) {
        json item{{"severity", toString(a.severity)}, {"code", a.code}, {"message", a.message}};
        if (a.track)
            item["track"] = *a.track;
        if (a.otherTrack)
            item["otherTrack"] = *a.otherTrack;
        if (!a.fixes.empty()) {
            json fixes = json::array();
            for (const CleanupFix& f : a.fixes) {
                // The fix applied to an empty chain; only the stages it sets are listed.
                const json full = cleanupToJson(f.applyTo({}));
                json fix{{"track", f.track}};
                if (f.dcRemoval)
                    fix["dcRemoval"] = *f.dcRemoval;
                if (f.highPass)
                    fix["highPass"] = full["highPass"];
                if (f.expander)
                    fix["expander"] = full["expander"];
                if (f.transient)
                    fix["transient"] = full["transient"];
                if (f.resonance)
                    fix["resonance"] = full["resonance"];
                fixes.push_back(std::move(fix));
            }
            item["fixes"] = std::move(fixes);
        }
        advice.push_back(std::move(item));
    }
    return {{"format", "stem-dawg-stem-report"},
            {"version", 1},
            {"sampleRate", report.sampleRate},
            {"frames", report.frames},
            {"analysisSeconds", report.analysisSeconds},
            {"mix", loudness(report.mix)},
            {"mixOvers", report.mixOvers},
            {"tracks", tracks},
            {"pairs", pairs},
            {"advice", advice}};
}

} // namespace daw
