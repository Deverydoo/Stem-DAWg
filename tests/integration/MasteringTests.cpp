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

#include "TempDir.h"
#include "TestSignals.h"

#include <daw/media/WavWriter.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>

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

TEST_CASE("Match Loudness adds the clipper only when the limiter alone falls short, and stops at both budgets",
          "[session][master]")
{
    Fixture f;
    addSong(f, 1.4f);
    MasterSettings master;
    master.limiter = true;

    // Within the limiter's reach: no clipper.
    const auto easy = matchLoudness(f.model, f.pool, -14.0, master);
    REQUIRE(easy.ok());
    CHECK(easy->reached);
    CHECK_FALSE(easy->clipper);
    CHECK(easy->clippedShare == 0.0);

    // Far beyond it, on a denser mix (a noise bed under the hits, so the
    // budgets bind before the fader runs out): the clipper joins, and the
    // match stops at the budgets.
    f.add(makeNoise(static_cast<std::int64_t>(10.0 * kRate), kRate, 2, 0.25f, 7));
    const auto hard = matchLoudness(f.model, f.pool, -5.0, master);
    REQUIRE(hard.ok());
    CHECK_FALSE(hard->reached);
    CHECK(hard->limited);
    CHECK(hard->clipper);
    CHECK(hard->resultLufs < -5.0);
    CHECK(hard->clippedShare <= kMaxClippedShare + 0.001);
    CHECK(hard->maxReductionDb >= -kMaxLimiterReductionDb - 0.05);
    CHECK(hard->resultTruePeakDbtp <= -1.0 + 0.02);

    // Applied, the settings carry the clipper, and the export measures what the match promised.
    f.model.masterGainDb = hard->masterGainDb;
    f.model.master = applyLoudnessMatch(master, *hard, -5.0);
    CHECK(f.model.master.clipper);
    const auto mix = renderMix(f.model, f.pool);
    REQUIRE(mix.ok());
    CHECK(mix->report.loudness.integratedLufs == Approx(hard->resultLufs).margin(0.1));
    CHECK(truePeakDb(*mix->audio) <= -1.0 + 0.02);
}

TEST_CASE("Match Loudness refuses a silent mix", "[session][master]")
{
    Fixture f;
    f.add(makeSilence(48000, kRate));
    CHECK_FALSE(matchLoudness(f.model, f.pool, -14.0, MasterSettings{}).ok());
}

TEST_CASE("Tonal targets come from reference files and the mix is profiled as heard", "[session][master][tonal]")
{
    TempDir dir;
    const auto bright = dir.path() / "bright.wav";
    const auto dark = dir.path() / "dark.wav";
    const auto broken = dir.path() / "broken.wav";
    REQUIRE(writeWavFile(bright, *makeNoise(96000, kRate, 2, 0.3f, 1)).ok());   // white: lots of top end
    REQUIRE(writeWavFile(dark, *makeSine(100.0, 2.0, kRate, 2, 0.5f)).ok());   // all low end
    { std::ofstream(broken) << "not audio"; }

    std::vector<std::string> skipped;
    const auto target = tonalTargetFromFiles("refs", {bright, dark, broken}, &skipped);
    REQUIRE(target.ok());
    CHECK(target->references == std::vector<std::string>{"bright.wav", "dark.wav"});
    CHECK(skipped == std::vector<std::string>{"broken.wav"});
    CHECK_FALSE(tonalTargetFromFiles("none", {broken}).ok());

    // Stored in a project and read back, it compares the same way.
    const TonalTargetSettings stored = toSettings(*target);
    CHECK(stored.meanDb.size() == kTonalBands);
    CHECK(fromSettings(stored) == *target);

    // The mix as heard: the master EQ is in it.
    Fixture f;
    f.add(makeNoise(96000, kRate, 2, 0.2f, 5));
    const auto plain = tonalProfileOfMix(f.model, f.pool);
    f.model.master.eq = true;
    f.model.master.eqGainDb = {0.0, 0.0, 0.0, 0.0, -6.0};
    const auto darker = tonalProfileOfMix(f.model, f.pool);
    REQUIRE(plain.ok());
    REQUIRE(darker.ok());
    const std::size_t top = kTonalBands - 1, mid = 15; // 16 kHz, 1 kHz
    CHECK((darker->levelDb[top] - darker->levelDb[mid]) - (plain->levelDb[top] - plain->levelDb[mid]) == Approx(-6.0).margin(0.5));
}
