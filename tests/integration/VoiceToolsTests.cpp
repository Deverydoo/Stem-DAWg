// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Voice denoise: processed copies of a track's media, going back to the
// original, the model package, and (when the model is installed) the model itself.

#include <daw/ai/InferenceBackend.h>
#include <daw/core/Files.h>
#include <daw/project/ProjectSerializer.h>
#include <daw/session/ProjectOperations.h>
#include <daw/session/VoiceTools.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace daw;
using namespace daw::test;

namespace {

MediaRef mediaRef(const std::string& name, std::optional<MediaDerivation> derivation = std::nullopt)
{
    MediaRef ref{MediaId::generate(), name, "Media/Original/" + name + ".wav", MediaStorage::Copied, 48000.0, 2, 48000, 1};
    ref.derivation = std::move(derivation);
    return ref;
}

} // namespace

TEST_CASE("A track plays a processed copy and goes back to its original", "[session][voice]")
{
    ProjectModel model = makeEmptyProject("voice");
    model.sampleRate = 48000.0;
    const MediaRef recording = mediaRef("Take");
    model.media.push_back(recording);
    const TrackId vocal = addMediaAsNewTrack(model, recording, 0);
    const MediaRef other = mediaRef("Guitar");
    model.media.push_back(other);
    const TrackId guitar = addMediaAsNewTrack(model, other, 0);
    CHECK_FALSE(trackVoiceDenoise(model, vocal).has_value());
    REQUIRE(trackOriginalMedia(model, vocal).size() == 1);

    const MediaRef strong = mediaRef("Take (voice)", MediaDerivation{recording.id, kVoiceDenoiseProcess, 100.0});
    REQUIRE(applyProcessedMedia(model, vocal, {strong}));
    CHECK(model.findTrack(vocal)->clips[0].media == strong.id);
    CHECK(model.findTrack(guitar)->clips[0].media == other.id); // other tracks untouched
    REQUIRE(trackVoiceDenoise(model, vocal).has_value());
    CHECK(trackVoiceDenoise(model, vocal)->amountDb == 100.0);
    CHECK(originalMediaOf(model, strong.id)->id == recording.id);
    CHECK(trackOriginalMedia(model, vocal)[0].id == recording.id);
    CHECK(model.findMedia(recording.id) != nullptr); // the original stays: a clip plays its copy
    CHECK(validateProject(model).ok());

    // Re-applying at another strength replaces the copy (made from the original again).
    const MediaRef gentle = mediaRef("Take (voice) 2", MediaDerivation{recording.id, kVoiceDenoiseProcess, 12.0});
    REQUIRE(applyProcessedMedia(model, vocal, {gentle}));
    CHECK(model.findTrack(vocal)->clips[0].media == gentle.id);
    CHECK(model.findMedia(strong.id) == nullptr); // unused copies leave the model (the file stays for undo)
    CHECK_FALSE(applyProcessedMedia(model, vocal, {gentle})); // nothing changes

    // The derivation round-trips through the project file.
    const auto parsed = parseProject(serializeProject(model));
    REQUIRE(parsed.ok());
    const MediaRef* copy = parsed->findMedia(gentle.id);
    REQUIRE(copy != nullptr);
    REQUIRE(copy->derivation.has_value());
    CHECK(*copy->derivation == *gentle.derivation);
    CHECK(*parsed == model);

    // Deleting the track keeps nothing; reverting keeps only the original.
    ProjectModel removed = model;
    REQUIRE(removeTrack(removed, vocal));
    CHECK(removed.findMedia(recording.id) == nullptr);
    CHECK(removed.findMedia(gentle.id) == nullptr);

    REQUIRE(revertTrackProcessing(model, vocal));
    CHECK(model.findTrack(vocal)->clips[0].media == recording.id);
    CHECK(model.findMedia(gentle.id) == nullptr);
    CHECK_FALSE(trackVoiceDenoise(model, vocal).has_value());
    CHECK_FALSE(revertTrackProcessing(model, vocal));

    // A copy derived from media that is not in the project is invalid.
    ProjectModel broken = model;
    broken.media.push_back(mediaRef("Orphan", MediaDerivation{MediaId::generate(), kVoiceDenoiseProcess, 12.0}));
    CHECK_FALSE(validateProject(broken).ok());
}

