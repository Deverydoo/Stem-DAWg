// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Release metadata and artist profiles.
//
// An ArtistProfile holds the fields that stay the same for every song by one
// artist (artist name, label, copyright line, website, artwork...). It lives in
// the user's profile library (profiles.json in the app data folder), not in
// projects, so it is entered once and shared by all projects.
//
// Each project stores the active profile id plus song-level fields (title,
// album, track number...). At export, song fields override profile fields and
// placeholders in the copyright and comment are expanded:
//   {year} {artist} {title} {album}

#include <daw/core/Result.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace daw {

struct MetadataFields {
    std::string title;
    std::string artist;
    std::string albumArtist;
    std::string album;
    std::string trackNumber;
    std::string year;
    std::string genre;
    std::string composer;
    std::string publisher;
    std::string copyright;
    std::string comment;
    std::string isrc;
    std::string url;
    std::string artworkPath; // UTF-8 path to a PNG or JPEG

    bool empty() const noexcept;
    friend bool operator==(const MetadataFields&, const MetadataFields&) = default;
};

struct ArtistProfile {
    std::string id; // UUID
    std::string name; // shown in the profile switcher
    MetadataFields fields;

    friend bool operator==(const ArtistProfile&, const ArtistProfile&) = default;
};

struct ProfileLibrary {
    static constexpr int kSchemaVersion = 1;
    std::vector<ArtistProfile> profiles;
    std::string activeProfileId; // default for new projects

    const ArtistProfile* find(std::string_view id) const noexcept;
    ArtistProfile* find(std::string_view id) noexcept;
    friend bool operator==(const ProfileLibrary&, const ProfileLibrary&) = default;
};

/// Only non-empty fields are written, so files stay small and readable.
nlohmann::json metadataToJson(const MetadataFields& fields);
MetadataFields metadataFromJson(const nlohmann::json& json);

nlohmann::json profileLibraryToJson(const ProfileLibrary& library);
Result<ProfileLibrary> profileLibraryFromJson(const nlohmann::json& json);

/// A missing file is an empty library, not an error.
Result<ProfileLibrary> loadProfileLibrary(const std::filesystem::path& path);
Status saveProfileLibrary(const std::filesystem::path& path, const ProfileLibrary& library);
/// <app data>/profiles.json
std::filesystem::path defaultProfileLibraryPath();

ArtistProfile makeProfile(std::string name);

/// Merges song fields over profile fields (non-empty wins), falls back to
/// `fallbackTitle` for an empty title and `currentYear` for an empty year, and
/// expands {year} {artist} {title} {album} in copyright and comment.
MetadataFields resolveMetadata(const MetadataFields& song, const ArtistProfile* profile, std::string_view fallbackTitle,
                               std::string_view currentYear);

/// Common genres offered in the UI (free text is always allowed).
const std::vector<std::string>& suggestedGenres();

} // namespace daw
