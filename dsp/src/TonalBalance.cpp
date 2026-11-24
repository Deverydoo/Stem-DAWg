// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/TonalBalance.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace daw {

namespace {

constexpr double kFloorDb = -120.0;
constexpr double kAlignLowHz = 100.0;
constexpr double kAlignHighHz = 8000.0;
constexpr double kFitRateHz = 48000.0; // the EQ responses used for fitting
constexpr double kRidge = 0.02;        // keeps the five gains from fighting each other

// Solves A x = b in place (Gaussian elimination with partial pivoting). A is n x n, row-major.
template <std::size_t N>
std::array<double, N> solve(std::array<std::array<double, N>, N> a, std::array<double, N> b)
{
    for (std::size_t col = 0; col < N; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < N; ++row)
            if (std::abs(a[row][col]) > std::abs(a[pivot][col]))
                pivot = row;
        std::swap(a[col], a[pivot]);
        std::swap(b[col], b[pivot]);
        if (std::abs(a[col][col]) < 1e-12)
            continue;
        for (std::size_t row = col + 1; row < N; ++row) {
            const double f = a[row][col] / a[col][col];
            for (std::size_t k = col; k < N; ++k)
                a[row][k] -= f * a[col][k];
            b[row] -= f * b[col];
        }
    }
    std::array<double, N> x{};
    for (std::size_t i = N; i-- > 0;) {
        double sum = b[i];
        for (std::size_t k = i + 1; k < N; ++k)
            sum -= a[i][k] * x[k];
        x[i] = std::abs(a[i][i]) < 1e-12 ? 0.0 : sum / a[i][i];
    }
    return x;
}

// Response of each master EQ band in dB per dB of gain, at each tonal band.
const std::array<std::array<double, kTonalBands>, kMasterEqBandCount>& eqResponses()
{
    static const auto table = [] {
        std::array<std::array<double, kTonalBands>, kMasterEqBandCount> t{};
        for (std::size_t j = 0; j < kMasterEqBandCount; ++j) {
            const BiquadCoefficients c = masterEqCoefficients(j, 6.0, kFitRateHz);
            for (std::size_t b = 0; b < kTonalBands; ++b)
                t[j][b] = c.magnitudeDb(kTonalBandHz[b], kFitRateHz) / 6.0;
        }
        return t;
    }();
    return table;
}

} // namespace

// ---- profile --------------------------------------------------------------------

TonalAnalyzer::TonalAnalyzer(double sampleRate)
    : sampleRate_(sampleRate > 0.0 ? sampleRate : 48000.0), fft_(kFftSize)
{
    window_.resize(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window_[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFftSize));
    for (auto& channel : input_)
        channel.assign(kFftSize, 0.0f);
    spectrum_.resize(kFftSize);
    bandOfBin_.assign(kFftSize / 2 + 1, -1);
    const double edge = std::exp2(1.0 / 6.0);
    for (int k = 1; k <= kFftSize / 2; ++k) {
        const double hz = k * sampleRate_ / kFftSize;
        for (std::size_t b = 0; b < kTonalBands; ++b)
            if (hz >= kTonalBandHz[b] / edge && hz < kTonalBandHz[b] * edge) {
                bandOfBin_[static_cast<std::size_t>(k)] = static_cast<int>(b);
                break;
            }
    }
    fill_ = kFftSize / 2; // the first frame starts half a frame of silence early
}

void TonalAnalyzer::process(const float* left, const float* right, int numFrames)
{
    for (int i = 0; i < numFrames; ++i) {
        input_[0][static_cast<std::size_t>(fill_)] = left[i];
        input_[1][static_cast<std::size_t>(fill_)] = right != nullptr ? right[i] : left[i];
        if (++fill_ == kFftSize) {
            frame();
            for (auto& channel : input_)
                std::copy(channel.begin() + kFftSize / 2, channel.end(), channel.begin());
            fill_ = kFftSize / 2;
        }
    }
}

void TonalAnalyzer::frame()
{
    for (int i = 0; i < kFftSize; ++i) {
        const float w = window_[static_cast<std::size_t>(i)];
        spectrum_[static_cast<std::size_t>(i)] = {input_[0][static_cast<std::size_t>(i)] * w, input_[1][static_cast<std::size_t>(i)] * w};
    }
    fft_.forward(spectrum_);
    std::array<float, kTonalBands> bands{};
    for (int k = 1; k <= kFftSize / 2; ++k) {
        const int band = bandOfBin_[static_cast<std::size_t>(k)];
        if (band < 0)
            continue;
        // With z = l + i r: (|L[k]|^2 + |R[k]|^2) / 2 = (|Z[k]|^2 + |Z[N-k]|^2) / 4.
        const auto power = 0.25f * (std::norm(spectrum_[static_cast<std::size_t>(k)])
                                    + std::norm(spectrum_[static_cast<std::size_t>((kFftSize - k) % kFftSize)]));
        bands[static_cast<std::size_t>(band)] += power;
    }
    frames_.push_back(bands);
}

TonalProfile TonalAnalyzer::result() const
{
    TonalProfile profile;
    if (frames_.empty())
        return profile;
    std::vector<double> totals;
    totals.reserve(frames_.size());
    for (const auto& f : frames_)
        totals.push_back(std::accumulate(f.begin(), f.end(), 0.0));
    std::vector<double> sorted = totals;
    std::sort(sorted.begin(), sorted.end());
    const double loud = sorted[static_cast<std::size_t>(0.9 * static_cast<double>(sorted.size() - 1))];
    if (!(loud > 0.0))
        return profile;
    const double gate = loud * std::pow(10.0, -kGateDb / 10.0);
    std::array<double, kTonalBands> energy{};
    std::size_t kept = 0;
    for (std::size_t f = 0; f < frames_.size(); ++f) {
        if (totals[f] < gate)
            continue;
        for (std::size_t b = 0; b < kTonalBands; ++b)
            energy[b] += frames_[f][b];
        ++kept;
    }
    const double total = std::accumulate(energy.begin(), energy.end(), 0.0);
    if (kept == 0 || !(total > 0.0))
        return profile;
    for (std::size_t b = 0; b < kTonalBands; ++b)
        profile.levelDb[b] = energy[b] > 0.0 ? std::max(kFloorDb, 10.0 * std::log10(energy[b] / total)) : kFloorDb;
    profile.seconds = static_cast<double>(kept) * (kFftSize / 2) / sampleRate_;
    return profile;
}

