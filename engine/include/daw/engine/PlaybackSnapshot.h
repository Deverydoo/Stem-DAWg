// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Immutable description of everything the audio thread renders. Built on a
// control/worker thread from the project model (see daw::session), published
// through AudioEngine::publishSnapshot, and destroyed on the control thread.
//
// The audio thread only reads raw pointers; `keepAlive` owns the assets, so no
// shared_ptr reference counting ever happens on the audio thread.
//
// Milestone 1 renders a flat list of tracks of audio clips into a stereo
// master. Milestone 3 replaces TrackPlayback with a compiled processing graph
// (buses, sends, plugins, latency compensation) behind the same exchange.

#include <daw/core/AudioAsset.h>
#include <daw/dsp/CleanupChain.h>

#include <cstdint>
#include <vector>

namespace daw {

struct ClipPlayback {
    const AudioAsset* asset = nullptr; // engine-rate rendition, owned via keepAlive
    std::int64_t timelineStart = 0;    // engine frames
    std::int64_t sourceStart = 0;      // frame offset into asset
    std::int64_t length = 0;           // frames
    float gain = 1.0f;
};

struct TrackPlayback {
    std::vector<ClipPlayback> clips; // sorted by timelineStart
    // Final per-output-channel gains with fader, pan, mute and solo already
    // resolved on the control thread. The engine ramps to these over ~10 ms
    // whenever they change during playback, so mixer moves never click.
    float gainLeft = 1.0f;
    float gainRight = 1.0f;
    // Stable identity across snapshots (hash of the TrackId) so the engine can
    // carry each track's ramp state into the next snapshot. 0 = no identity
    // (the track starts at its target gain).
    std::uint64_t key = 0;
    // Pre-fader clean-up (DC removal, high-pass, expander). Inactive params
    // play the track dry. Needs a non-zero key; the chain's state follows the
    // key across snapshots so parameter changes do not restart it.
    CleanupParams cleanup;
};

struct PlaybackSnapshot {
    std::uint64_t version = 0;
    double sampleRate = 0.0;     // must equal the engine rate or the engine renders silence
    std::vector<TrackPlayback> tracks;
    float masterGain = 1.0f;
    std::int64_t endFrame = 0;   // end of the last clip (engine frames)
    std::vector<AudioAssetPtr> keepAlive;
    // Noise profiles at the engine rate, pointed to by TrackPlayback::cleanup.
    std::vector<std::shared_ptr<const std::vector<float>>> noiseProfiles;
};

} // namespace daw
