// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Project schema migrations operate on the raw JSON document before it is
// mapped onto ProjectModel. Each step upgrades exactly one version (N -> N+1),
// so a file from any older release is migrated by chaining steps.
//
// Policy (see spec section 7.4):
//  * schemaVersion is bumped for any change that an older build cannot read.
//  * Every bump ships a migration step and a fixture-based test.
//  * Files newer than the running build are refused (never silently truncated).

#include <daw/core/Result.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <map>

namespace daw {

using MigrationStep = std::function<Status(nlohmann::json& document)>;

class MigrationRegistry {
public:
    /// Registers the step that upgrades `fromVersion` to `fromVersion + 1`.
    void add(int fromVersion, MigrationStep step);

    /// Upgrades `document` in place to `targetVersion` and returns the version
    /// the document had originally.
    Result<int> migrate(nlohmann::json& document, int targetVersion) const;

    /// Migrations shipped with the application.
    static const MigrationRegistry& builtin();

private:
    std::map<int, MigrationStep> steps_;
};

} // namespace daw