// ---- target ---------------------------------------------------------------------

TonalTarget makeTonalTarget(std::string name, std::vector<std::string> references, const std::vector<TonalProfile>& profiles)
{
    TonalTarget target;
    target.name = std::move(name);
    target.references = std::move(references);
    std::vector<const TonalProfile*> valid;
    for (const TonalProfile& p : profiles)
        if (p.valid())
            valid.push_back(&p);
    if (valid.empty()) {
        target.references.clear();
        return target;
    }
    const auto n = static_cast<double>(valid.size());
    for (std::size_t b = 0; b < kTonalBands; ++b) {
        double sum = 0.0;
        for (const TonalProfile* p : valid)
            sum += p->levelDb[b];
        const double mean = sum / n;
        double squares = 0.0;
        for (const TonalProfile* p : valid)
            squares += (p->levelDb[b] - mean) * (p->levelDb[b] - mean);
        target.meanDb[b] = mean;
        target.spreadDb[b] = valid.size() < 2 ? TonalTarget::kSingleSpreadDb
                                              : std::max(TonalTarget::kMinSpreadDb, std::sqrt(squares / (n - 1.0)));
    }
    return target;
}

// ---- comparison -----------------------------------------------------------------

TonalComparison compareTonal(const TonalProfile& mix, const TonalTarget& target, const std::array<float, kMasterEqBandCount>& currentEq)
{
    TonalComparison result;
    if (!mix.valid() || !target.valid())
        return result;

    std::array<double, kTonalBands> weight{};
    double weightSum = 0.0;
    for (std::size_t b = 0; b < kTonalBands; ++b) {
        weight[b] = 1.0 / (target.spreadDb[b] * target.spreadDb[b]);
        weightSum += weight[b];
    }
    // Lining up: subtract the weighted mean over the midrange. It is linear,
    // so the EQ responses go through it too, and the fit sees exactly what a
    // new measurement would.
    const auto lineUp = [&](std::array<double, kTonalBands>& v) {
        double sum = 0.0, w = 0.0;
        for (std::size_t b = 0; b < kTonalBands; ++b)
            if (kTonalBandHz[b] >= kAlignLowHz && kTonalBandHz[b] <= kAlignHighHz) {
                sum += weight[b] * v[b];
                w += weight[b];
            }
        for (double& x : v)
            x -= sum / w;
    };
    for (std::size_t b = 0; b < kTonalBands; ++b)
        result.deviationDb[b] = mix.levelDb[b] - target.meanDb[b];
    lineUp(result.deviationDb);
    double before = 0.0;
    for (std::size_t b = 0; b < kTonalBands; ++b) {
        result.outside[b] = std::abs(result.deviationDb[b]) > target.spreadDb[b];
        before += weight[b] * result.deviationDb[b] * result.deviationDb[b];
    }
    result.rmsBeforeDb = std::sqrt(before / weightSum);

    // Weighted least squares: minimise sum w (d + U' dg)^2 + ridge |dg|^2,
    // with U' the lined-up EQ responses.
    constexpr std::size_t n = kMasterEqBandCount;
    std::array<std::array<double, kTonalBands>, n> u = eqResponses();
    for (auto& column : u)
        lineUp(column);
    std::array<std::array<double, n>, n> a{};
    std::array<double, n> rhs{};
    for (std::size_t b = 0; b < kTonalBands; ++b)
        for (std::size_t i = 0; i < n; ++i) {
            rhs[i] -= weight[b] * u[i][b] * result.deviationDb[b];
            for (std::size_t j = 0; j < n; ++j)
                a[i][j] += weight[b] * u[i][b] * u[j][b];
        }
    // The ridge holds the total gains (current + change) small, not the
    // change: comparing again after applying a suggestion then suggests the
    // same EQ instead of creeping further.
    for (std::size_t i = 0; i < n; ++i) {
        a[i][i] += kRidge * weightSum;
        rhs[i] -= kRidge * weightSum * currentEq[i];
    }
    const std::array<double, n> x = solve(a, rhs);

    std::array<double, kMasterEqBandCount> change{};
    for (std::size_t j = 0; j < kMasterEqBandCount; ++j) {
        const double wanted = std::clamp(static_cast<double>(currentEq[j]) + x[j], -TonalComparison::kMaxSuggestedDb,
                                         TonalComparison::kMaxSuggestedDb);
        double rounded = std::round(wanted * 2.0) / 2.0;
        if (std::abs(rounded) < 0.5)
            rounded = 0.0;
        result.eqGainDb[j] = static_cast<float>(rounded);
        change[j] = rounded - currentEq[j];
    }

    // What the rounded suggestion leaves (already lined up).
    double after = 0.0;
    for (std::size_t b = 0; b < kTonalBands; ++b) {
        double residual = result.deviationDb[b];
        for (std::size_t j = 0; j < kMasterEqBandCount; ++j)
            residual += u[j][b] * change[j];
        after += weight[b] * residual * residual;
    }
    result.rmsAfterDb = std::sqrt(after / weightSum);
    return result;
}

} // namespace daw
