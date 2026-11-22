// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Model-level operations shared by the app and headless tests. In Milestone 2
// these become undoable commands; for now they mutate the model directly.

#include <daw/core/Result.h>
#include <daw/project/ProjectModel.h>

#include <filesystem>

namespace daw {

/// Probes `source`, stages it into the project (copy or reference) and returns
/// a MediaRef describing it. Does not modify the model.
Result<MediaRef> importAudioFile(const std::filesystem::path& projectRoot, const std::filesystem::path& source,
                                 MediaStorage storage = MediaStorage::Copied);

/// Adds `media` to the model and creates a new track holding one clip that
/// covers the whole file at `timelineStart`. If the project has no media yet,
/// its sample rate is set to the media's native rate. Returns the new track id.
TrackId addMediaAsNewTrack(ProjectModel& model, const MediaRef& media, std::int64_t timelineStart = 0);

/// Length of `media` expressed in project frames.
std::int64_t mediaLengthInProjectFrames(const ProjectModel& model, const MediaRef& media);

} // namespace daw
