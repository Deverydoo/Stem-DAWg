// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/Analyzers.h>
#include <daw/core/Files.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/PeakCache.h>
#include <daw/media/WavWriter.h>
#include <daw/project/ProjectStore.h>
#include <daw/session/MediaLoader.h>
#include <daw/session/ProjectOperations.h>
#include <daw/session/SnapshotBuilder.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

using namespace daw;
using namespace daw::test;
namespace fs = std::filesystem;

TEST_CASE("Importing a file stages it and describes it", "[session][import]")
{
    TempDir dir("import");
    const auto source = dir / "suno_drums.wav";
    REQUIRE(writeWavFile(source, *makeRamp(44100, 44100.0)).ok());
    const fs::path root = dir / "Project";
    REQUIRE(ProjectStore::createLayout(root).ok());

    const auto media = importAudioFile(root, source);
    REQUIRE(media.ok());
    REQUIRE(media->path == "Media/Original/suno_drums.wav");
    REQUIRE(media->numFrames == 44100);
    REQUIRE(media->numChannels == 2);
    REQUIRE(media->sampleRate == 44100.0);
    REQUIRE(media->sourceKey != 0);
    REQUIRE(fs::is_regular_file(root / "Media" / "Original" / "suno_drums.wav"));

    REQUIRE(importAudioFile(root, dir / "missing.wav").error().code == ErrorCode::NotFound);
}

TEST_CASE("First import sets the project rate and later imports are length-converted", "[session][import]")
{
    ProjectModel model = makeEmptyProject("rates");
    MediaRef a{MediaId::generate(), "a", "Media/Original/a.wav", MediaStorage::Copied, 44100.0, 2, 44100, 1};
    MediaRef b{MediaId::generate(), "b", "Media/Original/b.wav", MediaStorage::Copied, 48000.0, 2, 48000, 2};
    addMediaAsNewTrack(model, a);
    REQUIRE(model.sampleRate == 44100.0);
    addMediaAsNewTrack(model, b, 1000);
    REQUIRE(model.tracks.size() == 2);
    REQUIRE(model.tracks[1].clips[0].length == 44100); // 1 s at the project rate
    REQUIRE(model.tracks[1].clips[0].timelineStart == 1000);
    REQUIRE(model.contentEnd() == 45100);
}

TEST_CASE("Media loading caches peaks and creates engine-rate renditions", "[session][media]")
{
    TempDir dir("load");
    const fs::path root = dir.path();
    REQUIRE(ProjectStore::createLayout(root).ok());
    const auto source = dir / "tone.wav";
    REQUIRE(writeWavFile(source, *makeSine(440.0, 1.0, 44100.0), WavSampleFormat::Float32).ok());
    const auto media = importAudioFile(root, source);
    REQUIRE(media.ok());

    MediaLoadOptions options;
    options.engineSampleRate = 48000.0;
    const auto first = loadMedia(root, *media, options);
    REQUIRE(first.ok());
    REQUIRE_FALSE(first->peaksFromCache);
    REQUIRE(first->original->sampleRate == 44100.0);
    REQUIRE(first->rendition->sampleRate == 48000.0);
    REQUIRE(first->rendition->numFrames() == 48000);
    REQUIRE(fs::is_regular_file(ProjectStore::peakCachePath(root, media->id)));

    const auto second = loadMedia(root, *media, options);
    REQUIRE(second.ok());
    REQUIRE(second->peaksFromCache);
    REQUIRE(*second->peaks == *first->peaks);

    // Corrupt cache -> transparently rebuilt.
    REQUIRE(writeFileAtomically(ProjectStore::peakCachePath(root, media->id), "garbage").ok());
    const auto third = loadMedia(root, *media, options);
    REQUIRE(third.ok());
    REQUIRE_FALSE(third->peaksFromCache);
    REQUIRE(*third->peaks == *first->peaks);

    // Same-rate engine shares the decoded original.
    const auto native = retargetMedia(*first, 44100.0);
    REQUIRE(native->rendition == native->original);
}

TEST_CASE("Snapshot builder maps project time to engine time and reports missing media", "[session][snapshot]")
{
    ProjectModel model = makeEmptyProject("map");
    model.sampleRate = 44100.0;
    MediaRef media{MediaId::generate(), "m", "x.wav", MediaStorage::Copied, 44100.0, 2, 88200, 1};
    addMediaAsNewTrack(model, media, 44100); // starts at 1 s
    model.tracks[0].clips[0].sourceStart = 22050; // 0.5 s into the file
    model.tracks[0].clips[0].length = 44100;
    model.tracks[0].gainDb = -6.0;

    MediaPool pool;
    SnapshotBuildReport report;
    auto empty = buildPlaybackSnapshot(model, pool, 48000.0, 1, &report);
    REQUIRE(report.missingMedia.size() == 1);
    REQUIRE(empty->tracks[0].clips.empty());

    LoadedMedia loaded;
    loaded.id = media.id;
    loaded.original = makeRamp(88200, 44100.0);
    loaded.rendition = makeRamp(96000, 48000.0);
    loaded.renditionRate = 48000.0;
    pool.put(loaded);

    report = {};
    const auto snapshot = buildPlaybackSnapshot(model, pool, 48000.0, 2, &report);
    REQUIRE(report.missingMedia.empty());
    REQUIRE(snapshot->sampleRate == 48000.0);
    const ClipPlayback& clip = snapshot->tracks[0].clips[0];
    REQUIRE(clip.timelineStart == 48000);
    REQUIRE(clip.sourceStart == 24000);
    REQUIRE(clip.length == 48000);
    REQUIRE(snapshot->endFrame == 96000);
    REQUIRE(std::abs(snapshot->tracks[0].gainLeft - 0.501187f) < 1e-5f);
    REQUIRE(snapshot->tracks[0].gainRight == snapshot->tracks[0].gainLeft);
    REQUIRE(snapshot->tracks[0].key != 0);
    REQUIRE(snapshot->keepAlive.size() == 1);

    // Rendition at the wrong rate counts as missing.
    report = {};
    buildPlaybackSnapshot(model, pool, 96000.0, 3, &report);
    REQUIRE(report.missingMedia.size() == 1);
}

