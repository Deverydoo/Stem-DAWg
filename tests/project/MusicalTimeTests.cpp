// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/project/MusicalTime.h>
#include <daw/project/ProjectSerializer.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace daw;
using Catch::Approx;

namespace {

/// Beats every 0.5 s from 1.0 s, speeding to 0.4 s apart after beat 8,
/// with a one-beat pickup: bar lines at beats 1, 5, 9 (4/4).
TempoMap trackedTempo()
{
    TempoMap tempo;
    std::vector<double> beats;
    double t = 1.0;
    for (int i = 0; i < 13; ++i) {
        beats.push_back(t);
        t += i < 8 ? 0.5 : 0.4;
    }
    setTrackedBeats(tempo, beats, {1, 5, 9}, 4, "Drums");
    return tempo;
}

} // namespace

TEST_CASE("A steady tempo counts bars from time 0", "[project][tempo]")
{
    TempoMap tempo; // 120 BPM, 4/4
    REQUIRE_FALSE(followsTrackedBeats(tempo));
    auto p = musicalPosition(tempo, 0.0);
    REQUIRE((p.bar == 1 && p.beat == 1));
    p = musicalPosition(tempo, 2.25); // beat 4.5
    REQUIRE((p.bar == 2 && p.beat == 1));
    REQUIRE(p.fraction == Approx(0.5));
    p = musicalPosition(tempo, 1.6);
    REQUIRE((p.bar == 1 && p.beat == 4));

    const auto marks = beatMarks(tempo, 0.1, 4.0);
    REQUIRE(marks.size() == 8); // beats 1..8 at 0.5 s .. 4.0 s
    REQUIRE(marks.front().seconds == Approx(0.5));
    REQUIRE(marks[3].bar == 2);
    REQUIRE(marks[3].beat == 1);
}

TEST_CASE("Tracked beats set the median tempo and bar length", "[project][tempo]")
{
    const TempoMap tempo = trackedTempo();
    REQUIRE(followsTrackedBeats(tempo));
    REQUIRE(tempo.bpm == Approx(120.0));
    REQUIRE(tempo.numerator == 4);
    REQUIRE(tempo.beatsSource == "Drums");

    TempoMap cleared = tempo;
    setTrackedBeats(cleared, {2.0}, {}, 4, "Drums");
    REQUIRE_FALSE(followsTrackedBeats(cleared));
    REQUIRE(cleared.beatsSource.empty());
    REQUIRE(cleared.bpm == Approx(120.0)); // the steady tempo is kept
}

TEST_CASE("Bars and beats follow the tracked beats", "[project][tempo]")
{
    const TempoMap tempo = trackedTempo();
    // Beat index: exact on the beats, interpolated between, extrapolated
    // outside at the neighbouring spacing.
    REQUIRE(beatIndexAt(tempo, 1.0) == Approx(0.0));
    REQUIRE(beatIndexAt(tempo, 1.25) == Approx(0.5));
    REQUIRE(beatIndexAt(tempo, 0.0) == Approx(-2.0));
    REQUIRE(beatIndexAt(tempo, 5.0) == Approx(8.0));
    REQUIRE(beatIndexAt(tempo, 5.2) == Approx(8.5));
    REQUIRE(beatIndexAt(tempo, 6.6 + 0.8) == Approx(14.0)); // two past the last beat (12 at 6.6 s)
    for (double index : {-3.0, -0.25, 0.0, 3.7, 8.5, 12.0, 15.25})
        REQUIRE(beatIndexAt(tempo, secondsAtBeat(tempo, index)) == Approx(index));

    // Time 0 is beat -2, the second beat of the bar before the first bar
    // line (beat 1), so that bar line is bar 2.
    auto p = musicalPosition(tempo, 0.0);
    REQUIRE((p.bar == 1 && p.beat == 2));
    p = musicalPosition(tempo, 1.0); // beat 0, the pickup
    REQUIRE((p.bar == 1 && p.beat == 4));
    p = musicalPosition(tempo, 1.5); // beat 1, the first bar line
    REQUIRE((p.bar == 2 && p.beat == 1));
    p = musicalPosition(tempo, 4.6); // beat 7.2
    REQUIRE((p.bar == 3 && p.beat == 3));
    REQUIRE(p.fraction == Approx(0.2));
    p = musicalPosition(tempo, 5.4); // beat 9, the last bar line
    REQUIRE((p.bar == 4 && p.beat == 1));
    p = musicalPosition(tempo, 7.4); // beat 14: past the end, bars run on in 4s
    REQUIRE((p.bar == 5 && p.beat == 2));

    const auto marks = beatMarks(tempo, 1.4, 5.5);
    REQUIRE(marks.size() == 9); // beats 1..9
    REQUIRE(marks.front().seconds == Approx(1.5));
    REQUIRE((marks.front().bar == 2 && marks.front().beat == 1));
    REQUIRE((marks.back().bar == 4 && marks.back().beat == 1));
    REQUIRE(marks.back().seconds == Approx(5.4));
}

TEST_CASE("A bar tracked a beat long keeps its extra beat", "[project][tempo]")
{
    TempoMap tempo;
    std::vector<double> beats;
    for (int i = 0; i < 12; ++i)
        beats.push_back(0.5 * i);
    setTrackedBeats(tempo, beats, {0, 5, 9}, 4, "Drums");
    REQUIRE(musicalPosition(tempo, 2.0).beat == 5); // beat 4: the fifth of bar 1
    REQUIRE(musicalPosition(tempo, 2.0).bar == 1);
    REQUIRE(musicalPosition(tempo, 2.5).bar == 2);
    REQUIRE(musicalPosition(tempo, 4.5).bar == 3);
}

TEST_CASE("Projects keep and validate their tracked beats", "[project][tempo][serialize]")
{
    ProjectModel model = makeEmptyProject("Beats");
    model.tempo = trackedTempo();
    const auto parsed = parseProject(serializeProject(model));
    REQUIRE(parsed.ok());
    REQUIRE(parsed->tempo.beats.size() == 13);
    REQUIRE(parsed->tempo.beats[9] == Approx(5.4).margin(1e-5));
    REQUIRE(parsed->tempo.downbeats == model.tempo.downbeats);
    REQUIRE(parsed->tempo.beatsSource == "Drums");

    auto doc = projectToJson(model);
    doc["timeline"]["tempo"]["beats"][3] = 0.1; // out of order
    REQUIRE(projectFromJson(doc).error().code == ErrorCode::Corrupt);
    doc = projectToJson(model);
    doc["timeline"]["tempo"]["downbeats"] = {1, 40};
    REQUIRE(projectFromJson(doc).error().code == ErrorCode::Corrupt);

    // A project from before tracked beats loads with a steady tempo.
    doc = projectToJson(makeEmptyProject("Old"));
    REQUIRE_FALSE(doc["timeline"]["tempo"].contains("beats"));
    REQUIRE(projectFromJson(doc)->tempo.beats.empty());
}
