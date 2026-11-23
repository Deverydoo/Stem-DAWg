// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Per-track clean-up: DC removal -> Butterworth high-pass -> hum notches ->
// spectral noise reduction -> downward expander.
//
// The noise reduction works on overlapping FFT frames and the expander looks
// ahead (it measures the signal kLookaheadSeconds before it is heard, so it is
// fully open when a note starts). The chain therefore delays its output by a
// fixed latency() frames, whichever stages are on. The engine hides this by
// reading the track's audio latency() frames early (stems are random access),
// so processed tracks stay sample-aligned with dry ones.
//
// "Listen to removed" outputs the dry input minus the processed signal: what
// the chain takes away.
//
// Real-time safe after prepare(): process(), setParams() and reset() never
// allocate, lock or block.

#include <daw/dsp/Biquad.h>
#include <daw/dsp/SpectralDenoiser.h>

#include <array>
#include <vector>

namespace daw {

struct CleanupParams {
    bool dcRemoval = false;
    bool highPass = false;
    float highPassHz = 120.0f; // -3 dB point
    int highPassOrder = 4;     // 2, 4 or 8 (12, 24 or 48 dB/octave)
    bool expander = false;
    float thresholdDb = -50.0f; // RMS level (mean power of both channels)
    float ratio = 4.0f;         // >= 1
    float rangeDb = 24.0f;      // maximum attenuation
    float releaseMs = 80.0f;
    bool hum = false;
    float humHz = 50.0f;
    int humHarmonics = 4;
    bool denoise = false;
    float denoiseReductionDb = 12.0f;
    float denoiseSensitivityDb = 0.0f;
    const float* noiseProfile = nullptr; // SpectralDenoiser::kBins powers for this rate, owned by the snapshot
    bool listenRemoved = false;          // output what the chain removes instead of the result

    bool active() const noexcept { return dcRemoval || highPass || hum || denoise || expander; }

    friend bool operator==(const CleanupParams&, const CleanupParams&) = default;
};

class CleanupChain {
public:
    static constexpr double kLookaheadSeconds = 0.005;
    static constexpr double kDcCutoffHz = 5.0;
    static constexpr double kDetectorMs = 2.0;  // RMS averaging time
    static constexpr double kAttackMs = 1.0;    // opening time
    static constexpr double kHoldMs = 20.0;     // stays open this long after the level falls
    static constexpr double kKneeDb = 6.0;      // soft knee width around the threshold
    static constexpr int kMaxSections = 4;      // 8th-order high-pass

    /// Allocates the delay lines and FFT buffers (not real-time safe). Resets the state.
    void prepare(double sampleRate);
    /// Total delay in frames: the FFT frame plus the expander lookahead. The
    /// same whichever stages are on.
    int latency() const noexcept { return latency_; }
    /// Turning the noise reduction on or off changes how the delay is made:
    /// the chain must then be reset and primed again (the engine does this).
    static bool needsRestart(const CleanupParams& from, const CleanupParams& to) noexcept { return from.denoise != to.denoise; }
    double sampleRate() const noexcept { return sampleRate_; }

    /// Recomputes coefficients; filter memory is kept unless the high-pass
    /// order or on/off state changes, so moving a control does not click.
    void setParams(const CleanupParams& params) noexcept;
    const CleanupParams& params() const noexcept { return params_; }

    /// Clears filter memory, the detector and the delay line (the expander starts open).
    void reset() noexcept;

    /// Processes one stereo block in place. The output is delayed by latency().
    void process(float* left, float* right, int numFrames) noexcept;

    /// Expander gain applied to the last output sample, in dB (<= 0).
    float currentGainDb() const noexcept { return static_cast<float>(gainDb_); }
    /// Lowest expander gain since the last call, in dB (<= 0), for a gain-reduction meter.
    float takeMinGainDb() noexcept
    {
        const double lowest = minGainDb_;
        minGainDb_ = gainDb_;
        return static_cast<float>(lowest);
    }

    /// Expander gain computer (static curve with soft knee), in dB (<= 0).
    static double expanderGainDb(double levelDb, double thresholdDb, double ratio, double rangeDb) noexcept;

private:
    void processChunk(float* left, float* right, int numFrames) noexcept;

    double sampleRate_ = 48000.0;
    int latency_ = 0;
    int lookahead_ = 0;
    CleanupParams params_;

    HumFilter hum_;
    SpectralDenoiser denoiser_;
    std::vector<float> fftDelay_; // stands in for the denoiser's delay while it is off (interleaved)
    int fftDelayPos_ = 0;
    std::vector<float> dryRing_;  // input delayed by latency(), interleaved, for listenRemoved
    std::vector<float> dryOut_;   // planar scratch: the delayed input of the current chunk
    int dryPos_ = 0;

    // DC blocker (one pole, one zero).
    double dcR_ = 0.0;
    std::array<double, 2> dcX_{};
    std::array<double, 2> dcY_{};

    // High-pass sections per channel.
    int sections_ = 0;
    std::array<std::array<Biquad, kMaxSections>, 2> hp_{};

    // Expander.
    double detectorCoeff_ = 0.0;
    double attackCoeff_ = 0.0;
    double releaseCoeff_ = 0.0;
    int holdFrames_ = 0;
    double openPower_ = 0.0; // detector power above which the gain is 0 dB
    double power_ = 0.0;
    double gainDb_ = 0.0;
    double gain_ = 1.0;
    double minGainDb_ = 0.0;
    int hold_ = 0;

    // Expander lookahead delay line, interleaved stereo.
    std::vector<float> delay_;
    int delayPos_ = 0;
};

} // namespace daw
