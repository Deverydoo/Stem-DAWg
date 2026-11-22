// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/Analyzers.h>
#include <daw/ai/BuiltinModels.h>

#include "DspUtil.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace daw {

namespace {

// Pushes real audio through the ONNX inference path from an analysis worker.
// It proves the integration (model load, tensor marshalling, provider
// selection, concurrency) that Milestone 6 models will use.
class InferenceProbeAnalyzer final : public IAnalyzer {
public:
    std::string id() const override { return kInferenceProbeAnalyzerId; }
    int version() const override { return 1; }
    std::string displayName() const override { return "AI inference probe"; }

    Result<AnalysisResult> analyze(const AudioAsset& audio, const nlohmann::json& params, AnalysisContext& context) override
    {
        if (context.inference == nullptr)
            return makeError(ErrorCode::Unavailable, "no inference backend");
        InferenceBackend& backend = *context.inference;
        if (!backend.isLoaded(kSelfTestModelId)) {
            auto loaded = backend.load(selfTestModelDescriptor());
            if (!loaded)
                return loaded.error();
        }

        const auto maxSamples = params.value("maxSamples", std::int64_t{48000});
        const std::vector<float> mono = detail::monoMix(audio);
        TensorF input;
        input.data.assign(mono.begin(), mono.begin() + static_cast<std::ptrdiff_t>(std::min<std::int64_t>(maxSamples, static_cast<std::int64_t>(mono.size()))));
        if (input.data.empty())
            input.data.push_back(0.0f);
        input.shape = {static_cast<std::int64_t>(input.data.size())};

        context.progress(0.1f);
        const auto start = std::chrono::steady_clock::now();
        auto outputs = backend.run(kSelfTestModelId, {{"x", input}}, context.cancel);
        const double latency = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!outputs)
            return outputs.error();
        if (outputs->empty() || outputs->front().data.size() != input.data.size())
            return makeError(ErrorCode::InferenceError, "unexpected model output shape");

        double maxError = 0.0;
        for (std::size_t i = 0; i < input.data.size(); ++i)
            maxError = std::max(maxError, static_cast<double>(std::abs(outputs->front().data[i] - 2.0f * input.data[i])));

        auto info = backend.info(kSelfTestModelId);
        AnalysisResult result;
        result.data = {{"backend", backend.name()},
                       {"provider", info ? toString(info->provider) : "unknown"},
                       {"samples", input.data.size()},
                       {"latencyMs", latency},
                       {"maxAbsError", maxError},
                       {"passed", maxError <= 1e-6}};
        context.progress(1.0f);
        return result;
    }
};

} // namespace

std::shared_ptr<IAnalyzer> createInferenceProbeAnalyzer()
{
    return std::make_shared<InferenceProbeAnalyzer>();
}

} // namespace daw
