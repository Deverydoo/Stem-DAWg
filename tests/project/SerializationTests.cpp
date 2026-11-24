// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/project/ProjectSerializer.h>

#include "ProjectFixtures.h"

#include <catch2/catch_test_macros.hpp>

using namespace daw;
using namespace daw::test;

TEST_CASE("Project round-trips through JSON without loss", "[project][serialize]")
{
    const ProjectModel original = makeRichProject();
    const std::string text = serializeProject(original);
    const auto parsed = parseProject(text);
    REQUIRE(parsed.ok());
    REQUIRE(*parsed == original);
    REQUIRE(serializeProject(*parsed) == text); // stable output
}

TEST_CASE("Serialized project declares format and schema version", "[project][serialize]")
{
    const auto doc = projectToJson(makeRichProject());
    REQUIRE(doc["format"] == "stem-dawg-project");
    REQUIRE(doc["schemaVersion"] == 1);
    REQUIRE(doc["media"][0]["sourceKey"] == "fedcba9876543210"); // hex string: safe for JS tooling
    REQUIRE(doc["media"][0]["frames"] == 9'000'000'000LL);
    REQUIRE(doc["media"][1]["storage"] == "referenced");
}

TEST_CASE("Parser rejects invalid documents with specific errors", "[project][serialize]")
{
    REQUIRE(parseProject("{ not json").error().code == ErrorCode::ParseError);
    REQUIRE(parseProject("[]").error().code == ErrorCode::ParseError);

    auto doc = projectToJson(makeRichProject());
    doc["format"] = "some-other-daw";
    REQUIRE(projectFromJson(doc).error().code == ErrorCode::UnsupportedFormat);

    doc = projectToJson(makeRichProject());
    doc["schemaVersion"] = 99;
    REQUIRE(projectFromJson(doc).error().code == ErrorCode::VersionTooNew);

    doc = projectToJson(makeRichProject());
    doc.erase("timeline");
    REQUIRE(projectFromJson(doc).error().code == ErrorCode::ParseError);

    doc = projectToJson(makeRichProject());
    doc["media"][0]["storage"] = "teleported";
    REQUIRE(projectFromJson(doc).error().code == ErrorCode::ParseError);
}

TEST_CASE("Validation catches dangling references and duplicate ids", "[project][validate]")
{
    ProjectModel model = makeRichProject();
    REQUIRE(validateProject(model).ok());

    model.tracks[0].clips[0].media = MediaId("does-not-exist");
    REQUIRE(validateProject(model).error().code == ErrorCode::Corrupt);

    model = makeRichProject();
    model.tracks[1].id = model.tracks[0].id;
    REQUIRE_FALSE(validateProject(model).ok());

    model = makeRichProject();
    model.tracks[0].clips[0].length = -1;
    REQUIRE_FALSE(validateProject(model).ok());

    model = makeRichProject();
    model.sampleRate = 0.0;
    REQUIRE_FALSE(validateProject(model).ok());

    model = makeRichProject();
    model.tracks[0].pan = 1.5;
    REQUIRE_FALSE(validateProject(model).ok());
}

TEST_CASE("Optional fields fall back to defaults", "[project][serialize]")
{
    auto doc = projectToJson(makeEmptyProject("minimal"));
    doc.erase("mixer");
    doc.erase("transport");
    doc["timeline"].erase("tempo");
    const auto parsed = projectFromJson(doc);
    REQUIRE(parsed.ok());
    REQUIRE(parsed->tempo.bpm == 120.0);
    REQUIRE(parsed->masterGainDb == 0.0);
    REQUIRE(parsed->master == MasterSettings{});
    REQUIRE(parsed->playhead == 0);
}

TEST_CASE("The master bus is written only when set, and clamped when read", "[project][serialize][master]")
{
    CHECK_FALSE(projectToJson(makeEmptyProject("plain"))["mixer"].contains("master"));
    auto doc = projectToJson(makeRichProject());
    CHECK(doc["mixer"]["master"]["limiter"] == true);
    CHECK(doc["mixer"]["master"]["ceilingDbtp"] == -0.5);
    CHECK(doc["mixer"]["master"]["releaseMs"] == 150.0);
    CHECK(doc["mixer"]["master"]["targetLufs"] == -10.0);
    CHECK(doc["mixer"]["master"]["eq"]["enabled"] == true);
    CHECK(doc["mixer"]["master"]["eq"]["gainsDb"][3] == 2.5);
    CHECK(doc["mixer"]["master"]["tonalTarget"]["name"] == "Techno refs");
    CHECK(doc["mixer"]["master"]["tonalTarget"]["meanDb"].size() == 28);
    CHECK(doc["mixer"]["master"]["multiband"]["ratios"][2] == 3.0);
    CHECK(doc["mixer"]["master"]["clipper"]["belowCeilingDb"] == 1.5);
    auto parsed = projectFromJson(doc);
    REQUIRE(parsed.ok());
    CHECK(parsed->master == makeRichProject().master);

    doc["mixer"]["master"] = {{"limiter", "yes"},
                              {"ceilingDbtp", 6.0},
                              {"releaseMs", 1.0},
                              {"targetLufs", "loud"},
                              {"eq", {{"enabled", true}, {"gainsDb", {40.0, "x", -3.0}}}},
                              {"tonalTarget", {{"name", "short"}, {"meanDb", {1.0, 2.0}}, {"spreadDb", {1.0, 2.0}}}},
                              {"multiband", {{"enabled", true}, {"thresholdsDb", {-80.0, 5.0}}, {"ratios", {0.5, 50.0, 2.5}}}},
                              {"clipper", {{"enabled", true}, {"belowCeilingDb", 20.0}}}};
    parsed = projectFromJson(doc);
    REQUIRE(parsed.ok());
    CHECK_FALSE(parsed->master.limiter);
    CHECK(parsed->master.ceilingDbtp == MasterSettings::kMaxCeilingDbtp);
    CHECK(parsed->master.releaseMs == MasterSettings::kMinReleaseMs);
    CHECK(parsed->master.targetLufs == MasterSettings{}.targetLufs);
    CHECK(parsed->master.eq);
    CHECK(parsed->master.eqGainDb == std::array<double, 5>{MasterSettings::kMaxEqDb, 0.0, -3.0, 0.0, 0.0});
    CHECK(parsed->master.tonalTarget.empty()); // two bands instead of 28: dropped
    CHECK(parsed->master.multiband);
    CHECK(parsed->master.multibandThresholdDb == std::array<double, 3>{MasterSettings::kMinThresholdDb, 0.0, -20.0});
    CHECK(parsed->master.multibandRatio == std::array<double, 3>{1.0, MasterSettings::kMaxRatio, 2.5});
    CHECK(parsed->master.clipper);
    CHECK(parsed->master.clipBelowCeilingDb == 6.0);
}

TEST_CASE("Track pan and solo are optional and default to centre and off", "[project][serialize]")
{
    // Files written before the mixer fields existed have no "pan" or "solo".
    auto doc = projectToJson(makeRichProject());
    for (auto& track : doc["tracks"]) {
        track.erase("pan");
        track.erase("solo");
    }
    auto parsed = projectFromJson(doc);
    REQUIRE(parsed.ok());
    for (const Track& track : parsed->tracks) {
        REQUIRE(track.pan == 0.0);
        REQUIRE_FALSE(track.soloed);
    }

    // Out-of-range pan from a hand-edited file is clamped rather than rejected.
    doc["tracks"][0]["pan"] = -7.0;
    parsed = projectFromJson(doc);
    REQUIRE(parsed.ok());
    REQUIRE(parsed->tracks[0].pan == -1.0);
}

TEST_CASE("Track clean-up is written only when set, optional when read, and clamped", "[project][serialize][cleanup]")
{
    auto doc = projectToJson(makeRichProject());
    REQUIRE(doc["tracks"][0].contains("cleanup"));
    CHECK(doc["tracks"][0]["cleanup"]["highPass"]["slopeDbPerOctave"] == 48);
    CHECK(doc["tracks"][0]["cleanup"]["expander"]["thresholdDb"] == -47.0);
    CHECK(doc["tracks"][0]["cleanup"]["transient"]["attackDb"] == 4.5);
    CHECK(doc["tracks"][0]["cleanup"]["transient"]["sustainDb"] == -3.0);
    CHECK(doc["tracks"][0]["cleanup"]["resonance"]["harshDb"] == 4.0);
    CHECK(doc["tracks"][0]["cleanup"]["resonance"]["shimmerDb"] == 7.5);
    CHECK(doc["tracks"][0]["cleanup"]["stereo"]["monoBelowHz"] == 90.0);
    CHECK(doc["tracks"][0]["cleanup"]["stereo"]["widthPercent"] == 80.0);
    CHECK(doc["tracks"][0]["cleanup"]["stereo"]["phaseAlign"] == true);
    CHECK_FALSE(doc["tracks"][1].contains("cleanup")); // default chain: nothing written

    // Older files have no clean-up: every stage is off.
    doc["tracks"][0].erase("cleanup");
    auto parsed = projectFromJson(doc);
    REQUIRE(parsed.ok());
    CHECK(parsed->tracks[0].cleanup == TrackCleanup{});

    // Hand-edited nonsense is clamped or ignored, never rejected.
    doc["tracks"][0]["cleanup"] = {{"dcRemoval", "yes"},
                                   {"highPass", {{"enabled", true}, {"frequencyHz", 5.0}, {"slopeDbPerOctave", 30}}},
                                   {"expander", {{"enabled", true}, {"thresholdDb", 12.0}, {"ratio", 0.5}, {"rangeDb", "a lot"}, {"releaseMs", 1e9}}},
                                   {"transient", {{"enabled", true}, {"attackDb", 99.0}, {"sustainDb", "loud"}}},
                                   {"resonance", {{"enabled", true}, {"harshDb", -4.0}, {"shimmerDb", 50.0}}},
                                   {"stereo", {{"enabled", true}, {"monoBass", "on"}, {"monoBelowHz", 5000.0}, {"widthPercent", -20.0}}}};
    parsed = projectFromJson(doc);
    REQUIRE(parsed.ok());
    const TrackCleanup& c = parsed->tracks[0].cleanup;
    CHECK_FALSE(c.dcRemoval);
    CHECK(c.highPass.enabled);
    CHECK(c.highPass.frequencyHz == HighPassSettings::kMinHz);
    CHECK(c.highPass.slopeDbPerOctave == 24);
    CHECK(c.expander.thresholdDb == 0.0);
    CHECK(c.expander.ratio == 1.0);
    CHECK(c.expander.rangeDb == ExpanderSettings{}.rangeDb);
    CHECK(c.expander.releaseMs == 2000.0);
    CHECK(c.transient.enabled);
    CHECK(c.transient.attackDb == TransientSettings::kMaxDb);
    CHECK(c.transient.sustainDb == TransientSettings{}.sustainDb);
    CHECK(c.resonance.enabled);
    CHECK(c.resonance.harshDb == 0.0);
    CHECK(c.resonance.shimmerDb == ResonanceSettings::kMaxDb);
    CHECK(c.stereo.enabled);
    CHECK(c.stereo.monoBass == StereoSettings{}.monoBass);
    CHECK(c.stereo.monoBelowHz == StereoSettings::kMaxHz);
    CHECK(c.stereo.widthPercent == 0.0);
    CHECK_FALSE(c.stereo.phaseAlign);
}

TEST_CASE("Projects saved under the old Industrial DAW format id still open", "[project][serialize][regression]")
{
    const ProjectModel original = makeRichProject();
    auto doc = projectToJson(original);
    doc["format"] = ProjectModel::kLegacyFormatId;
    const auto parsed = projectFromJson(doc);
    REQUIRE(parsed.ok());
    REQUIRE(*parsed == original);
    REQUIRE(projectToJson(*parsed)["format"] == "stem-dawg-project"); // re-saved under the new id

    doc["format"] = "some-other-app";
    REQUIRE(projectFromJson(doc).error().code == ErrorCode::UnsupportedFormat);
}
