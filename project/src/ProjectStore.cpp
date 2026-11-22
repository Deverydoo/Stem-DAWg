// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/project/ProjectStore.h>

#include <daw/core/Files.h>
#include <daw/core/Logger.h>
#include <daw/project/ProjectSerializer.h>

#include <algorithm>
#include <vector>

namespace fs = std::filesystem;

namespace daw {

namespace {

const char* const kLayout[] = {
    "Media/Original", "Media/Recorded", "Media/Samples", "Analysis",  "Cache/waveforms",
    "Cache/spectrograms", "Presets",    "PluginStates",  "Backups",  "Exports",
};

Status ioError(const std::string& what, const std::error_code& ec)
{
    return makeError(ErrorCode::IoError, what + ": " + ec.message());
}

void pruneBackups(const fs::path& dir, int maxBackups)
{
    std::error_code ec;
    std::vector<fs::path> backups;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = pathToUtf8(entry.path().filename());
        if (entry.is_regular_file() && name.rfind("project-", 0) == 0 && entry.path().extension() == ".json")
            backups.push_back(entry.path());
    }
    if (static_cast<int>(backups.size()) <= maxBackups)
        return;
    std::sort(backups.begin(), backups.end()); // timestamped names sort chronologically
    const std::size_t excess = backups.size() - static_cast<std::size_t>(std::max(maxBackups, 0));
    for (std::size_t i = 0; i < excess; ++i)
        fs::remove(backups[i], ec);
}

} // namespace

Status ProjectStore::createLayout(const fs::path& root)
{
    std::error_code ec;
    for (const char* sub : kLayout) {
        fs::create_directories(root / utf8ToPath(sub), ec);
        if (ec)
            return ioError("cannot create project directory " + std::string(sub), ec);
    }
    return Status::success();
}

Status ProjectStore::save(const fs::path& root, const ProjectModel& model, const SaveOptions& options)
{
    if (Status valid = validateProject(model); !valid)
        return valid;
    if (Status layout = createLayout(root); !layout)
        return layout;

    const fs::path manifest = root / kManifestName;
    std::error_code ec;
    if (options.keepBackup && fs::exists(manifest, ec)) {
        const fs::path dir = backupsDirectory(root);
        fs::path backup = dir / utf8ToPath("project-" + utcTimestampForFilename() + ".json");
        for (int n = 1; fs::exists(backup, ec); ++n)
            backup = dir / utf8ToPath("project-" + utcTimestampForFilename() + "-" + std::to_string(n) + ".json");
        fs::copy_file(manifest, backup, fs::copy_options::overwrite_existing, ec);
        if (ec)
            logging::warn("project", "backup failed", {{"error", ec.message()}});
        else
            pruneBackups(dir, options.maxBackups);
    }

    if (Status written = writeFileAtomically(manifest, serializeProject(model)); !written)
        return written;
    discardAutosave(root);
    logging::info("project", "saved", {{"path", pathToUtf8(manifest)}, {"tracks", model.tracks.size()}});
    return Status::success();
}

Result<ProjectModel> ProjectStore::load(const fs::path& root)
{
    auto text = readFileBytes(root / kManifestName);
    if (!text)
        return text.error();
    auto model = parseProject(*text);
    if (model)
        logging::info("project", "loaded", {{"path", pathToUtf8(root / kManifestName)}});
    return model;
}

bool ProjectStore::isProjectDirectory(const fs::path& root)
{
    std::error_code ec;
    return fs::is_regular_file(root / kManifestName, ec);
}

Status ProjectStore::writeAutosave(const fs::path& root, const ProjectModel& model)
{
    if (Status valid = validateProject(model); !valid)
        return valid;
    return writeFileAtomically(backupsDirectory(root) / kAutosaveName, serializeProject(model));
}

std::optional<fs::path> ProjectStore::findRecoverableAutosave(const fs::path& root)
{
    std::error_code ec;
    const fs::path autosave = backupsDirectory(root) / kAutosaveName;
    if (!fs::is_regular_file(autosave, ec))
        return std::nullopt;
    const fs::path manifest = root / kManifestName;
    if (!fs::is_regular_file(manifest, ec))
        return autosave;
    if (fs::last_write_time(autosave, ec) > fs::last_write_time(manifest, ec))
        return autosave;
    return std::nullopt;
}

Result<ProjectModel> ProjectStore::loadAutosave(const fs::path& root)
{
    auto text = readFileBytes(backupsDirectory(root) / kAutosaveName);
    if (!text)
        return text.error();
    return parseProject(*text);
}

void ProjectStore::discardAutosave(const fs::path& root)
{
    std::error_code ec;
    fs::remove(backupsDirectory(root) / kAutosaveName, ec);
}

Result<std::string> ProjectStore::stageMediaFile(const fs::path& root, const fs::path& source, MediaStorage storage)
{
    std::error_code ec;
    if (!fs::is_regular_file(source, ec))
        return makeError(ErrorCode::NotFound, "media file not found: " + pathToUtf8(source));

    if (storage == MediaStorage::Referenced)
        return pathToGenericUtf8(fs::absolute(source, ec));

    const fs::path relativeDir = utf8ToPath("Media/Original");
    const fs::path dir = root / relativeDir;
    fs::create_directories(dir, ec);
    if (ec)
        return makeError(ErrorCode::IoError, "cannot create media directory: " + ec.message());

    // Already inside the project (e.g. re-import): store as-is.
    const fs::path canonicalSource = fs::weakly_canonical(source, ec);
    const fs::path canonicalDir = fs::weakly_canonical(dir, ec);
    if (canonicalSource.parent_path() == canonicalDir)
        return pathToGenericUtf8(relativeDir / canonicalSource.filename());

    fs::path filename = source.filename();
    fs::path destination = dir / filename;
    for (int n = 2; fs::exists(destination, ec); ++n) {
        filename = utf8ToPath(pathToUtf8(source.stem()) + " (" + std::to_string(n) + ")" + pathToUtf8(source.extension()));
        destination = dir / filename;
    }
    fs::copy_file(source, destination, ec);
    if (ec)
        return makeError(ErrorCode::IoError, "cannot copy media into project: " + ec.message());
    return pathToGenericUtf8(relativeDir / filename);
}

fs::path ProjectStore::resolveMediaPath(const fs::path& root, const MediaRef& media)
{
    const fs::path stored = utf8ToPath(media.path);
    if (media.storage == MediaStorage::Copied || stored.is_relative())
        return (root / stored).lexically_normal();
    return stored;
}

fs::path ProjectStore::peakCachePath(const fs::path& root, const MediaId& media)
{
    return root / utf8ToPath("Cache/waveforms") / utf8ToPath(media.str() + ".peaks");
}

fs::path ProjectStore::analysisDirectory(const fs::path& root, const MediaId& media)
{
    return root / utf8ToPath("Analysis") / utf8ToPath(media.str());
}

fs::path ProjectStore::backupsDirectory(const fs::path& root)
{
    return root / utf8ToPath("Backups");
}

} // namespace daw
