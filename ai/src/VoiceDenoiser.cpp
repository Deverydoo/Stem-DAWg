// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/VoiceDenoiser.h>

#include <daw/core/Files.h>
#include <daw/dsp/DeepFilterDsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace daw {

namespace df = deepfilter;

namespace {

constexpr const char* kModelPrefix = "deepfilternet3.";

} // namespace

Result<std::shared_ptr<VoiceDenoiser>> VoiceDenoiser::load(std::shared_ptr<InferenceBackend> backend,
                                                           const std::filesystem::path& modelDir)
{
    if (!backend)
        return makeError(ErrorCode::Unavailable, "no inference backend");
    std::shared_ptr<VoiceDenoiser> denoiser(new VoiceDenoiser());
    denoiser->backend_ = backend;
    const auto loadGraph = [&](const char* file, Graph& graph) -> Status {
        graph.id = std::string(kModelPrefix) + file;
        auto info = backend->isLoaded(graph.id) ? backend->info(graph.id)
                                                : backend->load(ModelDescriptor{graph.id, "3", "voice-denoise", modelDir / file, {}, {}});
        if (!info)
            return info.error();
        graph.inputs = info->inputNames;
        graph.outputs = info->outputNames;
        return Status::success();
    };
    for (auto [file, graph] : {std::pair{kEncoderFile, &denoiser->encoder_}, std::pair{kErbDecoderFile, &denoiser->erbDecoder_},
                               std::pair{kDfDecoderFile, &denoiser->dfDecoder_}})
        if (auto status = loadGraph(file, *graph); !status)
            return status.error();
    return denoiser;
}

