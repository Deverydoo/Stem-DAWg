// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Built-in analyzers shipped in Milestone 1. The deterministic DSP analyzers
// provide immediately useful measurements and are the reference baseline that
// later learned models (stem classification, artifact detection, drum onset
// classification, bass pitch tracking) are evaluated against.
//
//  id                    output (AnalysisResult::data)
//  -------------------   ------------------------------------------------------
//  daw.levels            per-channel peak/RMS/crest/DC/clipping, correlation
//  daw.transients        onset candidates {frame, timeSec, strength}
//  daw.spectrum          8-band energy distribution, centroid, harshness ratio
//  daw.inference-probe   runs the built-in ONNX model on the audio (AI path check)

#include <daw/ai/AnalysisService.h>
#include <daw/ai/Analyzer.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace daw {

inline constexpr const char* kLevelAnalyzerId = "daw.levels";
inline constexpr const char* kTransientAnalyzerId = "daw.transients";
inline constexpr const char* kSpectralAnalyzerId = "daw.spectrum";
inline constexpr const char* kInferenceProbeAnalyzerId = "daw.inference-probe";

std::shared_ptr<IAnalyzer> createLevelAnalyzer();
std::shared_ptr<IAnalyzer> createTransientAnalyzer();
std::shared_ptr<IAnalyzer> createSpectralAnalyzer();
std::shared_ptr<IAnalyzer> createInferenceProbeAnalyzer();

/// Registers every built-in analyzer.
Status registerBuiltinAnalyzers(AnalysisService& service);

/// Extracts onset frame positions from a daw.transients result.
std::vector<std::int64_t> onsetFrames(const nlohmann::json& transientResult);

} // namespace daw
