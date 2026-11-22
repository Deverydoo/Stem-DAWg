// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Catalogue of models known to the application. Models are declared in a JSON
// manifest (models/models.json next to the executable, or per user):
//
//   {
//     "models": [
//       { "id": "daw.stem-classifier", "version": "0.1.0", "task": "stem-classification",
//         "file": "stem_classifier.onnx", "providers": ["directml", "cpu"] }
//     ]
//   }
//
// Relative "file" paths resolve against the manifest's directory. Registration
// does not load the model; InferenceBackend::load does that lazily.

#include <daw/ai/InferenceBackend.h>
#include <daw/core/Result.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

namespace daw {

class ModelRegistry {
public:
    Status add(ModelDescriptor model);
    std::optional<ModelDescriptor> find(const std::string& id) const;
    std::vector<ModelDescriptor> list() const;

    /// Registers every model in the manifest. Returns how many were added;
    /// fails without registering anything if any entry is invalid.
    Result<int> loadManifest(const std::filesystem::path& manifest);

private:
    mutable std::mutex mutex_;
    std::vector<ModelDescriptor> models_;
};

std::optional<ExecutionProvider> executionProviderFromString(const std::string& text);

} // namespace daw
