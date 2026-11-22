// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Stable, strongly typed identifiers. Every persisted entity (project, track,
// clip, media, analysis record) carries a UUIDv4 that never changes for the
// lifetime of the entity, so routing, automation and AI results can refer to
// it across saves, moves and renames.

#include <compare>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace daw {

/// Returns a random RFC 4122 version-4 UUID in lowercase canonical form.
std::string generateUuid();

/// True if `text` is a canonical 36-character UUID (any version).
bool isCanonicalUuid(std::string_view text) noexcept;

template <typename Tag>
class Id {
public:
    Id() = default;
    explicit Id(std::string value) : value_(std::move(value)) {}

    static Id generate() { return Id(generateUuid()); }

    const std::string& str() const noexcept { return value_; }
    bool valid() const noexcept { return !value_.empty(); }

    friend bool operator==(const Id&, const Id&) = default;
    friend auto operator<=>(const Id&, const Id&) = default;

private:
    std::string value_;
};

struct ProjectIdTag {};
struct TrackIdTag {};
struct ClipIdTag {};
struct MediaIdTag {};
struct AnalysisIdTag {};

using ProjectId = Id<ProjectIdTag>;
using TrackId = Id<TrackIdTag>;
using ClipId = Id<ClipIdTag>;
using MediaId = Id<MediaIdTag>;
using AnalysisId = Id<AnalysisIdTag>;

} // namespace daw

template <typename Tag>
struct std::hash<daw::Id<Tag>> {
    std::size_t operator()(const daw::Id<Tag>& id) const noexcept { return std::hash<std::string>{}(id.str()); }
};
