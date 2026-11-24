// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// The master bus in full mixdowns, and loudness matching: the export runs the
// limiter without shifting the audio, Match Loudness reaches its target
// within 0.05 LU under the ceiling, and refuses to crush the mix for a target
// the limiter cannot reach.

#include <daw/dsp/TruePeak.h>
#include <daw/session/MasteringTools.h>
#include <daw/session/ProjectOperations.h>
#include <daw/session/Renderer.h>

#include "TestSignals.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace daw;
using namespace daw::test;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

struct Fixture {
    ProjectModel model = makeEmptyProject("mastering");
    MediaPool pool;

    Fixture() { model.sampleRate = kRate; }

    void add(const AudioAssetPtr& asset)
    {
        MediaRef ref{MediaId::generate(), "m", "x.wav", MediaStorage::Copied, asset->sampleRate, asset->numChannels(),
                     asset->numFrames(), 1};
        model.media.push_back(ref);
        addMediaAsNewTrack(model, ref, 0);
        LoadedMedia loaded;
        loaded.id = ref.id;
        loaded.original = asset;
        pool.put(loaded);
    }
};

/// A song-like mix: drum hits every half second over a sustained tone, peaking above full scale.
void addSong(Fixture& f, float hitLevel = 1.4f)
{
    std::vector<double> onsets;
    for (double t = 0.25; t < 9.5; t += 0.5)
        onsets.push_back(t);
    f.add(makeClickTrack(10.0, kRate, onsets, hitLevel, 0.0f));
    f.add(makeSine(110.0, 10.0, kRate, 2, 0.15f));
}

double truePeakDb(const AudioAsset& audio)
{
    TruePeakDetector meter;
    meter.prepare(audio.sampleRate, 2);
    const float* channels[2] = {audio.channels[0].data(), audio.channels[1].data()};
    return 20.0 * std::log10(meter.process(channels, 2, static_cast<int>(audio.numFrames())));
}

} // namespace

TEST_CASE("The export runs the master limiter without shifting the audio", "[session][master]")
{
    // A quiet mix leaves the limiter idle: on or off, the export is sample-identical.
    Fixture f;
    f.add(makeSine(440.0, 2.0, kRate, 2, 0.2f));
    const auto off = renderMix(f.model, f.pool);
    f.model.master.limiter = true;
    const auto on = renderMix(f.model, f.pool);
    REQUIRE(off.ok());
    REQUIRE(on.ok());
    REQUIRE(on->audio->numFrames() == off->audio->numFrames());
    CHECK(on->audio->channels[0] == off->audio->channels[0]);
    CHECK(on->audio->channels[1] == off->audio->channels[1]);

    // A mix that clips without it stays under the ceiling with it.
    Fixture loud;
    addSong(loud);
    REQUIRE(renderMix(loud.model, loud.pool)->report.clippedSamples > 0);
    loud.model.master.limiter = true;
    const auto limited = renderMix(loud.model, loud.pool);
    REQUIRE(limited.ok());
    CHECK(limited->report.clippedSamples == 0);
    CHECK(truePeakDb(*limited->audio) <= -1.0 + 0.02);
}

TEST_CASE("Match Loudness reaches the target through the limiter, and the export measures it", "[session][master]")
{
    Fixture f;
    addSong(f);
    const ProjectModel before = f.model;
    MasterSettings master;
    master.limiter = true;
    const auto match = matchLoudness(f.model, f.pool, -14.0, master);
    REQUIRE(match.ok());
    CHECK(f.model == before); // the project is not changed
    CHECK(match->reached);
    CHECK_FALSE(match->limited);
    CHECK(match->resultLufs == Approx(-14.0).margin(0.05));
    CHECK(match->resultTruePeakDbtp <= -1.0 + 0.02);
    CHECK(match->mixTruePeakDbtp > 0.0);
    CHECK(match->maxReductionDb < -1.0); // the hits needed limiting
    CHECK(match->maxReductionDb >= -kMaxLimiterReductionDb);

    // Applied, the exported mix measures the same.
    f.model.masterGainDb = match->masterGainDb;
    f.model.master = master;
    const auto mix = renderMix(f.model, f.pool);
    REQUIRE(mix.ok());
    CHECK(mix->report.loudness.integratedLufs == Approx(-14.0).margin(0.1));
    CHECK(truePeakDb(*mix->audio) <= -1.0 + 0.02);

    // A quiet target needs no limiting at all.
    const auto quiet = matchLoudness(f.model, f.pool, -30.0, master);
    REQUIRE(quiet.ok());
    CHECK(quiet->reached);
    CHECK(quiet->masterGainDb < 0.0);
    CHECK(quiet->maxReductionDb > -0.1);
}

TEST_CASE("Match Loudness stops short of a target that would crush the mix", "[session][master]")
{
    Fixture f;
    addSong(f, 1.4f);
    MasterSettings master;
    master.limiter = true;
    const auto match = matchLoudness(f.model, f.pool, -5.0, master);
    REQUIRE(match.ok());
    CHECK_FALSE(match->reached);
    CHECK(match->limited);
    CHECK(match->resultLufs < -5.0);
    CHECK(match->maxReductionDb >= -kMaxLimiterReductionDb - 0.05);
    CHECK(match->resultTruePeakDbtp <= -1.0 + 0.02);
}

TEST_CASE("Match Loudness refuses a silent mix", "[session][master]")
{
    Fixture f;
    f.add(makeSilence(48000, kRate));
    CHECK_FALSE(matchLoudness(f.model, f.pool, -14.0, MasterSettings{}).ok());
}