TEST_CASE("Transients are found in a decoded 16-bit stem exactly as the app runs them", "[session][analysis][regression]")
{
    TempDir dir("decode-analyze");
    const auto wav = dir / "suno_drums.wav";
    REQUIRE(writeWavFile(wav, *makeDroneWithKicks(12.0, 44100.0, 0.5), WavSampleFormat::Pcm16).ok());
    const auto decoded = decodeAudioFile(wav);
    REQUIRE(decoded.ok());

    auto analyzer = createTransientAnalyzer();
    AnalysisContext context;
    context.progress = [](float) {};
    const auto result = analyzer->analyze(**decoded, nlohmann::json::object(), context);
    REQUIRE(result.ok());
    const auto frames = onsetFrames(result->data);
    REQUIRE(frames.size() == 24);
    for (std::size_t i = 0; i < frames.size(); ++i)
        REQUIRE(std::abs(static_cast<double>(frames[i]) / 44100.0 - 0.5 * static_cast<double>(i)) < 0.003);
}

TEST_CASE("TimeMapper round-trips positions", "[session][snapshot]")
{
    const TimeMapper map{44100.0, 48000.0};
    REQUIRE(map.toEngine(44100) == 48000);
    REQUIRE(map.toProject(48000) == 44100);
    for (std::int64_t p : {0LL, 1LL, 12345LL, 987654321LL})
        REQUIRE(std::llabs(map.toProject(map.toEngine(p)) - p) <= 1);
    const TimeMapper same{48000.0, 48000.0};
    REQUIRE(same.toEngine(123456789) == 123456789);
}

TEST_CASE("Track gains resolve fader, balance pan, mute and solo-in-place", "[session][mixer]")
{
    ProjectModel model = makeEmptyProject("mix");
    for (int i = 0; i < 3; ++i) {
        Track track;
        track.id = TrackId::generate();
        track.name = "t" + std::to_string(i);
        model.tracks.push_back(track);
    }
    auto gains = [&](std::size_t i) { return trackChannelGains(model, model.tracks[i]); };

    // Centre, 0 dB: exact unity, so untouched tracks render bit-identically.
    REQUIRE(gains(0).left == 1.0f);
    REQUIRE(gains(0).right == 1.0f);

    // Balance: panning right attenuates only the left side; hard pan silences it.
    model.tracks[0].pan = 0.5;
    REQUIRE(gains(0).right == 1.0f);
    REQUIRE(std::abs(gains(0).left - 0.70710678f) < 1e-6f);
    model.tracks[0].pan = 1.0;
    REQUIRE(gains(0).left == 0.0f);
    model.tracks[0].pan = -1.0;
    REQUIRE(gains(0).right == 0.0f);
    REQUIRE(gains(0).left == 1.0f);
    model.tracks[0].pan = 0.0;

    model.tracks[0].gainDb = -6.0;
    REQUIRE(std::abs(gains(0).left - 0.501187f) < 1e-5f);
    model.tracks[0].gainDb = kSilenceDb;
    REQUIRE(gains(0).left == 0.0f);
    model.tracks[0].gainDb = 0.0;

    // Solo-in-place: only soloed tracks are heard; several may be soloed.
    model.tracks[1].soloed = true;
    REQUIRE(gains(0).left == 0.0f);
    REQUIRE(gains(1).left == 1.0f);
    REQUIRE(gains(2).left == 0.0f);
    model.tracks[2].soloed = true;
    REQUIRE(gains(2).left == 1.0f);

    // Mute wins over solo.
    model.tracks[1].muted = true;
    REQUIRE(gains(1).left == 0.0f);
    REQUIRE_FALSE(model.isAudible(model.tracks[1]));
    REQUIRE(model.isAudible(model.tracks[2]));

    // Clearing every solo restores the non-muted tracks.
    model.tracks[1].soloed = model.tracks[2].soloed = false;
    REQUIRE(gains(0).left == 1.0f);
    REQUIRE(gains(1).left == 0.0f);
}

TEST_CASE("Snapshot track keys are stable across rebuilds and distinct per track", "[session][mixer]")
{
    ProjectModel model = makeEmptyProject("keys");
    MediaRef media{MediaId::generate(), "m", "x.wav", MediaStorage::Copied, 48000.0, 2, 4800, 1};
    addMediaAsNewTrack(model, media);
    addMediaAsNewTrack(model, media);
    MediaPool pool;
    const auto a = buildPlaybackSnapshot(model, pool, 48000.0, 1);
    model.tracks[0].gainDb = -12.0;
    const auto b = buildPlaybackSnapshot(model, pool, 48000.0, 2);
    REQUIRE(a->tracks[0].key == b->tracks[0].key);
    REQUIRE(a->tracks[1].key == b->tracks[1].key);
    REQUIRE(a->tracks[0].key != a->tracks[1].key);
    REQUIRE(b->tracks[0].gainLeft < a->tracks[0].gainLeft);
}
