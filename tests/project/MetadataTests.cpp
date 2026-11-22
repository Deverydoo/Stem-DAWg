// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/project/Metadata.h>
#include <daw/project/ProjectSerializer.h>

#include "ProjectFixtures.h"
#include "TempDir.h"

#include <catch2/catch_test_macros.hpp>

using namespace daw;
using namespace daw::test;

TEST_CASE("Song fields override profile fields and placeholders expand", "[project][metadata]")
{
    ArtistProfile profile = makeProfile("Example Artist");
    profile.fields.publisher = "Example Records";
    profile.fields.genre = "Industrial";
    profile.fields.comment = "{title} by {artist}, {year}";

    MetadataFields song;
    song.title = "Test Song";
    song.genre = "Industrial Techno";
    song.album = "Rust Belt Liturgy";

    const MetadataFields m = resolveMetadata(song, &profile, "fallback", "2026");
    REQUIRE(m.title == "Test Song");
    REQUIRE(m.artist == "Example Artist");
    REQUIRE(m.albumArtist == "Example Artist");
    REQUIRE(m.genre == "Industrial Techno");          // song wins
    REQUIRE(m.publisher == "Example Records");         // inherited
    REQUIRE(m.year == "2026");                          // default year
    REQUIRE(m.copyright == "\xC2\xA9 2026 Example Artist");
    REQUIRE(m.comment == "Test Song by Example Artist, 2026");

    // No profile, empty song: title falls back to the project name.
    const MetadataFields bare = resolveMetadata({}, nullptr, "My Project", "2027");
    REQUIRE(bare.title == "My Project");
    REQUIRE(bare.year == "2027");
    REQUIRE(bare.artist.empty());
}

TEST_CASE("Profile library round-trips and repairs bad input", "[project][metadata]")
{
    TempDir dir("profiles");
    const auto path = dir / "profiles.json";

    auto missing = loadProfileLibrary(path);
    REQUIRE(missing.ok());
    REQUIRE(missing->profiles.empty());

    ProfileLibrary library;
    library.profiles.push_back(makeProfile("Artist One"));
    library.profiles.push_back(makeProfile("Ärtist Twø"));
    library.profiles[1].fields.url = "https://example.com";
    library.activeProfileId = library.profiles[1].id;
    REQUIRE(saveProfileLibrary(path, library).ok());
    auto loaded = loadProfileLibrary(path);
    REQUIRE(loaded.ok());
    REQUIRE(*loaded == library);

    // Duplicate ids are re-issued; a dangling active id falls back to the first profile.
    auto json = profileLibraryToJson(library);
    json["profiles"][1]["id"] = json["profiles"][0]["id"];
    json["activeProfileId"] = "nope";
    auto repaired = profileLibraryFromJson(json);
    REQUIRE(repaired.ok());
    REQUIRE(repaired->profiles[0].id != repaired->profiles[1].id);
    REQUIRE(repaired->activeProfileId == repaired->profiles[0].id);

    json["schemaVersion"] = 99;
    REQUIRE(profileLibraryFromJson(json).error().code == ErrorCode::VersionTooNew);
    REQUIRE(profileLibraryFromJson(nlohmann::json::object()).error().code == ErrorCode::Corrupt);
}

TEST_CASE("Project release metadata is saved and optional", "[project][metadata][serialize]")
{
    ProjectModel model = makeRichProject();
    model.profileId = "4a1c3f9e-0000-4000-8000-000000000001";
    model.release.title = "Test Song";
    model.release.trackNumber = "1";
    const auto parsed = parseProject(serializeProject(model));
    REQUIRE(parsed.ok());
    REQUIRE(parsed->profileId == model.profileId);
    REQUIRE(parsed->release == model.release);

    // Absent block: empty fields; and no block is written for empty metadata.
    const auto empty = projectToJson(makeEmptyProject("x"));
    REQUIRE_FALSE(empty.contains("release"));
    REQUIRE(projectFromJson(empty)->release.empty());
}