Result<std::vector<float>> VoiceDenoiser::process(const float* input, std::size_t frames, const VoiceDenoiseOptions& options,
                                                  const CancellationToken& cancel,
                                                  const std::function<void(double)>& progress) const
{
    std::vector<float> output(frames, 0.0f);
    if (frames == 0)
        return output;

    // One extra frame of padding flushes the synthesis delay (as libDF's enhance does).
    const std::size_t padded = frames + df::kFftSize;
    const auto total = static_cast<int>((padded + df::kHop - 1) / df::kHop);
    const auto hopAt = [&](int t, std::array<float, df::kHop>& hop) {
        const std::size_t start = static_cast<std::size_t>(t) * df::kHop;
        for (int i = 0; i < df::kHop; ++i) {
            const std::size_t at = start + static_cast<std::size_t>(i);
            hop[static_cast<std::size_t>(i)] = at < frames ? input[at] : 0.0f;
        }
    };

    // Pass 1: features of every frame (their normalisation runs through the whole channel).
    std::vector<float> erb(static_cast<std::size_t>(total) * df::kErbBands);
    std::vector<float> spec(static_cast<std::size_t>(total) * 2 * df::kDfBins);
    {
        df::Analysis analysis;
        df::Features features;
        df::Spectrum x;
        std::array<float, df::kHop> hop{};
        for (int t = 0; t < total; ++t) {
            hopAt(t, hop);
            analysis.process(hop.data(), x);
            features.compute(x, erb.data() + static_cast<std::size_t>(t) * df::kErbBands,
                             spec.data() + static_cast<std::size_t>(t) * 2 * df::kDfBins);
        }
    }

    // Pass 2: inference chunk by chunk, then the mask and deep filter frame by frame.
    df::Analysis analysis;
    df::Synthesis synthesis;
    std::array<df::Spectrum, df::kDfOrder> ring{}; // X[tau - 2 .. tau + 2], by frame index modulo kDfOrder
    for (auto& s : ring)
        s.fill({});
    int analysed = 0; // frames analysed so far
    std::array<float, df::kHop> hop{};
    const auto analyseUpTo = [&](int last) {
        for (; analysed <= last; ++analysed) {
            df::Spectrum& slot = ring[static_cast<std::size_t>(analysed % df::kDfOrder)];
            if (analysed < total) {
                hopAt(analysed, hop);
                analysis.process(hop.data(), slot);
            } else {
                slot.fill({});
            }
        }
    };
    const df::Spectrum zero{};
    const float limit = options.reductionDb >= 100.0 ? 0.0f : static_cast<float>(std::pow(10.0, -options.reductionDb / 20.0));
    const int chunk = std::max(1, options.chunkFrames);
    const int warmup = std::max(0, options.warmupFrames);

    for (int a = 0; a < total; a += chunk) {
        if (cancel.isCancelled())
            return makeError(ErrorCode::Cancelled, "voice denoise cancelled");
        const int b = std::min(a + chunk, total);
        const int s0 = std::max(0, a - warmup);
        const int s1 = std::min(total, b + df::kLookahead);
        const int length = s1 - s0;

        TensorF featErb{{1, 1, length, df::kErbBands}, {}};
        featErb.data.assign(erb.begin() + static_cast<std::ptrdiff_t>(s0) * df::kErbBands,
                            erb.begin() + static_cast<std::ptrdiff_t>(s1) * df::kErbBands);
        TensorF featSpec{{1, 2, length, df::kDfBins}, std::vector<float>(static_cast<std::size_t>(2 * length * df::kDfBins))};
        for (int c = 0; c < 2; ++c)
            for (int t = 0; t < length; ++t)
                std::copy_n(spec.begin() + (static_cast<std::ptrdiff_t>(s0 + t) * 2 + c) * df::kDfBins, df::kDfBins,
                            featSpec.data.begin() + (static_cast<std::ptrdiff_t>(c) * length + t) * df::kDfBins);

        auto encoded = backend_->run(encoder_.id, {{encoder_.inputs.at(0), std::move(featErb)}, {encoder_.inputs.at(1), std::move(featSpec)}}, cancel);
        if (!encoded)
            return encoded.error();
        std::map<std::string, const TensorF*> named;
        for (std::size_t i = 0; i < encoder_.outputs.size() && i < encoded->size(); ++i)
            named[encoder_.outputs[i]] = &(*encoded)[i];
        const auto decode = [&](const Graph& graph) -> Result<std::vector<TensorF>> {
            std::vector<std::pair<std::string, TensorF>> inputs;
            for (const std::string& name : graph.inputs) {
                const auto it = named.find(name);
                if (it == named.end())
                    return makeError(ErrorCode::InferenceError, "the voice model's decoder needs '" + name + "', which the encoder does not give");
                inputs.emplace_back(name, *it->second);
            }
            return backend_->run(graph.id, inputs, cancel);
        };
        auto mask = decode(erbDecoder_);
        if (!mask)
            return mask.error();
        auto coefs = decode(dfDecoder_);
        if (!coefs)
            return coefs.error();
        const TensorF& m = mask->at(0);   // [1, 1, length, 32]
        const TensorF& c = coefs->at(0);  // [1, length, 96, 10]
        if (m.data.size() != static_cast<std::size_t>(length) * df::kErbBands
            || c.data.size() != static_cast<std::size_t>(length) * df::kDfBins * df::kDfOrder * 2)
            return makeError(ErrorCode::InferenceError, "the voice model's outputs have unexpected shapes");

        // The network's output for frame t belongs to the spectrum kLookahead
        // frames earlier (its convolutions look ahead by that much). The deep
        // filter runs on the unmasked spectrum, over frames tau - 2 .. tau + 2;
        // its coefficients are laid out bin by bin, then tap, then (re, im).
        for (int tau = a; tau < b; ++tau) {
            analyseUpTo(tau + df::kLookahead);
            const df::Spectrum& x = ring[static_cast<std::size_t>(tau % df::kDfOrder)];
            const int t = std::min(tau + df::kLookahead, s1 - 1) - s0;
            df::Spectrum y = x;
            df::applyErbGains(y, m.data.data() + static_cast<std::size_t>(t) * df::kErbBands);
            std::array<const df::Spectrum*, df::kDfOrder> taps{};
            for (int i = 0; i < df::kDfOrder; ++i) {
                const int frame = tau - df::kLookahead + i;
                taps[static_cast<std::size_t>(i)] = frame < 0 ? &zero : &ring[static_cast<std::size_t>(frame % df::kDfOrder)];
            }
            df::applyDeepFilter(taps.data(), c.data.data() + static_cast<std::size_t>(t) * df::kDfBins * df::kDfOrder * 2, y);
            if (limit > 0.0f)
                for (int k = 0; k < df::kBins; ++k)
                    y[static_cast<std::size_t>(k)] = limit * x[static_cast<std::size_t>(k)] + (1.0f - limit) * y[static_cast<std::size_t>(k)];
            synthesis.process(y, hop.data());
            // Synthesis lags analysis by one hop.
            const std::ptrdiff_t start = static_cast<std::ptrdiff_t>(tau - 1) * df::kHop;
            for (int i = 0; i < df::kHop; ++i) {
                const std::ptrdiff_t at = start + i;
                if (at >= 0 && at < static_cast<std::ptrdiff_t>(frames))
                    output[static_cast<std::size_t>(at)] = hop[static_cast<std::size_t>(i)];
            }
        }
        if (progress)
            progress(static_cast<double>(b) / total);
    }
    return output;
}

} // namespace daw