TEST_CASE("Clips whose processed copy is gone play their original again", "[session][voice]")
{
    TempDir dir("missingcopy");
    ProjectModel model = makeEmptyProject("missing");
    const MediaRef recording = mediaRef("Take");
    model.media.push_back(recording);
    const TrackId vocal = addMediaAsNewTrack(model, recording, 0);
    const MediaRef copy = mediaRef("Take (bleed)", MediaDerivation{recording.id, "bleed-suppress", 18.0});
    REQUIRE(applyProcessedMedia(model, vocal, {copy}));

    // The copy's file exists: nothing changes.
    REQUIRE(writeFileAtomically(dir / utf8ToPath(copy.path), std::string("audio")).ok());
    CHECK(fallBackFromMissingProcessedMedia(model, dir.path()) == 0);
    CHECK(model.findTrack(vocal)->clips[0].media == copy.id);

    // Gone: the clip plays the original and the copy leaves the model.
    std::filesystem::remove(dir / utf8ToPath(copy.path));
    CHECK(fallBackFromMissingProcessedMedia(model, dir.path()) == 1);
    CHECK(model.findTrack(vocal)->clips[0].media == recording.id);
    CHECK(model.findMedia(copy.id) == nullptr);
    CHECK(validateProject(model).ok());
}

TEST_CASE("The voice model package is verified before it is installed", "[session][voice]")
{
    TempDir dir("voicemodel");
    CHECK_FALSE(isVoiceModelInstalled(dir.path()));
    REQUIRE(writeFileAtomically(dir / "fake.tar.gz", std::string("not the model")).ok());
    const auto refused = installVoiceModel(dir / "fake.tar.gz", dir / "model");
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().code == ErrorCode::Corrupt);
    CHECK_FALSE(std::filesystem::exists(dir / "model" / "enc.onnx"));
    CHECK(voiceModelDirectory().filename() == "DeepFilterNet3");
}

TEST_CASE("The installed voice model removes noise and keeps the original's rate and length", "[session][voice][model]")
{
    if (!isVoiceModelInstalled())
        SKIP("the DeepFilterNet3 voice model is not installed (Clean-up > Voice denoise > Download)");
    auto backend = createOnnxBackend();
    REQUIRE(backend.ok());
    auto denoiser = VoiceDenoiser::load(*backend, voiceModelDirectory());
    REQUIRE(denoiser.ok());

    // Steady noise only, at 44.1 kHz: the model removes most of it.
    TempDir dir("voicerun");
    const double rate = 44100.0;
    auto noise = makeNoise(static_cast<std::int64_t>(4 * rate), rate, 2, 0.05f, 5);
    const MediaRef original{MediaId::generate(), "Room", "Media/Original/Room.wav", MediaStorage::Copied, rate, 2,
                            noise->numFrames(), 1};
    double lastProgress = 0.0;
    auto outcome = denoiseVoiceMedia(dir.path(), original, *noise, **denoiser, 100.0, 48000.0, {}, [&](double p) { lastProgress = p; });
    REQUIRE(outcome.ok());
    CHECK(lastProgress == 1.0);
    CHECK(outcome->media.sampleRate == rate);
    CHECK(outcome->media.numFrames == noise->numFrames());
    REQUIRE(outcome->media.derivation.has_value());
    CHECK(outcome->media.derivation->source == original.id);
    CHECK(outcome->noiseChangeDb < -15.0);
    CHECK(std::filesystem::exists(dir / utf8ToPath(outcome->media.path)));
    CHECK(outcome->loaded.original->numFrames() == noise->numFrames());
    CHECK(outcome->loaded.renditionRate == 48000.0);

    // A reduction limit caps how far it goes.
    auto limited = denoiseVoiceMedia(dir.path(), original, *noise, **denoiser, 6.0, 48000.0);
    REQUIRE(limited.ok());
    CHECK(limited->noiseChangeDb > -6.5);
    CHECK(limited->noiseChangeDb < -3.0);
    CHECK(limited->media.path != outcome->media.path); // a new file, the first kept for undo

    // MIDI tracks are refused.
    MediaRef midi = original;
    midi.path = "Media/Original/Song.mid";
    CHECK(denoiseVoiceMedia(dir.path(), midi, *noise, **denoiser, 12.0, 48000.0).error().code == ErrorCode::InvalidArgument);
}
