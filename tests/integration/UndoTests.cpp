// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Undo/redo of project edits, and the track operations that use it.

#include <daw/session/ProjectOperations.h>
#include <daw/session/UndoStack.h>

#include <catch2/catch_test_macros.hpp>

using namespace daw;

namespace {

ProjectModel twoTracks()
{
    ProjectModel model = makeEmptyProject("undo");
    for (const char* name : {"Bass", "Drums"}) {
        MediaRef media{MediaId::generate(), name, std::string(name) + ".wav", MediaStorage::Copied, 48000.0, 2, 48000, 1};
        const TrackId id = addMediaAsNewTrack(model, media, 0);
        renameTrack(model, id, name);
    }
    return model;
}

/// Applies an edit the way the app does: record the old state, then change.
template <typename Edit>
void edit(UndoStack& undo, ProjectModel& model, const std::string& label, Edit change, const UndoMerge& merge = {},
          double now = 0.0)
{
    const ProjectModel before = model;
    change(model);
    if (!(model == before))
        undo.record(before, label, merge, now);
}

} // namespace

TEST_CASE("Undo and redo walk back and forth through edits", "[session][undo]")
{
    ProjectModel model = twoTracks();
    const ProjectModel original = model;
    UndoStack undo;
    CHECK_FALSE(undo.canUndo());
    CHECK_FALSE(undo.undo(model).has_value());

    edit(undo, model, "Mute", [](ProjectModel& m) { m.tracks[0].muted = true; });
    edit(undo, model, "Master", [](ProjectModel& m) { m.masterGainDb = -6.0; });
    const ProjectModel edited = model;
    CHECK(undo.undoCount() == 2);
    CHECK(undo.undoLabel() == "Master");

    model = *undo.undo(model);
    CHECK(model.masterGainDb == 0.0);
    CHECK(model.tracks[0].muted);
    CHECK(undo.redoLabel() == "Master");
    model = *undo.undo(model);
    CHECK(model == original);
    CHECK_FALSE(undo.canUndo());

    model = *undo.redo(model);
    model = *undo.redo(model);
    CHECK(model == edited);
    CHECK_FALSE(undo.canRedo());

    // A new edit after an undo drops the redo history.
    model = *undo.undo(model);
    edit(undo, model, "Pan", [](ProjectModel& m) { m.tracks[1].pan = 0.5; });
    CHECK_FALSE(undo.canRedo());
    CHECK(undo.undoLabel() == "Pan");
}

TEST_CASE("A continuous gesture merges into one step; pauses and other edits split it", "[session][undo]")
{
    ProjectModel model = twoTracks();
    UndoStack undo;
    const UndoMerge fader{"gain:" + model.tracks[0].id.str(), 1.0};
    for (int i = 1; i <= 20; ++i) // a 1 s drag, one event every 50 ms
        edit(undo, model, "Fader", [i](ProjectModel& m) { m.tracks[0].gainDb = -0.5 * i; }, fader, 0.05 * i);
    CHECK(undo.undoCount() == 1);
    CHECK(model.tracks[0].gainDb == -10.0);

    // Grabbing the fader again after a pause is a new step.
    edit(undo, model, "Fader", [](ProjectModel& m) { m.tracks[0].gainDb = -12.0; }, fader, 3.0);
    CHECK(undo.undoCount() == 2);
    // So is the same fader right after another edit.
    edit(undo, model, "Mute", [](ProjectModel& m) { m.tracks[1].muted = true; }, {}, 3.1);
    edit(undo, model, "Fader", [](ProjectModel& m) { m.tracks[0].gainDb = -13.0; }, fader, 3.2);
    CHECK(undo.undoCount() == 4);
    // And after seal() (the mouse was released).
    undo.seal();
    edit(undo, model, "Fader", [](ProjectModel& m) { m.tracks[0].gainDb = -14.0; }, fader, 3.3);
    CHECK(undo.undoCount() == 5);

    model = *undo.undo(model);
    model = *undo.undo(model);
    model = *undo.undo(model);
    model = *undo.undo(model);
    CHECK(model.tracks[0].gainDb == -10.0); // the whole first drag is one step
    model = *undo.undo(model);
    CHECK(model.tracks[0].gainDb == 0.0);

    // A batch import merges for as long as it takes.
    UndoStack imports;
    const UndoMerge batch{"import:1", UndoMerge::kForever};
    for (int i = 0; i < 5; ++i)
        edit(imports, model, "Import", [i](ProjectModel& m) { m.tempo.bpm = 100.0 + i; }, batch, 60.0 * i);
    CHECK(imports.undoCount() == 1);
}

