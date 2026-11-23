// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/TransientShaper.h>

#include <algorithm>
#include <cmath>

namespace daw {

namespace {

double timeCoeff(double milliseconds, double sampleRate) noexcept
{
    const double frames = milliseconds * 0.001 * sampleRate;
    return frames <= 0.0 ? 0.0 : std::exp(-1.0 / frames);
}

constexpr double kSilence = 1e-6; // -120 dBFS: below this the ratios mean nothing

} // namespace

double TransientShaper::Boxcar::push(double power) noexcept
{
    double& oldest = ring[static_cast<std::size_t>(pos)];
    sum += power - oldest;
    oldest = power;
    if (++pos == static_cast<int>(ring.size()))
        pos = 0;
    return std::sqrt(std::max(0.0, sum) / static_cast<double>(ring.size()));
}

void TransientShaper::Boxcar::clear() noexcept
{
    std::fill(ring.begin(), ring.end(), 0.0);
    pos = 0;
    sum = 0.0;
}

void TransientShaper::prepare(double sampleRate, int lookahead)
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    fastRelease_ = timeCoeff(kFastReleaseMs, rate);
    riseLag_ = timeCoeff(kRiseLagMs, rate);
    softenLag_ = timeCoeff(kSoftenLagMs, rate);
    gateLag_ = timeCoeff(kGateLagMs, rate);
    rms_ = timeCoeff(kRmsMs, rate);
    slowRelease_ = timeCoeff(kSlowReleaseMs / 2.0, rate); // on power: half the amplitude time constant
    smoothing_ = timeCoeff(kSmoothingMs, rate);
    const BiquadCoefficients low = BiquadCoefficients::lowPass(std::min(kSplitHz, 0.4 * rate), 0.70710678118654752, rate);
    for (Biquad& b : detectLow_)
        b.setCoefficients(low);
    for (Biquad& b : outputLow_)
        b.setCoefficients(low);
    lookahead_ = std::max(1, lookahead);
    riseLead_ = std::clamp(static_cast<int>(std::lround(kRiseLeadMs * 0.001 * rate)), 0, lookahead_);
    sustainLead_ = std::clamp(static_cast<int>(std::lround(kSustainLeadMs * 0.001 * rate)), 0, lookahead_);
    for (auto [box, ms] : {std::pair{&level_, kLevelMs}, std::pair{&peakLevel_, kPeakLevelMs}}) {
        const int frames = std::max(1, static_cast<int>(std::lround(ms * 0.001 * rate)));
        box->ring.assign(static_cast<std::size_t>(frames), 0.0);
        box->centre = std::min(frames / 2, lookahead_);
    }
    ring_.assign(static_cast<std::size_t>(lookahead_) + 1, Entry{});
    maxQueue_.assign(static_cast<std::size_t>(lookahead_) + 1, 0);
    reset();
}

void TransientShaper::setParams(const TransientParams& params) noexcept
{
    params_.attackDb = std::isfinite(params.attackDb) ? std::clamp(params.attackDb, -kMaxDb, kMaxDb) : 0.0f;
    params_.sustainDb = std::isfinite(params.sustainDb) ? std::clamp(params.sustainDb, -kMaxDb, kMaxDb) : 0.0f;
}

void TransientShaper::reset() noexcept
{
    level_.clear();
    peakLevel_.clear();
    fast_ = lagged_ = laggedLong_ = fastAll_ = laggedAll_ = power_ = heldPower_ = 0.0;
    for (Biquad& b : detectLow_)
        b.reset();
    for (Biquad& b : outputLow_)
        b.reset();
    attackDb_ = sustainDb_ = 0.0;
    std::fill(ring_.begin(), ring_.end(), Entry{});
    count_ = 0;
    queueHead_ = queueSize_ = 0;
}

void TransientShaper::apply(float& left, float& right, const Gains& gains) noexcept
{
    const double l = left, r = right;
    const double lowL = outputLow_[0].process(l);
    const double lowR = outputLow_[1].process(r);
    if (gains.high == 1.0f && gains.all == 1.0f)
        return;
    left = static_cast<float>(gains.all * (lowL + gains.high * (l - lowL)));
    right = static_cast<float>(gains.all * (lowR + gains.high * (r - lowR)));
}

