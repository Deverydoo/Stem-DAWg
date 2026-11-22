// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/Analyzers.h>

namespace daw {

Status registerBuiltinAnalyzers(AnalysisService& service)
{
    for (auto analyzer : {createLevelAnalyzer(), createTransientAnalyzer(), createSpectralAnalyzer(),
                          createInferenceProbeAnalyzer()}) {
        if (Status status = service.registerAnalyzer(std::move(analyzer)); !status)
            return status;
    }
    return Status::success();
}

std::vector<std::int64_t> onsetFrames(const nlohmann::json& transientResult)
{
    std::vector<std::int64_t> frames;
    if (!transientResult.is_object() || !transientResult.contains("onsets"))
        return frames;
    for (const auto& onset : transientResult["onsets"])
        frames.push_back(onset.value("frame", std::int64_t{0}));
    return frames;
}

} // namespace daw
