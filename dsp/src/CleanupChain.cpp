// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/CleanupChain.h>

#include <daw/core/Denormals.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw {

namespace {

double timeCoeff(double milliseconds, double sampleRate) noexcept
{
    const double frames = milliseconds * 0.001 * sampleRate;
    return frames <= 0.0 ? 0.0 : std::exp(-1.0 / frames);
}

// Butterworth high-pass section (RBJ cookbook form, exact cutoff after the
// bilinear transform).
BiquadCoefficients highPassSection(double hz, double q, double sampleRate) noexcept
{
    const double w0 = 2.0 * std::numbers::pi * hz / sampleRate;
    const double cosW = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    BiquadCoefficients c;
    c.b0 = (1.0 + cosW) / 2.0 / a0;
    c.b1 = -(1.0 + cosW) / a0;
    c.b2 = c.b0;
    c.a1 = -2.0 * cosW / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

} // namespace

void CleanupChain::prepare(double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    lookahead_ = std::max(1, static_cast<int>(std::lround(sampleRate_ * kLookaheadSeconds)));
    latency_ = SpectralDenoiser::latency() + lookahead_;
    delay_.assign(static_cast<std::size_t>(lookahead_) * 2, 0.0f);
    fftDelay_.assign(static_cast<std::size_t>(SpectralDenoiser::latency()) * 2, 0.0f);
    dryRing_.assign(static_cast<std::size_t>(latency_) * 2, 0.0f);
    dryOut_.assign(static_cast<std::size_t>(latency_) * 2, 0.0f);
    hum_.prepare(sampleRate_);
    denoiser_.prepare();
    dcR_ = std::exp(-2.0 * std::numbers::pi * kDcCutoffHz / sampleRate_);
    detectorCoeff_ = timeCoeff(kDetectorMs, sampleRate_);
    attackCoeff_ = timeCoeff(kAttackMs, sampleRate_);
    holdFrames_ = static_cast<int>(std::lround(kHoldMs * 0.001 * sampleRate_));
    CleanupParams params = params_;
    params.noiseProfile = nullptr; // owned by a snapshot that may be gone; the next setParams brings it back
    params_ = {};
    sections_ = 0;
    setParams(params);
    reset();
}

void CleanupChain::setParams(const CleanupParams& params) noexcept
{
    const bool filterChanged = params.highPass != params_.highPass || params.highPassOrder != params_.highPassOrder;
    params_ = params;

    const int order = params.highPassOrder <= 2 ? 2 : params.highPassOrder <= 4 ? 4 : 8;
    const int sections = params.highPass ? order / 2 : 0;
    const double hz = std::clamp(static_cast<double>(params.highPassHz), 1.0, 0.45 * sampleRate_);
    for (int k = 0; k < sections; ++k) {
        // Butterworth pole pairs: Q = 1 / (2 cos(theta)), theta = pi (2k + 1) / (2 order).
        const double theta = std::numbers::pi * (2.0 * k + 1.0) / (2.0 * order);
        const BiquadCoefficients c = highPassSection(hz, 1.0 / (2.0 * std::cos(theta)), sampleRate_);
        for (auto& channel : hp_)
            channel[static_cast<std::size_t>(k)].setCoefficients(c);
    }
    sections_ = sections;
    if (filterChanged)
        for (auto& channel : hp_)
            for (Biquad& section : channel)
                section.reset();

    hum_.setParams(params.hum ? params.humHz : 0.0, params.hum ? params.humHarmonics : 0);
    denoiser_.setParams({params.denoiseReductionDb, params.denoiseSensitivityDb});
    denoiser_.setNoiseProfile(params.noiseProfile);

    releaseCoeff_ = timeCoeff(std::max(1.0f, params.releaseMs), sampleRate_);
    // Above threshold + half the knee the curve is flat: the per-sample gain
    // math is skipped there (most of the time while music plays).
    openPower_ = std::pow(10.0, (static_cast<double>(params.thresholdDb) + kKneeDb / 2.0) / 10.0);
}

void CleanupChain::reset() noexcept
{
    dcX_.fill(0.0);
    dcY_.fill(0.0);
    for (auto& channel : hp_)
        for (Biquad& section : channel)
            section.reset();
    power_ = 0.0;
    gainDb_ = 0.0;
    gain_ = 1.0;
    minGainDb_ = 0.0;
    hold_ = 0;
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    delayPos_ = 0;
    hum_.reset();
    denoiser_.reset();
    std::fill(fftDelay_.begin(), fftDelay_.end(), 0.0f);
    fftDelayPos_ = 0;
    std::fill(dryRing_.begin(), dryRing_.end(), 0.0f);
    dryPos_ = 0;
}

double CleanupChain::expanderGainDb(double levelDb, double thresholdDb, double ratio, double rangeDb) noexcept
{
    const double slope = std::max(0.0, ratio - 1.0);
    const double halfKnee = kKneeDb / 2.0;
    double reduction = 0.0;
    if (levelDb >= thresholdDb + halfKnee)
        reduction = 0.0;
    else if (levelDb > thresholdDb - halfKnee) {
        const double d = thresholdDb + halfKnee - levelDb;
        reduction = slope * d * d / (2.0 * kKneeDb);
    } else
        reduction = slope * (thresholdDb - levelDb);
    return -std::min(reduction, std::max(0.0, rangeDb));
}

void CleanupChain::process(float* left, float* right, int numFrames) noexcept
{
    if (latency_ <= 0 || numFrames <= 0)
        return;
    ScopedFlushDenormals noDenormals;
    // Chunks no longer than the latency, so the dry scratch always suffices.
    for (int done = 0; done < numFrames;) {
        const int n = std::min(numFrames - done, latency_);
        processChunk(left + done, right + done, n);
        done += n;
    }
}

void CleanupChain::processChunk(float* left, float* right, int numFrames) noexcept
{
    float* channels[2] = {left, right};

    // The input, delayed by the whole chain, for "listen to removed".
    float* dryLeft = dryOut_.data();
    float* dryRight = dryLeft + latency_;
    for (int i = 0; i < numFrames; ++i) {
        float* slot = dryRing_.data() + 2 * dryPos_;
        dryLeft[i] = slot[0];
        dryRight[i] = slot[1];
        slot[0] = left[i];
        slot[1] = right[i];
        if (++dryPos_ == latency_)
            dryPos_ = 0;
    }

    // DC removal and high-pass, in place.
    for (int c = 0; c < 2; ++c) {
        float* x = channels[c];
        if (params_.dcRemoval) {
            double x1 = dcX_[static_cast<std::size_t>(c)], y1 = dcY_[static_cast<std::size_t>(c)];
            for (int i = 0; i < numFrames; ++i) {
                const double in = x[i];
                y1 = in - x1 + dcR_ * y1;
                x1 = in;
                x[i] = static_cast<float>(y1);
            }
            dcX_[static_cast<std::size_t>(c)] = x1;
            dcY_[static_cast<std::size_t>(c)] = y1;
        }
        for (int k = 0; k < sections_; ++k) {
            Biquad& section = hp_[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)];
            for (int i = 0; i < numFrames; ++i)
                x[i] = static_cast<float>(section.process(x[i]));
        }
    }

