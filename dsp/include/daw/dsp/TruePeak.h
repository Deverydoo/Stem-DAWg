// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// True-peak detection (ITU-R BS.1770-4, Annex 2): the signal is oversampled
// (4x below 96 kHz, 2x below 192 kHz) with a polyphase low-pass interpolator
// and the largest absolute value of the oversampled signal is reported. This
// catches the inter-sample peaks that clip in DACs and lossy encoders even
// when no sample reaches 0 dBFS.
//
// prepare() allocates; process() and reset() are real-time safe.

#include <cstdint>
#include <vector>

namespace daw {

class TruePeakDetector {
public:
    void prepare(double sampleRate, int numChannels);
    void reset() noexcept;

    /// Returns the true peak (linear, >= the sample peak) of this block over
    /// all channels. Channels beyond the prepared count are ignored.
    float process(const float* const* channels, int numChannels, int numFrames) noexcept;

    int oversampling() const noexcept { return factor_; }

private:
    int factor_ = 1;
    int taps_ = 0;                // taps per polyphase branch
    int channels_ = 0;
    std::vector<float> phases_;   // [phase][tap], tap 0 multiplies the newest sample
    std::vector<float> history_;  // per channel: 2 * taps_ (mirrored ring for contiguous reads)
    std::vector<int> position_;   // per channel ring position
};

} // namespace daw
