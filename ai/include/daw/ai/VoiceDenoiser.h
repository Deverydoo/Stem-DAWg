// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Neural voice denoising with DeepFilterNet3 (Schröter et al., 2023; MIT or
// Apache-2.0), run through the InferenceBackend. It removes noise that is not
// steady (traffic, keyboard, a fan changing speed, room tone under a voice)
// where the spectral denoiser only removes a fixed profile.
//
// The model comes as three ONNX graphs (encoder, ERB-mask decoder, deep-filter
// decoder) that process whole sequences of 20 ms frames. Processing is
// offline: features for the whole channel first, then chunks of frames, each
// preceded by a warm-up stretch so the network's recurrent state settles
// (the warm-up output is discarded). Memory stays small for long songs.
//
// The model works on 48 kHz audio, one channel at a time. Never call from the
// audio thread.

#include <daw/ai/InferenceBackend.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace daw {

struct VoiceDenoiseOptions {
    /// The most the noise is turned down, in dB (the model's attenuation
    /// limit). 100 or more: no limit.
    double reductionDb = 100.0;
    int chunkFrames = 1000; // 10 s per inference
    int warmupFrames = 100; // 1 s of context before each chunk
};

class VoiceDenoiser {
public:
    /// The files a model directory must hold.
    static constexpr const char* kEncoderFile = "enc.onnx";
    static constexpr const char* kErbDecoderFile = "erb_dec.onnx";
    static constexpr const char* kDfDecoderFile = "df_dec.onnx";
    static constexpr double kSampleRate = 48000.0;

    /// Loads the three graphs from `modelDir` into `backend` (once per backend).
    static Result<std::shared_ptr<VoiceDenoiser>> load(std::shared_ptr<InferenceBackend> backend,
                                                       const std::filesystem::path& modelDir);

    /// Denoises one channel of 48 kHz audio. The output has the same length
    /// and no delay. `progress` receives 0..1. Safe to call concurrently.
    Result<std::vector<float>> process(const float* input, std::size_t frames, const VoiceDenoiseOptions& options = {},
                                       const CancellationToken& cancel = {},
                                       const std::function<void(double)>& progress = {}) const;

private:
    struct Graph {
        std::string id;
        std::vector<std::string> inputs;
        std::vector<std::string> outputs;
    };

    VoiceDenoiser() = default;

    std::shared_ptr<InferenceBackend> backend_;
    Graph encoder_, erbDecoder_, dfDecoder_;
};

} // namespace daw