TransientShaper::Gains TransientShaper::process(float left, float right, bool enabled) noexcept
{
    const int size = lookahead_ + 1;

    // Envelopes of the newest sample.
    const double highL = left - detectLow_[0].process(left);
    const double highR = right - detectLow_[1].process(right);
    const double highP = 0.5 * (highL * highL + highR * highR);
    const double x = level_.push(highP);
    const double wide = peakLevel_.push(highP);
    fast_ = x;
    lagged_ = fast_ + (lagged_ - fast_) * riseLag_;
    laggedLong_ = fast_ + (laggedLong_ - fast_) * softenLag_;
    const double xAll = std::max(std::abs(static_cast<double>(left)), std::abs(static_cast<double>(right)));
    fastAll_ = xAll > fastAll_ ? xAll : xAll + (fastAll_ - xAll) * fastRelease_;
    laggedAll_ = fastAll_ + (laggedAll_ - fastAll_) * gateLag_;
    const double p = 0.5 * (static_cast<double>(left) * left + static_cast<double>(right) * right);
    power_ = p + (power_ - p) * rms_;
    heldPower_ = std::max(power_, heldPower_ * slowRelease_);

    Entry entry;
    entry.fast = static_cast<float>(fast_);
    entry.wide = static_cast<float>(wide);
    if (fast_ > kSilence) {
        const double rise = fast_ > lagged_ ? 20.0 * std::log10(fast_ / std::max(lagged_, kSilence)) : 0.0;
        entry.rise = static_cast<float>(std::clamp((rise - kRiseDeadZoneDb) / kRiseRangeDb, 0.0, 1.0));
        const double above = fast_ > laggedLong_ ? 20.0 * std::log10(fast_ / std::max(laggedLong_, kSilence)) : 0.0;
        entry.soften = static_cast<float>(std::clamp((above - kSoftenDeadZoneDb) / kSoftenRangeDb, 0.0, 1.0));
    }
    if (fastAll_ > kSilence) {
        // R reacts to a new hit within milliseconds, not at once: no sustain
        // gain while the level is still rising.
        const double rise = fastAll_ > laggedAll_ ? 20.0 * std::log10(fastAll_ / std::max(laggedAll_, kSilence)) : 0.0;
        if (rise <= kGateDeadZoneDb && heldPower_ > power_ && power_ > kSilence * kSilence) {
            const double fall = 10.0 * std::log10(heldPower_ / power_);
            entry.sustain = static_cast<float>(std::clamp((fall - kSustainDeadZoneDb) / kSustainRangeDb, 0.0, 1.0));
        }
    }
    const std::int64_t now = count_++;
    ring_[static_cast<std::size_t>(now % size)] = entry;

    // Sliding maximum of the wide level over the lookahead.
    while (queueSize_ > 0) {
        const int back = (queueHead_ + queueSize_ - 1) % size;
        const std::int64_t sample = maxQueue_[static_cast<std::size_t>(back)];
        if (ring_[static_cast<std::size_t>(sample % size)].wide > entry.wide)
            break;
        --queueSize_;
    }
    maxQueue_[static_cast<std::size_t>((queueHead_ + queueSize_) % size)] = now;
    ++queueSize_;
    const std::int64_t heard = now - lookahead_;
    // The level describing the heard sample sits level_.centre samples later;
    // the peak to come is the highest wide level describing it or later.
    const std::int64_t levelOfHeard = heard + level_.centre;
    while (queueSize_ > 1 && maxQueue_[static_cast<std::size_t>(queueHead_)] < heard + peakLevel_.centre) {
        queueHead_ = queueHead_ + 1 == size ? 0 : queueHead_ + 1;
        --queueSize_;
    }

    double attackTarget = 0.0;
    double sustainTarget = 0.0;
    double headroomDb = kMaxDb; // how far the heard sample may be lifted before it passes the coming peak
    if (enabled && heard >= 0) {
        const int heardIndex = static_cast<int>(heard % size);
        const Entry& h = ring_[static_cast<std::size_t>(levelOfHeard % size)];
        const Entry& ahead = ring_[static_cast<std::size_t>((heardIndex + riseLead_) % size)];
        if (params_.attackDb > 0.0f && h.fast > kSilence) {
            const double peak = ring_[static_cast<std::size_t>(maxQueue_[static_cast<std::size_t>(queueHead_)] % size)].wide;
            headroomDb = peak > h.fast ? std::max(0.0, 20.0 * std::log10(peak / h.fast) - kHeadroomDeadZoneDb) : 0.0;
            attackTarget = std::min(static_cast<double>(params_.attackDb), headroomDb) * ahead.rise;
        } else if (params_.attackDb < 0.0f) {
            attackTarget = params_.attackDb * ahead.soften; // softening: the hit's first stretch turned down
        }
        sustainTarget = params_.sustainDb * ring_[static_cast<std::size_t>((heardIndex + sustainLead_) % size)].sustain;
    }
    // Smoothed against clicks, but a lift never passes the coming peak: at
    // the onset of a hit that rises at once, the gain drops straight to 0 dB.
    attackDb_ = std::min(attackTarget + (attackDb_ - attackTarget) * smoothing_, headroomDb);
    sustainDb_ = sustainTarget + (sustainDb_ - sustainTarget) * smoothing_;
    if (std::abs(attackDb_) < 1e-6)
        attackDb_ = 0.0;
    if (std::abs(sustainDb_) < 1e-6)
        sustainDb_ = 0.0;
    Gains gains;
    if (attackDb_ != 0.0)
        gains.high = static_cast<float>(std::pow(10.0, attackDb_ / 20.0));
    if (sustainDb_ != 0.0)
        gains.all = static_cast<float>(std::pow(10.0, sustainDb_ / 20.0));
    return gains;
}

} // namespace daw
