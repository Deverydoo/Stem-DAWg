// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/LoudnessMeter.h>

#include <daw/core/Denormals.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw {

namespace {

constexpr double kHistogramMin = -70.0; // absolute gate: only blocks above it are stored
constexpr double kHistogramMax = 10.0;
constexpr double kBinWidth = 0.02;
constexpr int kBins = static_cast<int>((kHistogramMax - kHistogramMin) / kBinWidth + 0.5);
constexpr double kAbsoluteGate = -70.0;

double toLufs(double meanSquare) noexcept
{
    if (!(meanSquare > 0.0))
        return kLoudnessFloor;
    return std::max(kLoudnessFloor, -0.691 + 10.0 * std::log10(meanSquare));
}

double toDb(float amplitude) noexcept
{
    if (!(amplitude > 0.0f))
        return kLoudnessFloor;
    return std::max(kLoudnessFloor, 20.0 * std::log10(static_cast<double>(amplitude)));
}

/// First bin whose centre lies above `gate`.
int firstBinAbove(double gate) noexcept
{
    const double position = (gate - kHistogramMin) / kBinWidth - 0.5;
    if (position < 0.0)
        return 0;
    return std::min(kBins, static_cast<int>(std::floor(position)) + 1);
}

double binCentre(int bin) noexcept
{
    return kHistogramMin + (bin + 0.5) * kBinWidth;
}

} // namespace

