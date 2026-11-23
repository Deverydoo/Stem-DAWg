// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Project directory persistence.
//
//   <Project>/
//     project.json            manifest (atomic replace on every save)
//     Media/Original/         imported source files (Copied storage)
//     Media/Processed/        processed copies (voice denoise), derived from Original
//     Media/Recorded/         (Milestone 2)
//     Media/Samples/          (Milestone 5)
//     Analysis/<mediaId>/     analysis results, JSON in M1
//     Cache/waveforms/        <mediaId>.peaks (disposable)
//     Cache/spectrograms/     (Milestone 6)
//     Presets/ PluginStates/  (Milestone 3)
//     Backups/                project-<utc>.json rotation + autosave.json
//     Exports/                (Milestone 7)

#include <daw/core/Result.h>
#include <daw/project/ProjectModel.h>

#include <filesystem>
#include <optional>
#include <string>

namespace daw {

struct SaveOptions {
    bool keepBackup = true;
    int maxBackups = 10;
};

class ProjectStore {
public:
    static constexpr const char* kManifestName = "project.json";
    static constexpr const char* kAutosaveName = "autosave.json";

    static Status createLayout(const std::filesystem::path& root);

    /// Validates, backs up the previous manifest, writes atomically, prunes old
    /// backups and discards any autosave (it is now older than the manifest).
    static Status save(const std::filesystem::path& root, const ProjectModel& model, const SaveOptions& options = {});
    static Result<ProjectModel> load(const std::filesystem::path& root);
    static bool isProjectDirectory(const std::filesystem::path& root);

    static Status writeAutosave(const std::filesystem::path& root, const ProjectModel& model);
    /// Autosave path if it exists and is newer than the manifest.
    static std::optional<std::filesystem::path> findRecoverableAutosave(const std::filesystem::path& root);
    static Result<ProjectModel> loadAutosave(const std::filesystem::path& root);
    static void discardAutosave(const std::filesystem::path& root);

    /// Copies (Copied) or validates (Referenced) a source file and returns the
    /// path string to store in MediaRef::path.
    static Result<std::string> stageMediaFile(const std::filesystem::path& root, const std::filesystem::path& source,
                                              MediaStorage storage);
    static std::filesystem::path resolveMediaPath(const std::filesystem::path& root, const MediaRef& media);

    static std::filesystem::path peakCachePath(const std::filesystem::path& root, const MediaId& media);
    static std::filesystem::path analysisDirectory(const std::filesystem::path& root, const MediaId& media);
    static std::filesystem::path backupsDirectory(const std::filesystem::path& root);
};

} // namespace daw
