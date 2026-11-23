// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Model-level operations shared by the app and headless tests. They mutate
// the model directly; the app makes them undoable with daw::UndoStack.

#include <daw/core/Result.h>
#include <daw/project/ProjectModel.h>

#include <filesystem>
#include <string>
#include <vector>

namespace daw {

/// Probes `source`, stages it into the project (copy or reference) and returns
/// a MediaRef describing it. Does not modify the model.
/// Stages an audio or MIDI file in the project. MIDI files are recorded as
/// stereo media at `midiSampleRate` (the rate they will be rendered at:
/// normally the project's sample rate).
Result<MediaRef> importAudioFile(const std::filesystem::path& projectRoot, const std::filesystem::path& source,
                                 MediaStorage storage = MediaStorage::Copied, double midiSampleRate = 48000.0);

/// Adds `media` to the model and creates a new track holding one clip that
/// covers the whole file at `timelineStart`. If the project has no media yet,
/// its sample rate is set to the media's native rate. Returns the new track id.
TrackId addMediaAsNewTrack(ProjectModel& model, const MediaRef& media, std::int64_t timelineStart = 0);

/// Length of `media` expressed in project frames.
std::int64_t mediaLengthInProjectFrames(const ProjectModel& model, const MediaRef& media);

/// Solo or un-solo a track. `exclusive` solo (the default click) un-solos
/// every other track first; additive solo (Ctrl+click) keeps them.
/// Returns false when nothing changed or the track does not exist.
bool setTrackSolo(ProjectModel& model, const TrackId& track, bool soloed, bool exclusive);

/// Short track names for a batch of files imported together: removes the
/// text every name shares at the start and end, when it ends at a separator
/// ("Song_ (Bass)", "Song_ (Drum Kit)" -> "Bass", "Drum Kit"). Names that
/// share nothing, or would become empty, are returned unchanged.
std::vector<std::string> shortTrackNames(const std::vector<std::string>& names);

/// Renames a track (surrounding white space removed). Clips named after the
/// track follow the new name. Returns false when the name is empty, unchanged
/// or the track does not exist.
bool renameTrack(ProjectModel& model, const TrackId& track, const std::string& name);

/// Removes a track. Media no other clip uses leaves the model with its
/// analysis records; the files stay on disk, so an undo can bring them back.
/// Returns false when the track does not exist.
bool removeTrack(ProjectModel& model, const TrackId& track);

/// Drops media no clip plays (keeping the sources of processed media that
/// clips do play) and their analysis records. Files stay on disk.
void pruneUnusedMedia(ProjectModel& model);

} // namespace daw
