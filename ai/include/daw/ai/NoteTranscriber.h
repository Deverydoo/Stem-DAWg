// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Pitched notes from audio with Basic Pitch (Bittner et al., ICASSP 2022;
// Spotify, Apache-2.0), run through the InferenceBackend. The model is
// instrument-agnostic and polyphonic: bass, guitars, keys, synths and voices
// all come out as notes on the 88 piano keys.
//
// The model hears 2-second windows of 22.05 kHz mono audio and gives, per
// 11.6 ms frame and key, how likely a note sounds there and how likely one
// starts there. Windows overlap by 30 frames and each keeps its middle, as in
// Basic Pitch's own inference; every frame's time is computed exactly (the
// reference code approximates it). Notes are then decoded from those
// activations as Basic Pitch does: onset peaks start notes that last while
// the key stays active, and the "melodia" pass adds notes that sound clearly
// without a clear start.
//
// The model is compiled into the library (third_party/basic-pitch). Never
// call from the audio thread.

#include <daw/ai/InferenceBackend.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace daw {

inline constexpr int kPianoLowestKey = 21; // A0, the model's first key
inline constexpr int kPianoKeys = 88;

/// The model's activations, one row of kPianoKeys per frame.
struct NoteActivations {
    std::vector<float> frames;  // a note sounds (0..1)
    std::vector<float> onsets;  // a note starts (0..1)
    std::vector<double> times;  // each frame's time in seconds
    std::size_t count() const noexcept { return times.size(); }
};

struct PitchedNote {
    double start = 0.0; // seconds
    double end = 0.0;
    int key = 60;       // MIDI key
    float amplitude = 0.0f; // the mean activation over the note (0..1)
};

/// Basic Pitch's decoding parameters; the defaults are its own.
struct NoteDecodeParams {
    float onsetThreshold = 0.5f;   // an onset peak at least this starts a note
    float frameThreshold = 0.3f;   // a note lasts while its activation stays at least this
    double minNoteSeconds = 0.1277;
    bool inferOnsets = true;       // also start notes where the activation jumps
    bool melodia = true;           // add notes that sound clearly without a clear start
    int lowestKey = kPianoLowestKey;
    int highestKey = kPianoLowestKey + kPianoKeys - 1;
};

/// Decodes notes from activations, in no particular order.
std::vector<PitchedNote> decodeNotes(const NoteActivations& activations, const NoteDecodeParams& params = {});

class NoteTranscriber {
public:
    static constexpr double kSampleRate = 22050.0;
    static constexpr int kHop = 256;               // samples per frame
    static constexpr int kWindowSamples = 43844;   // 2 s less a hop
    static constexpr int kWindowFrames = 172;
    static constexpr int kOverlapFrames = 30;
    static constexpr double kFrameRate = kSampleRate / kHop;

    /// Loads the compiled-in model into `backend` (once per backend).
    static Result<std::shared_ptr<NoteTranscriber>> load(std::shared_ptr<InferenceBackend> backend);

    /// The model's activations for mono audio at kSampleRate. `progress`
    /// receives 0..1. Safe to call concurrently.
    Result<NoteActivations> analyse(const float* mono, std::size_t frames, const CancellationToken& cancel = {},
                                    const std::function<void(double)>& progress = {}) const;

private:
    NoteTranscriber() = default;

    std::shared_ptr<InferenceBackend> backend_;
    std::string input_;
    std::size_t noteOutput_ = 0;  // index of each output in the run's results
    std::size_t onsetOutput_ = 0;
};

} // namespace daw
