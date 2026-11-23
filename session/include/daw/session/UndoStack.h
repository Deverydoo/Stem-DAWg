// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Undo and redo for project edits.
//
// The project model is a small value type (no audio: media lives in files and
// in the MediaPool), so each step stores the whole model as it was before the
// edit. Restoring it cannot drift out of sync with the edit the way
// hand-written inverse commands can, and every new kind of edit is undoable
// for free.
//
// Continuous gestures merge into one step: edits with the same merge key that
// follow each other within the merge window (a fader drag, a batch import)
// keep the state from before the first one.
//
// Not undone: the playhead (transport state, not an edit) and analysis results
// (they describe media, whichever edit is current).

#include <daw/project/ProjectModel.h>

#include <cstddef>
#include <deque>
#include <limits>
#include <optional>
#include <string>

namespace daw {

struct UndoMerge {
    std::string key;               // empty: never merges
    double windowSeconds = 1.0;    // merges while the previous edit with this key is at most this old
    static constexpr double kForever = std::numeric_limits<double>::infinity();
};

class UndoStack {
public:
    explicit UndoStack(std::size_t limit = 200) : limit_(limit) {}

    /// Records an edit. Call with the model as it was BEFORE the edit, and only
    /// when the edit changed something. Clears the redo history.
    void record(const ProjectModel& before, std::string label, const UndoMerge& merge = {}, double nowSeconds = 0.0);
    /// Ends merging: the next edit starts a new step even with the same key.
    void seal() noexcept { sealed_ = true; }

    bool canUndo() const noexcept { return !undo_.empty(); }
    bool canRedo() const noexcept { return !redo_.empty(); }
    /// Label of the step undo/redo would apply ("Fader", "Import 9 files"...).
    const std::string& undoLabel() const noexcept;
    const std::string& redoLabel() const noexcept;
    std::size_t undoCount() const noexcept { return undo_.size(); }
    std::size_t redoCount() const noexcept { return redo_.size(); }

    /// Steps back. Returns the model to adopt (see restoreUndoState), or
    /// nothing when there is no step.
    std::optional<ProjectModel> undo(const ProjectModel& current);
    std::optional<ProjectModel> redo(const ProjectModel& current);

    void clear() noexcept;

private:
    struct Step {
        ProjectModel model;
        std::string label;
        std::string mergeKey;
        double time = 0.0;
    };

    std::size_t limit_;
    std::deque<Step> undo_;
    std::deque<Step> redo_;
    bool sealed_ = true;
};

/// The model to adopt when going back to `target` from `current`: `target`,
/// but with the current playhead, and with analysis records kept for every
/// media item `target` has.
ProjectModel restoreUndoState(const ProjectModel& target, const ProjectModel& current);

} // namespace daw
