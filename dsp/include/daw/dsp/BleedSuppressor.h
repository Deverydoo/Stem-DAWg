// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Cross-stem bleed suppression by mask sharpening.
//
// A stem separator (Suno's included) splits each time-frequency bin of the
// mix between the stems; the stems then share the mix's phase in that bin and
// differ only in level. Its mistakes show as a stem keeping a small share of
// bins another stem owns: the hi-hat ghosting through the vocal, the kick
// inside the bass. With every stem at hand, each stem's share of a bin is
//
//     M_s = |Y_s| / sum_j |Y_j|
//
// and raising the shares to a power p > 1 and renormalising,
//
//     G_s = M_s^(p-1) / sum_j M_j^p,
//
// leaves bins a stem owns (M_s near 1) or splits evenly alone (G_s = 1 when
// two stems share equally) and turns down the small shares, by at most
// `reductionDb`. p = 1 changes nothing. The amplitudes are smoothed over
// 3 frames x 3 bins first, so the gains do not flutter ("musical noise").
//
// Short-time Fourier transform: kFftSize-point frames, hop kFftSize/4,
// square-root Hann analysis and synthesis (perfect reconstruction); both
// channels of a stem share one complex FFT and one gain per bin.
//
// Offline, in blocks of up to kMaxHops hops, in three stages so the per-stem
// transforms can run on several threads:
//     analyse(s, ...) for every stem -> computeGains() -> synthesise(s, ...)
// for the stems whose output is wanted (the others serve as references only).
// The output is delayed by latency() samples. No allocation after construction.

#include <daw/dsp/Fft.h>

#include <complex>
#include <vector>

namespace daw {

struct BleedParams {
    float exponent = 2.0f;     // p: 1 = off, 1.5 light, 2 medium, 3 strong
    float reductionDb = 18.0f; // the most any bin of a stem is turned down
};

class BleedSuppressor {
public:
    static constexpr int kFftSize = 2048;
    static constexpr int kHop = kFftSize / 4;
    static constexpr int kBins = kFftSize / 2 + 1;
    static constexpr int kMaxHops = 32;

    explicit BleedSuppressor(int numStems); // allocates
    int numStems() const noexcept { return static_cast<int>(stems_.size()); }
    static constexpr int latency() noexcept { return kFftSize; }

    void setParams(const BleedParams& params) noexcept;
    const BleedParams& params() const noexcept { return params_; }
    void reset() noexcept;

    /// Stage 1: takes hops * kHop new samples of stem `stem` (1 <= hops <=
    /// kMaxHops, the same for every stem of a block). `right` may be null
    /// (mono). Different stems may be analysed concurrently.
    void analyse(int stem, const float* left, const float* right, int hops) noexcept;
    /// Stage 2: the gains of every stem, once all stems were analysed.
    void computeGains() noexcept;
    /// Stage 3: writes hops * kHop output samples of `stem`; `right` may be
    /// null. Different stems may be synthesised concurrently.
    void synthesise(int stem, float* left, float* right) noexcept;

    /// The three stages for every stem, on the calling thread. Null output
    /// pointers skip a stem's synthesis.
    void process(const float* const* left, const float* const* right, float* const* outLeft, float* const* outRight,
                 int hops) noexcept;

    /// The gain applied to bin `bin` of `stem` in the newest frame of the last
    /// block (for tests).
    float lastGain(int stem, int bin) const noexcept;

private:
    // Frames 0 and 1 of a block are the last two frames of the previous one:
    // frame i is finished (its gain needs frames i-1 and i+1) one hop late.
    static constexpr int kFrames = kMaxHops + 2;

    struct Stem {
        std::vector<float> input[2];               // last kFftSize samples
        std::vector<float> output[2];              // overlap-add accumulator
        std::vector<std::complex<float>> spectra;  // kFrames x kFftSize (packed: left + i right)
        std::vector<float> amplitude;              // kFrames x kBins, summed over 3 bins
        std::vector<float> gains;                  // kMaxHops x kBins, for frames 1..hops
        std::vector<std::complex<float>> scratch;  // kFftSize
        std::vector<float> raw;                    // kBins, one frame's amplitude
        int hops = 0;                              // hops of the current block
    };

    Fft fft_;
    BleedParams params_;
    float floorGain_ = 0.125f;
    std::vector<float> window_;
    std::vector<Stem> stems_;
    std::vector<float> share_; // numStems, per bin
};

} // namespace daw
