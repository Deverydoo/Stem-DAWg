// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Model inference abstraction. The ONNX Runtime implementation lives behind
// this interface so analyzers, tests and the UI never include ORT headers, and
// alternative runtimes can be added without touching callers.
//
// Thread safety: all methods may be called concurrently from analysis workers.
// Never call from the audio thread.

#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace daw {

enum class ExecutionProvider { Cpu, DirectML, Cuda };

const char* toString(ExecutionProvider provider) noexcept;

struct TensorF {
    std::vector<std::int64_t> shape;
    std::vector<float> data;
};

struct ModelDescriptor {
    std::string id;                    // stable identifier, e.g. "daw.stem-classifier"
    std::string version;
    std::string task;                  // "stem-classification", "onset-detection", ...
    std::filesystem::path file;        // .onnx on disk, or empty when inlineBytes is set
    std::vector<std::uint8_t> inlineBytes;
    std::vector<ExecutionProvider> preferredProviders; // tried in order, CPU is the final fallback
};

struct LoadedModelInfo {
    std::string id;
    ExecutionProvider provider = ExecutionProvider::Cpu;
    std::vector<std::string> inputNames;
    std::vector<std::string> outputNames;
};

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;

    virtual std::string name() const = 0;
    virtual std::vector<ExecutionProvider> availableProviders() const = 0;

    virtual Result<LoadedModelInfo> load(const ModelDescriptor& model) = 0;
    virtual bool isLoaded(const std::string& modelId) const = 0;
    virtual Result<LoadedModelInfo> info(const std::string& modelId) const = 0;
    virtual void unload(const std::string& modelId) = 0;

    virtual Result<std::vector<TensorF>> run(const std::string& modelId,
                                             const std::vector<std::pair<std::string, TensorF>>& inputs,
                                             const CancellationToken& cancel = {}) = 0;
};

struct OnnxBackendConfig {
    int intraOpThreads = 2; // keep low: inference must not starve the audio thread
};

/// Creates the ONNX Runtime backend, or Unavailable when built without ONNX.
Result<std::shared_ptr<InferenceBackend>> createOnnxBackend(const OnnxBackendConfig& config = {});

} // namespace daw