TEST_CASE("Undo keeps the playhead and the analyses of media that is still there", "[session][undo]")
{
    ProjectModel model = twoTracks();
    UndoStack undo;
    const MediaId bassMedia = model.tracks[0].clips[0].media;
    const TrackId bass = model.tracks[0].id;

    edit(undo, model, "Delete Track", [&](ProjectModel& m) { removeTrack(m, bass); });
    // Meanwhile: the playhead moves and the drums get analysed.
    model.playhead = 4800;
    model.analyses.push_back({AnalysisId::generate(), model.tracks[0].clips[0].media, "daw.transients", 1, "a.json", "now"});

    model = *undo.undo(model);
    REQUIRE(model.tracks.size() == 2);
    CHECK(model.tracks[0].id == bass);
    CHECK(model.findMedia(bassMedia) != nullptr);
    CHECK(model.playhead == 4800);
    REQUIRE(model.analyses.size() == 1); // the drums' analysis survives the undo

    model = *undo.redo(model);
    CHECK(model.tracks.size() == 1);
    CHECK(model.analyses.size() == 1);
}

TEST_CASE("The undo history is limited", "[session][undo]")
{
    ProjectModel model = twoTracks();
    UndoStack undo(5);
    for (int i = 1; i <= 8; ++i)
        edit(undo, model, "Master", [i](ProjectModel& m) { m.masterGainDb = -i; });
    CHECK(undo.undoCount() == 5);
    while (undo.canUndo())
        model = *undo.undo(model);
    CHECK(model.masterGainDb == -3.0); // the three oldest steps were dropped
    undo.clear();
    CHECK_FALSE(undo.canRedo());
}

TEST_CASE("Renaming and removing tracks", "[session][tracks]")
{
    ProjectModel model = twoTracks();
    const TrackId bass = model.tracks[0].id;

    CHECK(renameTrack(model, bass, "  Sub Bass \t"));
    CHECK(model.tracks[0].name == "Sub Bass");
    CHECK(model.tracks[0].clips[0].name == "Sub Bass"); // clips named after the track follow
    model.tracks[0].clips[0].name = "take 2";
    CHECK(renameTrack(model, bass, "Bass"));
    CHECK(model.tracks[0].clips[0].name == "take 2"); // other clip names are left alone
    CHECK_FALSE(renameTrack(model, bass, "Bass"));
    CHECK_FALSE(renameTrack(model, bass, "   "));
    CHECK_FALSE(renameTrack(model, TrackId("missing"), "x"));

    // Media shared by two tracks stays until its last clip goes.
    ProjectModel shared = model;
    shared.tracks[1].clips[0].media = shared.tracks[0].clips[0].media;
    std::erase_if(shared.media, [&](const MediaRef& m) { return m.id != shared.tracks[0].clips[0].media; });
    shared.analyses.push_back({AnalysisId::generate(), shared.media[0].id, "daw.transients", 1, "a.json", "now"});
    CHECK(removeTrack(shared, shared.tracks[0].id));
    CHECK(shared.media.size() == 1);
    CHECK(shared.analyses.size() == 1);
    CHECK(removeTrack(shared, shared.tracks[0].id));
    CHECK(shared.tracks.empty());
    CHECK(shared.media.empty());
    CHECK(shared.analyses.empty());
    CHECK_FALSE(removeTrack(shared, bass));
}
