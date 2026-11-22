// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/Files.h>
#include <daw/project/ProjectSerializer.h>
#include <daw/project/ProjectStore.h>

#include "ProjectFixtures.h"
#include "TempDir.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace daw;
using namespace daw::test;
namespace fs = std::filesystem;

namespace {
int countBackups(const fs::path& root)
{
    int n = 0;
    for (const auto& e : fs::directory_iterator(ProjectStore::backupsDirectory(root)))
        n += pathToUtf8(e.path().filename()).rfind("project-", 0) == 0 ? 1 : 0;
    return n;
}

ProjectModel copiedOnlyProject()
{
    ProjectModel model = makeRichProject();
    model.media.pop_back();
    model.tracks.pop_back();
    return model;
}
} // namespace

TEST_CASE("Save creates the documented layout and load restores the model", "[project][store]")
{
    TempDir dir("store");
    const fs::path root = dir / "My Project";
    const ProjectModel model = makeRichProject();
    REQUIRE(ProjectStore::save(root, model).ok());

    for (const char* sub : {"Media/Original", "Media/Recorded", "Media/Samples", "Analysis", "Cache/waveforms",
                            "Cache/spectrograms", "Presets", "PluginStates", "Backups", "Exports"})
        REQUIRE(fs::is_directory(root / utf8ToPath(sub)));
    REQUIRE(ProjectStore::isProjectDirectory(root));

    const auto loaded = ProjectStore::load(root);
    REQUIRE(loaded.ok());
    REQUIRE(*loaded == model);
}

TEST_CASE("Saving keeps rotating backups and leaves no temporary files", "[project][store]")
{
    TempDir dir("backups");
    const fs::path root = dir.path();
    ProjectModel model = makeRichProject();
    SaveOptions options;
    options.maxBackups = 3;
    for (int i = 0; i < 6; ++i) {
        model.name = "rev " + std::to_string(i);
        REQUIRE(ProjectStore::save(root, model, options).ok());
    }
    REQUIRE(countBackups(root) == 3);
    REQUIRE(ProjectStore::load(root)->name == "rev 5");

    for (const auto& e : fs::directory_iterator(root))
        REQUIRE(pathToUtf8(e.path().filename()).find(".tmp") == std::string::npos);
}

TEST_CASE("Invalid models are refused without touching the existing manifest", "[project][store]")
{
    TempDir dir("refuse");
    ProjectModel model = makeRichProject();
    REQUIRE(ProjectStore::save(dir.path(), model).ok());
    model.tracks[0].clips[0].media = MediaId("dangling");
    REQUIRE_FALSE(ProjectStore::save(dir.path(), model).ok());
    REQUIRE(ProjectStore::load(dir.path())->tracks[0].clips[0].media != MediaId("dangling"));
}

TEST_CASE("Autosave is recoverable only while newer than the manifest", "[project][store]")
{
    TempDir dir("autosave");
    ProjectModel model = makeRichProject();
    REQUIRE(ProjectStore::save(dir.path(), model).ok());
    REQUIRE_FALSE(ProjectStore::findRecoverableAutosave(dir.path()).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    model.name = "unsaved edits";
    REQUIRE(ProjectStore::writeAutosave(dir.path(), model).ok());
    REQUIRE(ProjectStore::findRecoverableAutosave(dir.path()).has_value());
    REQUIRE(ProjectStore::loadAutosave(dir.path())->name == "unsaved edits");

    REQUIRE(ProjectStore::save(dir.path(), model).ok()); // explicit save supersedes the autosave
    REQUIRE_FALSE(ProjectStore::findRecoverableAutosave(dir.path()).has_value());
}

TEST_CASE("Copied media is portable when the project folder moves", "[project][store][media]")
{
    TempDir dir("portable");
    const fs::path source = dir / "incoming" / "suno_drums.wav";
    fs::create_directories(source.parent_path());
    REQUIRE(writeFileAtomically(source, "RIFF....fake").ok());

    const fs::path root = dir / "ProjectA";
    REQUIRE(ProjectStore::createLayout(root).ok());
    const auto stored = ProjectStore::stageMediaFile(root, source, MediaStorage::Copied);
    REQUIRE(stored.ok());
    REQUIRE(*stored == "Media/Original/suno_drums.wav");

    // A second import of a same-named file gets a unique name.
    const auto second = ProjectStore::stageMediaFile(root, source, MediaStorage::Copied);
    REQUIRE(*second == "Media/Original/suno_drums (2).wav");

    ProjectModel model = copiedOnlyProject();
    model.media[0].path = *stored;
    REQUIRE(ProjectStore::save(root, model).ok());

    const fs::path moved = dir / utf8ToPath("Moved — Ünïcødé");
    fs::rename(root, moved);
    const auto loaded = ProjectStore::load(moved);
    REQUIRE(loaded.ok());
    REQUIRE(fs::is_regular_file(ProjectStore::resolveMediaPath(moved, loaded->media[0])));
}

TEST_CASE("Referenced media keeps an absolute path", "[project][store][media]")
{
    TempDir dir("referenced");
    const fs::path source = dir / "elsewhere.wav";
    REQUIRE(writeFileAtomically(source, "x").ok());
    const auto stored = ProjectStore::stageMediaFile(dir / "P", source, MediaStorage::Referenced);
    REQUIRE(stored.ok());
    REQUIRE(utf8ToPath(*stored).is_absolute());
    REQUIRE_FALSE(fs::exists(dir / "P" / "Media" / "Original" / "elsewhere.wav"));
    REQUIRE(ProjectStore::stageMediaFile(dir / "P", dir / "nope.wav", MediaStorage::Copied).error().code == ErrorCode::NotFound);
}
