// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/ModelRegistry.h>

#include <daw/core/Files.h>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace fs = std::filesystem;

namespace daw {

std::optional<ExecutionProvider> executionProviderFromString(const std::string& text)
{
    if (text == "cpu")
        return ExecutionProvider::Cpu;
    if (text == "directml" || text == "dml")
        return ExecutionProvider::DirectML;
    if (text == "cuda")
        return ExecutionProvider::Cuda;
    return std::nullopt;
}

Status ModelRegistry::add(ModelDescriptor model)
{
    if (model.id.empty())
        return makeError(ErrorCode::InvalidArgument, "model id is empty");
    if (model.inlineBytes.empty()) {
        std::error_code ec;
        if (!fs::is_regular_file(model.file, ec))
            return makeError(ErrorCode::NotFound, "model file not found: " + pathToUtf8(model.file));
    }
    std::lock_guard lock(mutex_);
    if (std::any_of(models_.begin(), models_.end(), [&](const ModelDescriptor& m) { return m.id == model.id; }))
        return makeError(ErrorCode::AlreadyExists, "model already registered: " + model.id);
    models_.push_back(std::move(model));
    return Status::success();
}

std::optional<ModelDescriptor> ModelRegistry::find(const std::string& id) const
{
    std::lock_guard lock(mutex_);
    const auto it = std::find_if(models_.begin(), models_.end(), [&](const ModelDescriptor& m) { return m.id == id; });
    if (it == models_.end())
        return std::nullopt;
    return *it;
}

std::vector<ModelDescriptor> ModelRegistry::list() const
{
    std::lock_guard lock(mutex_);
    return models_;
}

Result<int> ModelRegistry::loadManifest(const fs::path& manifest)
{
    auto text = readFileBytes(manifest);
    if (!text)
        return text.error();
    const nlohmann::json doc = nlohmann::json::parse(*text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("models") || !doc["models"].is_array())
        return makeError(ErrorCode::ParseError, "model manifest must be an object with a 'models' array");

    std::vector<ModelDescriptor> parsed;
    try {
        for (const auto& entry : doc["models"]) {
            ModelDescriptor model;
            model.id = entry.at("id").get<std::string>();
            model.version = entry.value("version", std::string{});
            model.task = entry.value("task", std::string{});
            fs::path file = utf8ToPath(entry.at("file").get<std::string>());
            model.file = file.is_absolute() ? file : manifest.parent_path() / file;
            for (const auto& p : entry.value("providers", nlohmann::json::array())) {
                const auto provider = executionProviderFromString(p.get<std::string>());
                if (!provider)
                    return makeError(ErrorCode::ParseError, "unknown execution provider '" + p.get<std::string>() + "'");
                model.preferredProviders.push_back(*provider);
            }
            std::error_code ec;
            if (!fs::is_regular_file(model.file, ec))
                return makeError(ErrorCode::NotFound, "model file not found: " + pathToUtf8(model.file));
            parsed.push_back(std::move(model));
        }
    } catch (const std::exception& e) {
        return makeError(ErrorCode::ParseError, std::string("invalid model manifest: ") + e.what());
    }

    int added = 0;
    for (auto& model : parsed) {
        if (Status status = add(std::move(model)); !status)
            return status.error();
        ++added;
    }
    return added;
}

} // namespace daw
