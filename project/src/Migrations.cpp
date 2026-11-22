// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/project/Migrations.h>

#include <string>

namespace daw {

void MigrationRegistry::add(int fromVersion, MigrationStep step)
{
    steps_[fromVersion] = std::move(step);
}

Result<int> MigrationRegistry::migrate(nlohmann::json& document, int targetVersion) const
{
    if (!document.is_object() || !document.contains("schemaVersion") || !document["schemaVersion"].is_number_integer())
        return makeError(ErrorCode::ParseError, "project document has no integer schemaVersion");

    const int original = document["schemaVersion"].get<int>();
    if (original > targetVersion)
        return makeError(ErrorCode::VersionTooNew, "project schema version " + std::to_string(original)
                                                       + " is newer than supported version "
                                                       + std::to_string(targetVersion));
    if (original < 0)
        return makeError(ErrorCode::ParseError, "negative schemaVersion");

    for (int version = original; version < targetVersion; ++version) {
        const auto it = steps_.find(version);
        if (it == steps_.end())
            return makeError(ErrorCode::MigrationFailed, "no migration from schema version " + std::to_string(version));
        Status status = Status::success();
        try {
            status = it->second(document);
        } catch (const std::exception& e) {
            return makeError(ErrorCode::MigrationFailed, "migration " + std::to_string(version) + " threw: " + e.what());
        }
        if (!status)
            return makeError(ErrorCode::MigrationFailed,
                             "migration " + std::to_string(version) + " failed: " + status.error().describe());
        document["schemaVersion"] = version + 1;
    }
    return original;
}

const MigrationRegistry& MigrationRegistry::builtin()
{
    // Schema v1 is the first released format; no steps yet. Add future steps as:
    //   registry.add(1, [](nlohmann::json& doc) { ...upgrade v1 -> v2...; return Status::success(); });
    static const MigrationRegistry registry;
    return registry;
}

} // namespace daw
