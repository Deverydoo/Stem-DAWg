// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <daw/project/ProjectModel.h>

namespace daw::test {

/// A project exercising every field of schema v1.
inline ProjectModel makeRichProject()
{
    ProjectModel model = makeEmptyProject("Rust Belt Liturgy");
    model.appVersion = "0.1.0";
    model.sampleRate = 44100.0;
    model.tempo = {133.5, 7, 8};
    model.masterGainDb = -1.5;
    model.playhead = 123456789;

    MediaRef drums;
    drums.id = MediaId::generate();
    drums.name = "suno_drums";
    drums.path = "Media/Original/suno_drums.wav";
    drums.storage = MediaStorage::Copied;
    drums.sampleRate = 44100.0;
    drums.numChannels = 2;
    drums.numFrames = 9'000'000'000LL; // > 32-bit
    drums.sourceKey = 0xFEDCBA9876543210ull;

    MediaRef guitar = drums;
    guitar.id = MediaId::generate();
    guitar.name = "guitar ✓";
    guitar.path = "D:/Stems/Ünïcødé/guitar.flac";
    guitar.storage = MediaStorage::Referenced;
    guitar.sampleRate = 48000.0;
    guitar.numChannels = 1;
    guitar.numFrames = 480000;
    model.media = {drums, guitar};

    Track t1;
    t1.id = TrackId::generate();
    t1.name = "Drums (original)";
    t1.gainDb = -3.0;
    t1.pan = -0.35;
    t1.muted = true;
    t1.clips.push_back(Clip{ClipId::generate(), "drums A", drums.id, 0, 0, 441000, 0.0});
    t1.clips.push_back(Clip{ClipId::generate(), "drums B", drums.id, 882000, 44100, 220500, -6.0});

    Track t2;
    t2.id = TrackId::generate();
    t2.name = "Guitar";
    t2.pan = 1.0;
    t2.soloed = true;
    t2.clips.push_back(Clip{ClipId::generate(), "gtr", guitar.id, 1000, 0, 441000, 1.25});
    model.tracks = {t1, t2};

    model.analyses.push_back(AnalysisRecord{AnalysisId::generate(), drums.id, "daw.transients", 1,
                                            "Analysis/" + drums.id.str() + "/daw.transients-v1.json", "2026-11-21T10:00:00.000Z"});
    return model;
}

} // namespace daw::test