std::array<BiquadCoefficients, 2> kWeightingFilters(double rate)
{
    // Analogue prototype parameters fitted to the 48 kHz coefficients of
    // BS.1770-4 and re-discretised with the bilinear transform, so the filter
    // is correct at every sample rate (the same parameters are used by the
    // libebur128 reference implementation).
    std::array<BiquadCoefficients, 2> stages;
    {
        const double f0 = 1681.974450955533;
        const double gainDb = 3.999843853973347;
        const double q = 0.7071752369554196;
        const double k = std::tan(std::numbers::pi * f0 / rate);
        const double vh = std::pow(10.0, gainDb / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        stages[0] = {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                     2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
    }
    {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;
        const double k = std::tan(std::numbers::pi * f0 / rate);
        const double a0 = 1.0 + k / q + k * k;
        stages[1] = {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
    }
    return stages;
}

void LoudnessMeter::Histogram::prepare()
{
    energy.assign(static_cast<std::size_t>(kBins), 0.0);
    count.assign(static_cast<std::size_t>(kBins), 0);
    totalEnergy = 0.0;
    totalCount = 0;
}

void LoudnessMeter::Histogram::clear() noexcept
{
    std::fill(energy.begin(), energy.end(), 0.0);
    std::fill(count.begin(), count.end(), 0u);
    totalEnergy = 0.0;
    totalCount = 0;
}

void LoudnessMeter::Histogram::add(double meanSquare, double lufs) noexcept
{
    const int bin = std::clamp(static_cast<int>(std::floor((lufs - kHistogramMin) / kBinWidth)), 0, kBins - 1);
    energy[static_cast<std::size_t>(bin)] += meanSquare;
    ++count[static_cast<std::size_t>(bin)];
    totalEnergy += meanSquare;
    ++totalCount;
}

void LoudnessMeter::prepare(double sampleRate, int numChannels, bool truePeak)
{
    measureTruePeak_ = truePeak;
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    channels_ = std::clamp(numChannels, 0, 8);
    stepFrames_ = std::max(1, static_cast<int>(std::lround(sampleRate_ * 0.1)));
    const auto stages = kWeightingFilters(sampleRate_);
    filters_.assign(static_cast<std::size_t>(channels_ * 2), Biquad{});
    for (int c = 0; c < channels_; ++c) {
        filters_[static_cast<std::size_t>(2 * c)].setCoefficients(stages[0]);
        filters_[static_cast<std::size_t>(2 * c + 1)].setCoefficients(stages[1]);
    }
    truePeak_.prepare(sampleRate_, truePeak ? channels_ : 0);
    blocks_.prepare();
    shortTerm_.prepare();
    reset();
}

void LoudnessMeter::reset() noexcept
{
    for (Biquad& f : filters_)
        f.reset();
    truePeak_.reset();
    stepEnergy_ = 0.0;
    stepPosition_ = 0;
    steps_.fill(0.0);
    stepIndex_ = 0;
    stepsDone_ = 0;
    frames_ = 0;
    samplePeak_ = 0.0f;
    truePeakValue_ = 0.0f;
    blocks_.clear();
    shortTerm_.clear();
    reading_ = {};
}

bool LoudnessMeter::process(const float* const* channels, int numChannels, int numFrames) noexcept
{
    if (numFrames <= 0 || channels_ == 0)
        return false;
    // The K-weighting filters decay into denormals during silence, which are
    // very slow on x86. Offline callers do not run inside the engine's guard.
    ScopedFlushDenormals noDenormals;
    const int count = std::min(numChannels, channels_);
    if (measureTruePeak_)
        truePeakValue_ = std::max(truePeakValue_, truePeak_.process(channels, count, numFrames));

    bool stepped = false;
    for (int done = 0; done < numFrames;) {
        const int chunk = std::min(numFrames - done, stepFrames_ - stepPosition_);
        for (int c = 0; c < count; ++c) {
            const float* in = channels[c];
            if (in == nullptr)
                continue;
            Biquad& shelf = filters_[static_cast<std::size_t>(2 * c)];
            Biquad& highPass = filters_[static_cast<std::size_t>(2 * c + 1)];
            double sum = 0.0;
            float peak = samplePeak_;
            for (int i = done; i < done + chunk; ++i) {
                const float x = in[i];
                peak = std::max(peak, std::abs(x));
                const double y = highPass.process(shelf.process(static_cast<double>(x)));
                sum += y * y;
            }
            samplePeak_ = peak;
            stepEnergy_ += sum;
        }
        stepPosition_ += chunk;
        frames_ += chunk;
        done += chunk;
        if (stepPosition_ == stepFrames_) {
            finishStep();
            stepped = true;
        }
    }

    reading_.samplePeakDbfs = toDb(samplePeak_);
    reading_.truePeakDbtp = toDb(std::max(truePeakValue_, samplePeak_));
    reading_.seconds = static_cast<double>(frames_) / sampleRate_;
    return stepped;
}

void LoudnessMeter::finishStep() noexcept
{
    steps_[static_cast<std::size_t>(stepIndex_)] = stepEnergy_;
    stepIndex_ = (stepIndex_ + 1) % kShortTermSteps;
    stepEnergy_ = 0.0;
    stepPosition_ = 0;
    ++stepsDone_;

    double momentary = 0.0;
    for (int k = 1; k <= kMomentarySteps; ++k)
        momentary += steps_[static_cast<std::size_t>((stepIndex_ - k + kShortTermSteps) % kShortTermSteps)];
    momentary /= static_cast<double>(kMomentarySteps) * stepFrames_;
    double shortTerm = 0.0;
    for (double e : steps_)
        shortTerm += e;
    shortTerm /= static_cast<double>(kShortTermSteps) * stepFrames_;

    reading_.momentaryLufs = toLufs(momentary);
    reading_.shortTermLufs = toLufs(shortTerm);

    // Only complete windows enter the statistics (no zero padding at the start).
    if (stepsDone_ >= static_cast<std::uint64_t>(kMomentarySteps)) {
        reading_.maxMomentaryLufs = std::max(reading_.maxMomentaryLufs, reading_.momentaryLufs);
        if (reading_.momentaryLufs > kAbsoluteGate) {
            blocks_.add(momentary, reading_.momentaryLufs);
            reading_.integratedLufs = integrated();
        }
    }
    if (stepsDone_ >= static_cast<std::uint64_t>(kShortTermSteps)) {
        reading_.maxShortTermLufs = std::max(reading_.maxShortTermLufs, reading_.shortTermLufs);
        if (reading_.shortTermLufs > kAbsoluteGate) {
            shortTerm_.add(shortTerm, reading_.shortTermLufs);
            reading_.loudnessRangeLu = loudnessRange();
        }
    }
}

double LoudnessMeter::integrated() const noexcept
{
    if (blocks_.totalCount == 0)
        return kLoudnessFloor;
    const double relativeGate = toLufs(blocks_.totalEnergy / static_cast<double>(blocks_.totalCount)) - 10.0;
    double energy = 0.0;
    std::uint64_t count = 0;
    for (int bin = firstBinAbove(relativeGate); bin < kBins; ++bin) {
        energy += blocks_.energy[static_cast<std::size_t>(bin)];
        count += blocks_.count[static_cast<std::size_t>(bin)];
    }
    return count > 0 ? toLufs(energy / static_cast<double>(count)) : kLoudnessFloor;
}

double LoudnessMeter::loudnessRange() const noexcept
{
    if (shortTerm_.totalCount == 0)
        return 0.0;
    const double relativeGate = toLufs(shortTerm_.totalEnergy / static_cast<double>(shortTerm_.totalCount)) - 20.0;
    const int first = firstBinAbove(relativeGate);
    std::uint64_t total = 0;
    for (int bin = first; bin < kBins; ++bin)
        total += shortTerm_.count[static_cast<std::size_t>(bin)];
    if (total == 0)
        return 0.0;

    auto percentile = [&](double p) {
        const double target = std::max(1.0, p * static_cast<double>(total));
        std::uint64_t cumulative = 0;
        for (int bin = first; bin < kBins; ++bin) {
            cumulative += shortTerm_.count[static_cast<std::size_t>(bin)];
            if (static_cast<double>(cumulative) >= target)
                return binCentre(bin);
        }
        return binCentre(kBins - 1);
    };
    return std::max(0.0, percentile(0.95) - percentile(0.10));
}

} // namespace daw
