// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Milestone 1 definition of done, exercised headlessly (no audio device):
//
//   load a WAV, see its waveform (peaks), play it, stop it, seek, save the
//   project, reopen it, and reproduce the same playback state - while
//   background analysis runs without blocking.

#include <daw/ai/AnalysisService.h>
#include <daw/ai/Analyzers.h>
#include <daw/core/Files.h>
#include <daw/engine/AudioEngine.h>
#include <daw/media/WavWriter.h>
#include <daw/project/ProjectStore.h>
#include <daw/session/MediaLoader.h>
#include <daw/session/ProjectOperations.h>
#include <daw/session/SnapshotBuilder.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace daw;
using namespace daw::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct Session {
    fs::path root;
    ProjectModel model;
    MediaPool pool;
    AudioEngine engine;
    std::uint64_t snapshotVersion = 0;

    Status loadAllMedia(double engineRate)
    {
        MediaLoadOptions options;
        options.engineSampleRate = engineRate;
        for (const MediaRef& media : model.media) {
            auto loaded = loadMedia(root, media, options);
            if (!loaded)
                return loaded.error();
            pool.put(std::move(*loaded));
        }
        return Status::success();
    }

    void start(double engineRate)
    {
        engine.prepare({engineRate, 512, 2});
        SnapshotBuildReport report;
        engine.publishSnapshot(buildPlaybackSnapshot(model, pool, engineRate, ++snapshotVersion, &report));
        REQUIRE(report.missingMedia.empty());
        engine.seek(TimeMapper{model.sampleRate, engineRate}.toEngine(model.playhead));
    }

    std::vector<float> render(std::int64_t frames)
    {
        std::vector<float> captured;
        std::vector<float> l(512), r(512);
        for (std::int64_t done = 0; done < frames; done += 512) {
            const int n = static_cast<int>(std::min<std::int64_t>(512, frames - done));
            float* outs[2] = {l.data(), r.data()};
            engine.process(outs, 2, n);
            captured.insert(captured.end(), l.begin(), l.begin() + n);
        }
        return captured;
    }
};

} // namespace

TEST_CASE("Load, play, stop, seek, save, reopen and reproduce playback", "[integration][dod]")
{
    const double engineRate = GENERATE(44100.0, 48000.0); // native and resampled paths
    TempDir dir("dod");
    const auto source = dir / "suno_full_mix.wav";
    REQUIRE(writeWavFile(source, *makeNoise(44100 * 3, 44100.0, 2, 0.5f, 77), WavSampleFormat::Float32).ok());

    std::vector<float> beforeSave;
    std::int64_t savedPlayheadEngine = 0;
    const fs::path root = dir / utf8ToPath("Industrial Session ✓");

    {
        Session s;
        s.root = root;
        s.model = makeEmptyProject("DoD");
        REQUIRE(ProjectStore::createLayout(root).ok());

        // Load a WAV.
        const auto media = importAudioFile(root, source);
        REQUIRE(media.ok());
        addMediaAsNewTrack(s.model, *media);
        REQUIRE(s.loadAllMedia(engineRate).ok());

        // See its waveform.
        const auto loaded = s.pool.get(media->id);
        REQUIRE(loaded->peaks);
        REQUIRE(loaded->peaks->numFrames() == 44100 * 3);
        REQUIRE(loaded->peaks->query(0, 0, 44100).max > 0.4f);

        // Play, stop, seek.
        s.start(engineRate);
        s.engine.play();
        s.render(engineRate == 48000.0 ? 48000 : 44100);
        s.engine.stop();
        const auto stopped = s.engine.transportState();
        REQUIRE_FALSE(stopped.playing);
        REQUIRE(stopped.playhead == static_cast<std::int64_t>(engineRate));
        s.engine.seek(static_cast<std::int64_t>(engineRate * 1.5));
        REQUIRE(s.engine.transportState().playhead == static_cast<std::int64_t>(engineRate * 1.5));

        // Save (playhead stored in project frames).
        savedPlayheadEngine = s.engine.transportState().playhead;
        s.model.playhead = TimeMapper{s.model.sampleRate, engineRate}.toProject(savedPlayheadEngine);
        REQUIRE(ProjectStore::save(root, s.model).ok());

        s.engine.play();
        beforeSave = s.render(8192);
    }

    // Reopen in a fresh session and reproduce the same playback state.
    Session reopened;
    reopened.root = root;
    auto model = ProjectStore::load(root);
    REQUIRE(model.ok());
    reopened.model = *model;
    REQUIRE(reopened.model.playhead == 66150); // 1.5 s at the 44.1 kHz project rate
    REQUIRE(reopened.loadAllMedia(engineRate).ok());
    REQUIRE(reopened.pool.get(reopened.model.media[0].id)->peaksFromCache);
    reopened.start(engineRate);
    REQUIRE(reopened.engine.transportState().playhead == savedPlayheadEngine);
    reopened.engine.play();
    const auto afterReopen = reopened.render(8192);
    REQUIRE(afterReopen == beforeSave); // bit-identical
}

TEST_CASE("Background analysis does not block playback", "[integration][dod][threads]")
{
    const auto audio = makeNoise(48000 * 20, 48000.0, 2, 0.5f, 5);

    AnalysisService service(AnalysisServiceConfig{2});
    REQUIRE(registerBuiltinAnalyzers(service).ok());
    for (const char* analyzer : {kLevelAnalyzerId, kTransientAnalyzerId, kSpectralAnalyzerId})
        REQUIRE(service.submit({analyzer, audio}).ok());

    // Meanwhile the "audio thread" keeps rendering on time.
    AudioEngine engine;
    engine.prepare({48000.0, 256, 2});
    auto snapshot = std::make_unique<PlaybackSnapshot>();
    snapshot->sampleRate = 48000.0;
    snapshot->keepAlive.push_back(audio);
    TrackPlayback track;
    track.clips.push_back(ClipPlayback{audio.get(), 0, 0, audio->numFrames(), 1.0f});
    snapshot->tracks.push_back(std::move(track));
    snapshot->endFrame = audio->numFrames();
    engine.publishSnapshot(std::move(snapshot));
    engine.play();

    // Device callbacks run at elevated priority (WASAPI/MMCSS "Pro Audio");
    // model that, so the test measures lock contention with the analysis
    // workers rather than OS preemption by other test processes running in
    // parallel.
    const HANDLE thread = GetCurrentThread();
    const int previousPriority = GetThreadPriority(thread);
    SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
    std::vector<float> l(256), r(256);
    float* outs[2] = {l.data(), r.data()};
    for (int i = 0; i < 2000; ++i)
        engine.process(outs, 2, 256);
    SetThreadPriority(thread, previousPriority);

    REQUIRE(engine.diagnostics().deadlineMisses == 0);
    REQUIRE(service.waitIdle(120s));
}
