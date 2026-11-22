// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Analyzer plug-in interface. An analyzer turns an AudioAsset into an
// AnalysisResult: plain JSON data that becomes editable project data (markers,
// trigger events, suggested settings), never an opaque change to audio.
//
// Analyzers run on AnalysisService worker threads, must poll the cancellation
// token at least every ~50 ms of work, and report progress in [0, 1].

#include <daw/ai/InferenceBackend.h>
#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace daw {

struct AnalysisContext {
    CancellationToken cancel;
    std::function<void(float)> progress;     // never null inside analyze()
    InferenceBackend* inference = nullptr;   // null when AI inference is unavailable
};

struct AnalysisResult {
    std::string analyzerId;
    int analyzerVersion = 0;
    nlohmann::json data;
    double elapsedMs = 0.0;
};

class IAnalyzer {
public:
    virtual ~IAnalyzer() = default;

    virtual std::string id() const = 0;          // stable, e.g. "daw.transients"
    virtual int version() const = 0;             // bump when output changes meaning
    virtual std::string displayName() const = 0;

    virtual Result<AnalysisResult> analyze(const AudioAsset& audio, const nlohmann::json& params,
                                           AnalysisContext& context) = 0;
};

} // namespace daw
