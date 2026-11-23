// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// ProjectModel <-> project.json (schema v1). See the spec, section 7.2, for
// the annotated document layout.

#include <daw/core/Result.h>
#include <daw/project/Migrations.h>
#include <daw/project/ProjectModel.h>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace daw {

nlohmann::json projectToJson(const ProjectModel& model);
Result<ProjectModel> projectFromJson(nlohmann::json document,
                                     const MigrationRegistry& migrations = MigrationRegistry::builtin());

std::string serializeProject(const ProjectModel& model);
Result<ProjectModel> parseProject(std::string_view text,
                                  const MigrationRegistry& migrations = MigrationRegistry::builtin());

/// A track's clean-up chain as stored in project.json (also used by reports).
nlohmann::json cleanupToJson(const TrackCleanup& cleanup);
/// Reads a clean-up object; missing or invalid values take their defaults and
/// numbers are clamped to their ranges.
TrackCleanup cleanupFromJson(const nlohmann::json& json);

/// Structural validation: positive rates, unique ids, clips reference media,
/// non-negative positions and lengths.
Status validateProject(const ProjectModel& model);

const char* toString(MediaStorage storage) noexcept;

} // namespace daw
