// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Cross-stem bleed suppression on a project: stems judged against each other
// on the timeline, processed copies, strengths and refusals.

#include <daw/core/Files.h>
#include <daw/project/ProjectSerializer.h>
#include <daw/session/BleedTools.h>
#include <daw/session/ProjectOperations.h>
#include <daw/session/VoiceTools.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <numbers>

using namespace daw;
using namespace daw::test;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

/// 1 kHz bursts: on for 100 ms, off for 100 ms, from timeline frame 0.
float drumAt(std::int64_t timelineFrame)
{
    const bool on = (timelineFrame / 4800) % 2 == 0;
    return on ? static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(timelineFrame) / kRate))
              : 0.0f;
}

struct Stems {
    ProjectModel model = makeEmptyProject("bleed");
    std::map<MediaId, AudioAssetPtr> audio;
    TrackId drums, vocal, bass;
    MediaRef vocalMedia;

    MediaRef add(const std::string& name, std::shared_ptr<AudioAsset> asset)
    {
        MediaRef ref{MediaId::generate(), name, "Media/Original/" + name + ".wav", MediaStorage::Copied, asset->sampleRate,
                     asset->numChannels(), asset->numFrames(), 1};
        model.media.push_back(ref);
        audio[ref.id] = std::move(asset);
        return ref;
    }

    OriginalAudioLookup lookup() const
    {
        return [this](const MediaId& id) -> AudioAssetPtr {
            const auto it = audio.find(id);
            return it == audio.end() ? nullptr : it->second;
        };
    }
};

/// Drums (stereo 1 kHz bursts) at 0; a vocal stem placed at 0.5 s holding its
/// own 3 kHz tone plus the drums' bursts at -30 dB, as they fall on the
/// timeline; a mono bass at 100 Hz.
Stems makeStems()
{
    Stems s;
    s.model.sampleRate = kRate;
    const std::int64_t frames = static_cast<std::int64_t>(3 * kRate);
    auto drums = AudioAsset::create(2, frames, kRate);
    for (std::int64_t i = 0; i < frames; ++i)
        drums->channels[0][static_cast<std::size_t>(i)] = drums->channels[1][static_cast<std::size_t>(i)] = drumAt(i);
    const std::int64_t offset = static_cast<std::int64_t>(0.5 * kRate);
    const std::int64_t vocalFrames = static_cast<std::int64_t>(2 * kRate);
    auto vocal = AudioAsset::create(2, vocalFrames, kRate);
    const float bleed = static_cast<float>(std::pow(10.0, -30.0 / 20.0));
    for (std::int64_t i = 0; i < vocalFrames; ++i) {
        const float own = static_cast<float>(0.2 * std::sin(2.0 * std::numbers::pi * 3000.0 * static_cast<double>(i) / kRate));
        vocal->channels[0][static_cast<std::size_t>(i)] = vocal->channels[1][static_cast<std::size_t>(i)] = own + bleed * drumAt(i + offset);
    }
    const MediaRef drumsRef = s.add("Drums", drums);
    s.vocalMedia = s.add("Vocal", vocal);
    const MediaRef bassRef = s.add("Bass", makeSine(100.0, 3.0, kRate, 1, 0.3f));
    s.drums = addMediaAsNewTrack(s.model, drumsRef, 0);
    s.vocal = addMediaAsNewTrack(s.model, s.vocalMedia, offset);
    s.bass = addMediaAsNewTrack(s.model, bassRef, 0);
    return s;
}

/// Power of `hz` in frames [from, to) of channel 0.
double tonePower(const AudioAsset& audio, double hz, std::int64_t from, std::int64_t to)
{
    return goertzelPower(audio.channelData(0) + from, to - from, hz, audio.sampleRate);
}

double db(double powerRatio)
{
    return 10.0 * std::log10(powerRatio);
}

} // namespace

