// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Offline high-quality sample-rate conversion (Kaiser-windowed sinc, polyphase
// table with linear interpolation, anti-aliased when downsampling).
//
// Used to create an engine-rate "rendition" of a media file when the device
// runs at a different rate from the source (e.g. 44.1 kHz Suno stem on a
// 48 kHz WASAPI shared-mode device). Never runs on the audio thread.

#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

namespace daw {

// Defaults: ~-86 dB stop band beginning just below the lower of the two Nyquist
// frequencies (e.g. pass band to ~20 kHz, stop band from ~21.8 kHz for
// 48 <-> 44.1 kHz), so nothing above the output Nyquist aliases audibly.
struct ResampleOptions {
    int halfTaps = 64;          // zero crossings per side at the cutoff frequency
    double kaiserBeta = 8.6;    // ~ -86 dB stop band
    double cutoffMargin = 0.95; // filter centre as a fraction of the lower Nyquist
};

/// Returns a new asset at `targetRate`. Output length is round(frames * ratio).
Result<AudioAssetPtr> resampleAudio(const AudioAsset& input, double targetRate,
                                    const ResampleOptions& options = {},
                                    const CancellationToken& cancel = {});

} // namespace daw
