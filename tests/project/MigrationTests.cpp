// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/project/Migrations.h>
#include <daw/project/ProjectSerializer.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace daw;
using nlohmann::json;

namespace {

// A hypothetical pre-release "v0" document: positions in seconds, no format tag.
json makeV0Document()
{
    return json::parse(R"({
        "schemaVersion": 0,
        "id": "11111111-2222-4333-8444-555555555555",
        "title": "Prototype session",
        "sampleRate": 48000,
        "playheadSeconds": 1.5,
        "media": [],
        "tracks": []
    })");
}

MigrationRegistry makeTestRegistry()
{
    MigrationRegistry registry;
    registry.add(0, [](json& doc) {
        const double rate = doc.at("sampleRate").get<double>();
        doc["format"] = ProjectModel::kFormatId;
        doc["project"] = {{"id", doc.at("id")}, {"name", doc.value("title", "")}};
        doc["timeline"] = {{"sampleRate", rate}};
        doc["transport"] = {{"playheadFrames", static_cast<std::int64_t>(std::llround(doc.value("playheadSeconds", 0.0) * rate))}};
        for (const char* key : {"id", "title", "sampleRate", "playheadSeconds"})
            doc.erase(key);
        return Status::success();
    });
    return registry;
}

} // namespace

TEST_CASE("Migrations upgrade old documents step by step", "[project][migration]")
{
    const auto registry = makeTestRegistry();
    const auto model = projectFromJson(makeV0Document(), registry);
    REQUIRE(model.ok());
    REQUIRE(model->name == "Prototype session");
    REQUIRE(model->playhead == 72000);
    REQUIRE(model->sampleRate == 48000.0);
}

TEST_CASE("Migration reports the original version and stamps the new one", "[project][migration]")
{
    const auto registry = makeTestRegistry();
    json doc = makeV0Document();
    const auto original = registry.migrate(doc, 1);
    REQUIRE(original.ok());
    REQUIRE(*original == 0);
    REQUIRE(doc["schemaVersion"] == 1);
}

TEST_CASE("Missing or failing migrations are reported, not ignored", "[project][migration]")
{
    json doc = makeV0Document();
    REQUIRE(MigrationRegistry::builtin().migrate(doc, 1).error().code == ErrorCode::MigrationFailed);

    MigrationRegistry failing;
    failing.add(0, [](json&) { return Status(makeError(ErrorCode::Corrupt, "bad data")); });
    doc = makeV0Document();
    REQUIRE(failing.migrate(doc, 1).error().code == ErrorCode::MigrationFailed);

    MigrationRegistry throwing;
    throwing.add(0, [](json& d) {
        (void)d.at("nonexistent");
        return Status::success();
    });
    doc = makeV0Document();
    REQUIRE(throwing.migrate(doc, 1).error().code == ErrorCode::MigrationFailed);

    json noVersion = json::object();
    REQUIRE(MigrationRegistry::builtin().migrate(noVersion, 1).error().code == ErrorCode::ParseError);
}
