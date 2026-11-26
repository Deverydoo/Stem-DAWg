// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Export presets per destination: each file has the destination's rate and
// format, measures its loudness target under its ceiling, and the project
// is left as it was.

#include <daw/dsp/LoudnessMeter.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/session/Destinations.h>
#include <daw/session/ProjectOperations.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace daw;
using namespace daw::test;
using Catch::Approx;

namespace {

const Destination& destination(std::string_view key)
{
    for (const Destination& d : destinations())
        if (key == d.key)
            return d;
    throw std::logic_error("no such destination");
}

struct Fixture {
    ProjectModel model = makeEmptyProject("destinations");
    MediaPool pool;

    explicit Fixture(double rate)
    {
        model.sampleRate = rate;
        // A song-like mix: hits every half second over a sustained tone, peaking above full scale.
        std::vector<double> onsets;
        for (double t = 0.25; t < 9.5; t += 0.5)
            onsets.push_back(t);
        add(makeClickTrack(10.0, rate, onsets, 1.4f, 0.0f));
        add(makeSine(110.0, 10.0, rate, 2, 0.15f));
    }

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

LoudnessReading measure(const AudioAsset& audio)
{
    LoudnessMeter meter;
    meter.prepare(audio.sampleRate, 2);
    const float* channels[2] = {audio.channels[0].data(), audio.channels[1].data()};
    meter.process(channels, 2, static_cast<int>(audio.numFrames()));
    return meter.reading();
}

/// Every sample on the 16-bit grid.
bool sixteenBit(const AudioAsset& audio)
{
    for (const auto& channel : audio.channels)
        for (float x : channel) {
            const double steps = static_cast<double>(x) * 32768.0;
            if (std::abs(steps - std::round(steps)) > 1e-3)
                return false;
        }
    return true;
}

} // namespace

TEST_CASE("Destinations choose the file's rate and name", "[session][destinations]")
{
    CHECK(destinationSampleRate(destination("Streaming"), 48000.0) == 48000.0);
    CHECK(destinationSampleRate(destination("Bandcamp"), 96000.0) == 96000.0);
    CHECK(destinationSampleRate(destination("Club"), 48000.0) == 48000.0);
    CHECK(destinationSampleRate(destination("Club"), 44100.0) == 44100.0);
    CHECK(destinationSampleRate(destination("Club"), 96000.0) == 48000.0);
    CHECK(destinationSampleRate(destination("Club"), 88200.0) == 44100.0);
    CHECK(destinationSampleRate(destination("CD"), 48000.0) == 44100.0);
    CHECK(destinationSampleRate(destination("CD"), 44100.0) == 44100.0);

    CHECK(destinationPath(std::filesystem::path("out") / "A - Song.wav", destination("CD"))
          == std::filesystem::path("out") / "A - Song (CD).wav");
    CHECK(destinationPath("Song.wav", destination("Club")) == std::filesystem::path("Song (Club).wav"));
}

TEST_CASE("Each destination's export has its rate, format and loudness, and leaves the project alone",
          "[session][destinations]")
{
    TempDir dir("destinations");
    Fixture f(48000.0);
    f.model.master.eq = false;
    const ProjectModel before = f.model;

    for (const Destination& d : destinations()) {
        INFO(d.key);
        ExportRequest request;
        request.path = destinationPath(dir / "Song.wav", d);
        request.format = WavSampleFormat::Float32; // the destination's format wins
        request.software = "Stem DAWg test";
        double last = 0.0;
        bool monotonic = true;
        RenderOptions options;
        options.progress = [&](double p) {
            monotonic = monotonic && p >= last - 1e-9;
            last = p;
        };

        const auto report = exportForDestination(f.model, f.pool, d, request, options);
        REQUIRE(report.ok());
        CHECK(f.model == before);
        CHECK(monotonic);
        CHECK(last == Approx(1.0));

        const double rate = destinationSampleRate(d, f.model.sampleRate);
        const auto file = decodeAudioFile(request.path);
        REQUIRE(file.ok());
        const AudioAsset& audio = **file;
        CHECK(audio.sampleRate == rate);
        CHECK(std::abs(static_cast<double>(audio.numFrames()) - 10.0 * rate) <= 2.0);
        CHECK(sixteenBit(audio) == (d.format == WavSampleFormat::Pcm16));

        // The file measures what the match promised: the target when it is
        // within reach, under the destination's ceiling either way.
        const LoudnessReading reading = measure(audio);
        CHECK(reading.integratedLufs == Approx(report->match.resultLufs).margin(0.1));
        CHECK(reading.truePeakDbtp <= d.ceilingDbtp + 0.05);
        if (report->match.reached)
            CHECK(reading.integratedLufs == Approx(d.lufs).margin(0.1));
        else
            CHECK(report->match.resultLufs < d.lufs);
        CHECK(report->file.render.sampleRate == rate);
    }
}

TEST_CASE("A destination's master keeps the mix and sets the master bus", "[session][destinations]")
{
    Fixture f(48000.0);
    f.model.master.eq = true;
    f.model.master.eqGainDb[2] = 2.0;
    f.model.master.multiband = true;
    f.model.master.clipper = true;
    LoudnessMatch match;
    match.masterGainDb = 3.5;
    match.clipper = false;
    const ProjectModel mastered = masteredFor(f.model, destination("CD"), match);
    CHECK(mastered.masterGainDb == 3.5);
    CHECK(mastered.master.limiter);
    CHECK(mastered.master.ceilingDbtp == -0.3);
    CHECK(mastered.master.targetLufs == -10.0);
    CHECK_FALSE(mastered.master.clipper); // as the match decided
    CHECK(mastered.master.eq);
    CHECK(mastered.master.eqGainDb[2] == 2.0);
    CHECK(mastered.master.multiband);
    CHECK(mastered.tracks == f.model.tracks);
}
