// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/Files.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/session/ProjectOperations.h>
#include <daw/session/Renderer.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace daw;
using namespace daw::test;

namespace {

struct Fixture {
    ProjectModel model = makeEmptyProject("render");
    MediaPool pool;

    MediaId add(const AudioAssetPtr& asset, std::int64_t timelineStart = 0)
    {
        MediaRef ref{MediaId::generate(), "m", "x.wav", MediaStorage::Copied, asset->sampleRate, asset->numChannels(),
                     asset->numFrames(), 1};
        model.media.push_back(ref);
        addMediaAsNewTrack(model, ref, timelineStart);
        LoadedMedia loaded;
        loaded.id = ref.id;
        loaded.original = asset;
        pool.put(loaded);
        return ref.id;
    }
};

} // namespace

TEST_CASE("Render sums tracks with fader, pan, mute and master exactly like playback", "[session][render]")
{
    Fixture f;
    f.model.sampleRate = 48000.0;
    const auto a = makeSine(220.0, 1.0, 48000.0, 2, 0.25f);
    const auto b = makeRamp(24000, 48000.0);
    f.add(a);
    f.add(b, 12000);
    f.model.tracks[1].pan = 1.0;     // hard right
    f.model.masterGainDb = -6.0;

    auto mix = renderMix(f.model, f.pool);
    REQUIRE(mix.ok());
    REQUIRE(mix->audio->numFrames() == 48000);
    REQUIRE(mix->audio->sampleRate == 48000.0);
    const float master = static_cast<float>(dbToGain(-6.0));
    for (std::size_t i = 0; i < 48000; i += 97) {
        const bool inB = i >= 12000 && i < 36000;
        const float bl = 0.0f;
        const float br = inB ? b->channels[1][i - 12000] : 0.0f;
        REQUIRE(std::abs(mix->audio->channels[0][i] - (a->channels[0][i] + bl) * master) < 1e-6f);
        REQUIRE(std::abs(mix->audio->channels[1][i] - (a->channels[1][i] + br) * master) < 1e-6f);
    }
    REQUIRE(mix->report.clippedSamples == 0);

    f.model.tracks[0].muted = true;
    mix = renderMix(f.model, f.pool);
    REQUIRE(mix->audio->channels[0][100] == 0.0f);
}

TEST_CASE("Render reports clipping when stems sum above full scale", "[session][render]")
{
    Fixture f;
    f.model.sampleRate = 48000.0;
    for (int i = 0; i < 3; ++i)
        f.add(makeSine(100.0, 0.5, 48000.0, 2, 0.6f));
    const auto mix = renderMix(f.model, f.pool);
    REQUIRE(mix.ok());
    REQUIRE(mix->report.peakDbfs > 0.0);
    REQUIRE(mix->report.clippedSamples > 0);
}

TEST_CASE("Render runs at the project rate and resamples other-rate media", "[session][render]")
{
    Fixture f;
    f.model.sampleRate = 48000.0;
    f.add(makeSine(1000.0, 1.0, 44100.0, 2, 0.5f)); // project frames: 48000
    f.model.tracks[0].clips[0].length = 48000;
    const auto mix = renderMix(f.model, f.pool);
    REQUIRE(mix.ok());
    REQUIRE(mix->audio->sampleRate == 48000.0);
    const auto& left = mix->audio->channels[0];
    const double tone = goertzelPower(left.data() + 4800, 38400, 1000.0, 48000.0);
    const double off = goertzelPower(left.data() + 4800, 38400, 1088.0, 48000.0); // where a 44.1 kHz mistake would land
    REQUIRE(tone > off * 1000.0);
}

TEST_CASE("Export writes a tagged WAV whose audio matches the render", "[session][render][tags]")
{
    TempDir dir("export");
    Fixture f;
    f.model.sampleRate = 48000.0;
    f.add(makeSine(330.0, 0.5, 48000.0, 2, 0.3f));
    const std::string png = std::string("\x89PNG\r\n\x1A\n", 8) + "pixels";
    REQUIRE(writeFileAtomically(dir / "cover.png", png).ok());

    ArtistProfile profile = makeProfile("Example Artist");
    profile.fields.artworkPath = pathToUtf8(dir / "cover.png");
    MetadataFields song;
    song.title = "Test Song";

    ExportRequest request;
    request.path = dir / "Test Song.wav";
    request.format = WavSampleFormat::Float32;
    request.metadata = resolveMetadata(song, &profile, "render", "2026");
    request.software = "Stem DAWg test";
    double lastProgress = 0.0;
    RenderOptions options;
    options.progress = [&](double p) { lastProgress = p; };

    const auto report = exportMix(f.model, f.pool, request, options);
    REQUIRE(report.ok());
    REQUIRE(report->artworkEmbedded);
    REQUIRE(report->artworkWarning.empty());
    REQUIRE(lastProgress == 1.0);

    const auto bytes = readFileBytes(request.path);
    REQUIRE(bytes.ok());
    REQUIRE(report->bytes == bytes->size());
    const auto tags = readWavTags(*bytes);
    REQUIRE(tags->title == "Test Song");
    REQUIRE(tags->artist == "Example Artist");
    REQUIRE(tags->copyright == "\xC2\xA9 2026 Example Artist");
    REQUIRE(tags->software == "Stem DAWg test");
    REQUIRE(tags->artwork == png);

    const auto decoded = decodeAudioFile(request.path);
    const auto rendered = renderMix(f.model, f.pool);
    REQUIRE(decoded.ok());
    REQUIRE((*decoded)->channels == rendered->audio->channels);

    // Missing artwork is a warning, not a failure.
    request.metadata.artworkPath = pathToUtf8(dir / "missing.png");
    const auto warned = exportMix(f.model, f.pool, request);
    REQUIRE(warned.ok());
    REQUIRE_FALSE(warned->artworkEmbedded);
    REQUIRE_FALSE(warned->artworkWarning.empty());
}

TEST_CASE("Render refuses projects with unloaded media or no audio", "[session][render]")
{
    Fixture f;
    REQUIRE(renderMix(f.model, f.pool).error().code == ErrorCode::InvalidArgument);
    MediaRef ref{MediaId::generate(), "late", "x.wav", MediaStorage::Copied, 48000.0, 2, 4800, 1};
    f.model.media.push_back(ref);
    addMediaAsNewTrack(f.model, ref);
    REQUIRE(renderMix(f.model, f.pool).error().code == ErrorCode::NotFound);
}
