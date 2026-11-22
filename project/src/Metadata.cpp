// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/project/Metadata.h>

#include <daw/core/Files.h>
#include <daw/core/Ids.h>

#include <algorithm>
#include <utility>

namespace daw {

namespace {

using Member = std::string MetadataFields::*;

struct FieldName {
    const char* key;
    Member member;
};

constexpr FieldName kFields[] = {
    {"title", &MetadataFields::title},       {"artist", &MetadataFields::artist},
    {"albumArtist", &MetadataFields::albumArtist}, {"album", &MetadataFields::album},
    {"trackNumber", &MetadataFields::trackNumber}, {"year", &MetadataFields::year},
    {"genre", &MetadataFields::genre},       {"composer", &MetadataFields::composer},
    {"publisher", &MetadataFields::publisher}, {"copyright", &MetadataFields::copyright},
    {"comment", &MetadataFields::comment},   {"isrc", &MetadataFields::isrc},
    {"url", &MetadataFields::url},           {"artworkPath", &MetadataFields::artworkPath},
};

void replaceAll(std::string& text, std::string_view token, std::string_view value)
{
    for (std::size_t pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + value.size()))
        text.replace(pos, token.size(), value);
}

} // namespace

bool MetadataFields::empty() const noexcept
{
    return std::all_of(std::begin(kFields), std::end(kFields), [this](const FieldName& f) { return (this->*f.member).empty(); });
}

const ArtistProfile* ProfileLibrary::find(std::string_view id) const noexcept
{
    const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const ArtistProfile& p) { return p.id == id; });
    return it == profiles.end() ? nullptr : &*it;
}

ArtistProfile* ProfileLibrary::find(std::string_view id) noexcept
{
    const auto it = std::find_if(profiles.begin(), profiles.end(), [&](const ArtistProfile& p) { return p.id == id; });
    return it == profiles.end() ? nullptr : &*it;
}

nlohmann::json metadataToJson(const MetadataFields& fields)
{
    nlohmann::json json = nlohmann::json::object();
    for (const FieldName& f : kFields)
        if (!(fields.*f.member).empty())
            json[f.key] = fields.*f.member;
    return json;
}

MetadataFields metadataFromJson(const nlohmann::json& json)
{
    MetadataFields fields;
    if (!json.is_object())
        return fields;
    for (const FieldName& f : kFields) {
        const auto it = json.find(f.key);
        if (it != json.end() && it->is_string())
            fields.*f.member = it->get<std::string>();
    }
    return fields;
}

nlohmann::json profileLibraryToJson(const ProfileLibrary& library)
{
    nlohmann::json profiles = nlohmann::json::array();
    for (const ArtistProfile& p : library.profiles)
        profiles.push_back({{"id", p.id}, {"name", p.name}, {"fields", metadataToJson(p.fields)}});
    return {{"format", "stem-dawg-profiles"},
            {"schemaVersion", ProfileLibrary::kSchemaVersion},
            {"activeProfileId", library.activeProfileId},
            {"profiles", std::move(profiles)}};
}

Result<ProfileLibrary> profileLibraryFromJson(const nlohmann::json& json)
{
    const std::string format = json.is_object() ? json.value("format", std::string()) : std::string();
    if (format != "stem-dawg-profiles" && format != "industrial-daw-profiles") // legacy name accepted
        return makeError(ErrorCode::Corrupt, "not a Stem DAWg profile library");
    if (json.value("schemaVersion", 0) > ProfileLibrary::kSchemaVersion)
        return makeError(ErrorCode::VersionTooNew, "profile library was written by a newer version");
    ProfileLibrary library;
    library.activeProfileId = json.value("activeProfileId", std::string());
    for (const auto& p : json.value("profiles", nlohmann::json::array())) {
        if (!p.is_object())
            continue;
        ArtistProfile profile;
        profile.id = p.value("id", std::string());
        profile.name = p.value("name", std::string());
        profile.fields = metadataFromJson(p.value("fields", nlohmann::json::object()));
        if (profile.id.empty() || library.find(profile.id) != nullptr)
            profile.id = generateUuid();
        library.profiles.push_back(std::move(profile));
    }
    if (library.find(library.activeProfileId) == nullptr)
        library.activeProfileId = library.profiles.empty() ? std::string() : library.profiles.front().id;
    return library;
}

Result<ProfileLibrary> loadProfileLibrary(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return ProfileLibrary{};
    auto bytes = readFileBytes(path);
    if (!bytes)
        return bytes.error();
    const auto json = nlohmann::json::parse(*bytes, nullptr, false);
    if (json.is_discarded())
        return makeError(ErrorCode::Corrupt, "profile library is not valid JSON: " + pathToUtf8(path));
    return profileLibraryFromJson(json);
}

Status saveProfileLibrary(const std::filesystem::path& path, const ProfileLibrary& library)
{
    return writeFileAtomically(path, profileLibraryToJson(library).dump(2));
}

std::filesystem::path defaultProfileLibraryPath()
{
    return appDataDirectory() / "profiles.json";
}

ArtistProfile makeProfile(std::string name)
{
    ArtistProfile profile;
    profile.id = generateUuid();
    profile.fields.artist = name;
    profile.fields.albumArtist = name;
    profile.fields.copyright = "\xC2\xA9 {year} {artist}";
    profile.name = std::move(name);
    return profile;
}

MetadataFields resolveMetadata(const MetadataFields& song, const ArtistProfile* profile, std::string_view fallbackTitle,
                               std::string_view currentYear)
{
    MetadataFields out = profile != nullptr ? profile->fields : MetadataFields{};
    for (const FieldName& f : kFields)
        if (!(song.*f.member).empty())
            out.*f.member = song.*f.member;
    if (out.title.empty())
        out.title = std::string(fallbackTitle);
    if (out.year.empty())
        out.year = std::string(currentYear);
    if (out.albumArtist.empty())
        out.albumArtist = out.artist;

    for (std::string* text : {&out.copyright, &out.comment}) {
        replaceAll(*text, "{year}", out.year);
        replaceAll(*text, "{artist}", out.artist);
        replaceAll(*text, "{title}", out.title);
        replaceAll(*text, "{album}", out.album);
    }
    return out;
}

const std::vector<std::string>& suggestedGenres()
{
    static const std::vector<std::string> genres{
        "Industrial", "Industrial Metal", "Industrial Rock", "EBM", "Aggrotech", "Dark Electro", "Darkwave",
        "Techno", "Industrial Techno", "Hard Techno", "Dark Techno", "Electronic", "Synthwave", "Cyberpunk",
        "Electro", "Trance", "House", "Ambient", "Metal", "Rock", "Pop", "Hip-Hop", "Soundtrack",
    };
    return genres;
}

} // namespace daw
