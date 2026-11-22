// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Tiny ONNX models generated in code, so the inference path is exercised end
// to end (app startup self-test, tests, InferenceProbe analyzer) without
// shipping model files in Milestone 1.

#include <daw/ai/InferenceBackend.h>

#include <cstdint>
#include <vector>

namespace daw {

inline constexpr const char* kSelfTestModelId = "daw.selftest.scale2";

/// ONNX (IR 8, opset 13) graph computing y = x * factor for a float tensor of
/// shape [N]. Input "x", output "y".
std::vector<std::uint8_t> buildScaleModel(float factor);

ModelDescriptor selfTestModelDescriptor();

struct SelfTestReport {
    std::string backendName;
    ExecutionProvider provider = ExecutionProvider::Cpu;
    double maxAbsError = 0.0;
    double latencyMs = 0.0;
};

/// Loads the self-test model (if needed), runs it and checks y == 2x.
Result<SelfTestReport> runInferenceSelfTest(InferenceBackend& backend);

} // namespace daw