TEST_CASE("Bleed is suppressed against the other stems where they fall on the timeline", "[session][bleed]")
{
    Stems s = makeStems();
    CHECK(bleedTracks(s.model) == std::vector<TrackId>{s.drums, s.vocal, s.bass});
    TempDir dir("bleed");
    double lastProgress = 0.0;
    const BleedStrength& medium = kBleedStrengths[1];
    auto result = suppressBleed(dir.path(), s.model, s.lookup(), {s.vocal}, medium, kRate, {}, [&](double p) { lastProgress = p; });
    REQUIRE(result.ok());
    CHECK(lastProgress == Approx(1.0));
    REQUIRE(result->size() == 1);
    const BleedTrackOutcome& outcome = result->front();
    CHECK(outcome.track == s.vocal);
    REQUIRE(outcome.media.size() == 1);
    const MediaRef& copy = outcome.media[0];
    REQUIRE(copy.derivation.has_value());
    CHECK(copy.derivation->source == s.vocalMedia.id);
    CHECK(copy.derivation->process == kBleedSuppressProcess);
    CHECK(copy.derivation->amountDb == medium.reductionDb);
    CHECK(copy.numFrames == s.vocalMedia.numFrames);
    CHECK(copy.sampleRate == kRate);
    CHECK(std::filesystem::exists(dir / utf8ToPath(copy.path)));

    // The drums' bursts inside the vocal drop by the strength's limit; its own tone stays.
    const AudioAsset& before = *s.audio[s.vocalMedia.id];
    const AudioAsset& after = *outcome.loaded[0].original;
    REQUIRE(after.numFrames() == before.numFrames());
    const std::int64_t from = 4800, to = before.numFrames() - 4800;
    CHECK(db(tonePower(after, 1000.0, from, to) / tonePower(before, 1000.0, from, to)) == Approx(-medium.reductionDb).margin(1.5));
    CHECK(db(tonePower(after, 3000.0, from, to) / tonePower(before, 3000.0, from, to)) == Approx(0.0).margin(0.2));
    CHECK(outcome.musicChangeDb > -0.5);

    // The track plays the copy; the strength is recognised from it.
    REQUIRE(applyProcessedMedia(s.model, s.vocal, outcome.media));
    const auto applied = trackBleedSuppression(s.model, s.vocal);
    REQUIRE(applied.has_value());
    CHECK(std::string(bleedStrengthFor(applied->amountDb).name) == "Medium");
    CHECK_FALSE(trackVoiceDenoise(s.model, s.vocal).has_value());
    CHECK(trackProcessing(s.model, s.vocal)->process == kBleedSuppressProcess);
    CHECK(validateProject(s.model).ok());

    // Re-applying renders from the originals again, not from the copy.
    auto again = suppressBleed(dir.path(), s.model, s.lookup(), {s.vocal}, kBleedStrengths[2], kRate);
    REQUIRE(again.ok());
    const AudioAsset& strong = *again->front().loaded[0].original;
    CHECK(db(tonePower(strong, 1000.0, from, to) / tonePower(before, 1000.0, from, to)) == Approx(-24.0).margin(1.5));
    CHECK(again->front().media[0].path != copy.path);
}

TEST_CASE("Bleed suppression processes only what the clips play, and keeps mono stems mono", "[session][bleed]")
{
    Stems s = makeStems();
    // The vocal clip skips the first 0.25 s of its media.
    Clip& clip = s.model.findTrack(s.vocal)->clips[0];
    const std::int64_t skip = static_cast<std::int64_t>(0.25 * kRate);
    clip.sourceStart = skip;
    clip.timelineStart += skip;
    clip.length -= skip;
    TempDir dir("bleedclip");
    auto result = suppressBleed(dir.path(), s.model, s.lookup(), {s.vocal, s.bass}, kBleedStrengths[1], kRate);
    REQUIRE(result.ok());
    REQUIRE(result->size() == 2);
    const AudioAsset& before = *s.audio[s.vocalMedia.id];
    const AudioAsset& after = *(*result)[0].loaded[0].original;
    for (std::int64_t i = 0; i < skip; ++i)
        REQUIRE(after.channels[0][static_cast<std::size_t>(i)] == before.channels[0][static_cast<std::size_t>(i)]);
    CHECK(db(tonePower(after, 1000.0, skip + 4800, after.numFrames() - 4800)
             / tonePower(before, 1000.0, skip + 4800, after.numFrames() - 4800))
          < -15.0);

    const BleedTrackOutcome& bass = (*result)[1];
    CHECK(bass.track == s.bass);
    CHECK(bass.media[0].numChannels == 1);
    CHECK(bass.loaded[0].original->numChannels() == 1);
    CHECK(bass.musicChangeDb > -0.5); // the bass owns its bins
}

TEST_CASE("Bleed suppression refuses what it cannot compare", "[session][bleed]")
{
    Stems s = makeStems();
    TempDir dir("bleedrefuse");

    // Nothing asked for: nothing to do.
    auto none = suppressBleed(dir.path(), s.model, s.lookup(), {}, kBleedStrengths[0], kRate);
    REQUIRE(none.ok());
    CHECK(none->empty());

    // A track that is not in the project.
    auto unknown = suppressBleed(dir.path(), s.model, s.lookup(), {TrackId::generate()}, kBleedStrengths[0], kRate);
    REQUIRE_FALSE(unknown.ok());
    CHECK(unknown.error().code == ErrorCode::InvalidArgument);

    // Audio still loading.
    auto loading = suppressBleed(dir.path(), s.model, [](const MediaId&) { return AudioAssetPtr{}; }, {s.vocal}, kBleedStrengths[0], kRate);
    REQUIRE_FALSE(loading.ok());
    CHECK(loading.error().code == ErrorCode::Unavailable);

    // A lone stem has nothing to be compared with.
    ProjectModel lone = s.model;
    REQUIRE(removeTrack(lone, s.drums));
    REQUIRE(removeTrack(lone, s.bass));
    auto single = suppressBleed(dir.path(), lone, s.lookup(), {s.vocal}, kBleedStrengths[0], kRate);
    REQUIRE_FALSE(single.ok());
    CHECK(single.error().code == ErrorCode::InvalidArgument);

    CHECK(std::string(bleedStrengthFor(11.0).name) == "Light");
    CHECK(std::string(bleedStrengthFor(30.0).name) == "Strong");
}
