// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/UndoStack.h>

#include <algorithm>

namespace daw {

namespace {
const std::string kNoLabel;
}

void UndoStack::record(const ProjectModel& before, std::string label, const UndoMerge& merge, double nowSeconds)
{
    redo_.clear();
    if (!sealed_ && !merge.key.empty() && !undo_.empty()) {
        Step& top = undo_.back();
        if (top.mergeKey == merge.key && nowSeconds - top.time <= merge.windowSeconds) {
            top.time = nowSeconds; // keep the state from before the first edit
            return;
        }
    }
    undo_.push_back({before, std::move(label), merge.key, nowSeconds});
    while (undo_.size() > limit_)
        undo_.pop_front();
    sealed_ = merge.key.empty();
}

const std::string& UndoStack::undoLabel() const noexcept
{
    return undo_.empty() ? kNoLabel : undo_.back().label;
}

const std::string& UndoStack::redoLabel() const noexcept
{
    return redo_.empty() ? kNoLabel : redo_.back().label;
}

std::optional<ProjectModel> UndoStack::undo(const ProjectModel& current)
{
    if (undo_.empty())
        return std::nullopt;
    Step step = std::move(undo_.back());
    undo_.pop_back();
    ProjectModel restored = restoreUndoState(step.model, current);
    redo_.push_back({current, step.label, {}, 0.0});
    sealed_ = true;
    return restored;
}

std::optional<ProjectModel> UndoStack::redo(const ProjectModel& current)
{
    if (redo_.empty())
        return std::nullopt;
    Step step = std::move(redo_.back());
    redo_.pop_back();
    ProjectModel restored = restoreUndoState(step.model, current);
    undo_.push_back({current, step.label, {}, 0.0});
    sealed_ = true;
    return restored;
}

void UndoStack::clear() noexcept
{
    undo_.clear();
    redo_.clear();
    sealed_ = true;
}

ProjectModel restoreUndoState(const ProjectModel& target, const ProjectModel& current)
{
    ProjectModel restored = target;
    restored.playhead = current.playhead;
    restored.modifiedUtc = current.modifiedUtc;
    // Analyses describe media, not edits: keep every record whose media is in
    // the restored model, from either state (the current one wins when both
    // have a result of the same analyzer for the same media).
    const auto hasMedia = [&](const MediaId& id) { return restored.findMedia(id) != nullptr; };
    std::vector<AnalysisRecord> analyses;
    for (const auto* list : {&current.analyses, &target.analyses})
        for (const AnalysisRecord& a : *list)
            if (hasMedia(a.media)
                && std::none_of(analyses.begin(), analyses.end(), [&](const AnalysisRecord& b) {
                       return b.media == a.media && b.analyzer == a.analyzer && b.analyzerVersion == a.analyzerVersion;
                   }))
                analyses.push_back(a);
    restored.analyses = std::move(analyses);
    return restored;
}

} // namespace daw