    hum_.process(left, right, numFrames);

    // Noise reduction, or the same delay without it.
    if (params_.denoise) {
        denoiser_.process(left, right, numFrames);
    } else {
        const int length = SpectralDenoiser::latency();
        float* line = fftDelay_.data();
        for (int i = 0; i < numFrames; ++i) {
            float* slot = line + 2 * fftDelayPos_;
            const float l = slot[0], r = slot[1];
            slot[0] = left[i];
            slot[1] = right[i];
            left[i] = l;
            right[i] = r;
            if (++fftDelayPos_ == length)
                fftDelayPos_ = 0;
        }
    }

    // Expander (gain from the undelayed signal) and the lookahead delay.
    const bool expand = params_.expander;
    const double threshold = params_.thresholdDb;
    const double ratio = params_.ratio;
    const double range = params_.rangeDb;
    float* delay = delay_.data();
    for (int i = 0; i < numFrames; ++i) {
        const float l = left[i];
        const float r = right[i];
        if (expand) {
            const double p = 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);
            power_ = p + detectorCoeff_ * (power_ - p);
            if (power_ >= openPower_ && gainDb_ == 0.0) {
                hold_ = holdFrames_; // fully open: nothing to compute
            } else {
                const double levelDb = 10.0 * std::log10(power_ + 1e-30);
                const double target = expanderGainDb(levelDb, threshold, ratio, range);
                if (target >= gainDb_) {
                    gainDb_ = target + (gainDb_ - target) * attackCoeff_;
                    if (gainDb_ > -1e-4)
                        gainDb_ = 0.0;
                    hold_ = holdFrames_;
                } else if (hold_ > 0) {
                    --hold_;
                } else {
                    gainDb_ = target + (gainDb_ - target) * releaseCoeff_;
                }
                gain_ = gainDb_ == 0.0 ? 1.0 : std::pow(10.0, gainDb_ / 20.0);
                minGainDb_ = std::min(minGainDb_, gainDb_);
            }
        } else if (gainDb_ != 0.0) {
            // Switched off while attenuating: open smoothly instead of jumping.
            gainDb_ *= attackCoeff_;
            if (gainDb_ > -1e-4)
                gainDb_ = 0.0;
            gain_ = std::pow(10.0, gainDb_ / 20.0);
        }

        float* slot = delay + 2 * delayPos_;
        const float g = static_cast<float>(gain_);
        left[i] = slot[0] * g;
        right[i] = slot[1] * g;
        slot[0] = l;
        slot[1] = r;
        if (++delayPos_ == lookahead_)
            delayPos_ = 0;
    }

    if (params_.listenRemoved)
        for (int i = 0; i < numFrames; ++i) {
            left[i] = dryLeft[i] - left[i];
            right[i] = dryRight[i] - right[i];
        }
}

} // namespace daw
