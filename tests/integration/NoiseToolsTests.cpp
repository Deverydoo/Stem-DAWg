// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Learning noise profiles, detecting hum, and the noise stages end to end.

#include <daw/dsp/SpectralDenoiser.h>
#include <daw/session/NoiseTools.h>
#include <daw/session/ProjectOperations.h>
#include <daw/session/SnapshotBuilder.h>
#include <daw/session/StemReport.h>

#include "TestSignals.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace daw;
using namespace daw::test;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr double kSeconds = 8.0;
const auto kFrames = static_cast<std::int64_t>(kSeconds * kRate);

struct Fixture {
    ProjectModel model = makeEmptyProject("noise");
    MediaPool pool;

    Fixture() { model.sampleRate = kRate; }

    TrackId add(const std::string& name, const AudioAssetPtr& asset)
    {
        MediaRef ref{MediaId::generate(), name, name + ".wav", MediaStorage::Copied, asset->sampleRate, asset->numChannels(),
                     asset->numFrames(), 1};
        model.media.push_back(ref);
        const TrackId id = addMediaAsNewTrack(model, ref, 0);
        renameTrack(model, id, name);
        LoadedMedia loaded;
        loaded.id = ref.id;
        loaded.original = asset;
        pool.put(loaded);
        return id;
    }
};

/// Uniform noise of amplitude `noise` (RMS noise / sqrt 3) plus a 1 kHz note
/// for the first half of every second, plus optional hum.
AudioAssetPtr recording(float noise, double humHz = 0.0, float humLevel = 0.0f)
{
    auto asset = makeNoise(kFrames, kRate, 2, noise, 11);
    for (std::int64_t i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        float v = std::fmod(t, 1.0) < 0.5 ? 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * t)) : 0.0f;
        if (humHz > 0.0)
            v += humLevel * static_cast<float>(std::sin(2.0 * std::numbers::pi * humHz * t)
                                               + 0.5 * std::sin(2.0 * std::numbers::pi * 2.0 * humHz * t)
                                               + 0.3 * std::sin(2.0 * std::numbers::pi * 3.0 * humHz * t));
        for (auto& channel : asset->channels)
            channel[static_cast<std::size_t>(i)] += v;
    }
    return asset;
}

} // namespace

TEST_CASE("A learned noise profile matches the noise between the notes", "[session][noise]")
{
    Fixture f;
    const float amplitude = 0.005f; // uniform: variance a^2 / 3
    const TrackId id = f.add("Vocal", recording(amplitude));
    const auto profile = learnNoiseProfile(f.model, f.pool, id);
    REQUIRE(profile.ok());
    CHECK(profile->sampleRate == kRate);
    CHECK(profile->fftSize == SpectralDenoiser::kFftSize);
    CHECK(profile->powerDb.size() == static_cast<std::size_t>(SpectralDenoiser::kBins));
    CHECK(profile->seconds > 0.5);

    // White noise of variance s^2 has a mean frame power of s^2 * N / 2 per bin
    // (square-root Hann windows, both channels averaged).
    const double variance = amplitude * amplitude / 3.0;
    const double expectedDb = 10.0 * std::log10(variance * SpectralDenoiser::kFftSize / 2.0);
    double sum = 0.0;
    int count = 0;
    for (int k = 100; k < 800; ++k) { // 2.3 to 18.8 kHz, well away from the 1 kHz note
        sum += profile->powerDb[static_cast<std::size_t>(k)];
        ++count;
    }
    CHECK(sum / count == Approx(expectedDb).margin(0.5));
}

TEST_CASE("Noise reduction with a learned profile lowers the bed by the reduction amount", "[session][noise]")
{
    Fixture f;
    const TrackId id = f.add("Vocal", recording(0.005f)); // bed -50.8 dBFS RMS
    auto before = analyzeStems(f.model, f.pool);
    REQUIRE(before.ok());
    REQUIRE(before->tracks[0].noiseFloorDbfs.has_value());

    auto profile = learnNoiseProfile(f.model, f.pool, id);
    REQUIRE(profile.ok());
    TrackCleanup& c = f.model.findTrack(id)->cleanup;
    c.denoise.enabled = true;
    c.denoise.reductionDb = 12.0;
    c.denoise.profile = *profile;
    auto after = analyzeStems(f.model, f.pool);
    REQUIRE(after.ok());
    REQUIRE(after->tracks[0].noiseFloorDbfs.has_value());
    CHECK(*before->tracks[0].noiseFloorDbfs - *after->tracks[0].noiseFloorDbfs == Approx(12.0).margin(1.5));
    // The notes are not touched.
    CHECK(after->tracks[0].loudness.integratedLufs == Approx(before->tracks[0].loudness.integratedLufs).margin(0.3));
}

TEST_CASE("Hum detection finds 50 or 60 Hz hum and its harmonics, and nothing in clean audio", "[session][noise][hum]")
{
    for (const double mains : {50.0, 60.0}) {
        INFO("mains " << mains);
        Fixture f;
        const TrackId id = f.add("Guitar", recording(0.001f, mains, 0.01f));
        const auto hum = detectHum(f.model, f.pool, id);
        REQUIRE(hum.ok());
        REQUIRE(hum->has_value());
        CHECK((*hum)->frequencyHz == mains);
        CHECK((*hum)->harmonics >= 3);
        CHECK((*hum)->prominenceDb >= kHumDetectDb);
    }
    Fixture clean;
    const TrackId id = clean.add("Vocal", recording(0.001f));
    const auto none = detectHum(clean.model, clean.pool, id);
    REQUIRE(none.ok());
    CHECK_FALSE(none->has_value());

    // A bass note near the mains frequency (G1 = 49 Hz, with harmonics) is not hum.
    Fixture bass;
    const TrackId note = bass.add("Bass", recording(0.001f, 49.0, 0.01f));
    const auto notHum = detectHum(bass.model, bass.pool, note);
    REQUIRE(notHum.ok());
    CHECK_FALSE(notHum->has_value());

    // With hum removal on, the measured hum (after the chain) is gone too.
    Fixture fixed;
    const TrackId humId = fixed.add("Guitar", recording(0.001f, 60.0, 0.01f));
    TrackCleanup& c = fixed.model.findTrack(humId)->cleanup;
    c.hum = {true, 60.0, 4};
    auto profile = learnNoiseProfile(fixed.model, fixed.pool, humId); // learned after the hum stage
    REQUIRE(profile.ok());
    const auto bin = [&](double hz) { return profile->powerDb[static_cast<std::size_t>(std::lround(hz * SpectralDenoiser::kFftSize / kRate))]; };
    CHECK(bin(60.0) < bin(90.0) + 6.0); // no hum peak left in what the denoiser would see
}

TEST_CASE("A selected region teaches the noise in it, not the quietest stretch of the track", "[session][noise][region]")
{
    // Fan noise for the first half, then a much quieter room: the quietest
    // frames are the room, but the fan is what needs removing.
    Fixture f;
    auto asset = makeNoise(kFrames, kRate, 2, 0.005f, 23);
    for (auto& channel : asset->channels)
        for (std::size_t i = static_cast<std::size_t>(kFrames / 2); i < channel.size(); ++i)
            channel[i] *= 0.1f;
    const TrackId id = f.add("Vocal", asset);

    const auto bandDb = [](const NoiseProfile& p) {
        double sum = 0.0;
        for (int k = 100; k < 800; ++k)
            sum += p.powerDb[static_cast<std::size_t>(k)];
        return sum / 700.0;
    };
    const auto levelDb = [](float amplitude) {
        return 10.0 * std::log10(amplitude * amplitude / 3.0 * SpectralDenoiser::kFftSize / 2.0);
    };

    const auto quietest = learnNoiseProfile(f.model, f.pool, id);
    REQUIRE(quietest.ok());
    CHECK(bandDb(*quietest) == Approx(levelDb(0.0005f)).margin(0.5));

    NoiseLearnOptions options;
    options.region = FrameRange{static_cast<std::int64_t>(1.0 * kRate), static_cast<std::int64_t>(3.0 * kRate)};
    const auto fan = learnNoiseProfile(f.model, f.pool, id, options);
    REQUIRE(fan.ok());
    CHECK(bandDb(*fan) == Approx(levelDb(0.005f)).margin(0.5));
    CHECK(fan->seconds == Approx(2.0).margin(0.1)); // all of the region, not a tenth of it
    CHECK(fan->fromSelection);
    CHECK_FALSE(quietest->fromSelection);

    // Too short, past the end, or silent: refused with a reason.
    options.region = FrameRange{0, 1000};
    CHECK(learnNoiseProfile(f.model, f.pool, id, options).error().code == ErrorCode::InvalidArgument);
    options.region = FrameRange{kFrames + 10, kFrames + 100000};
    CHECK(learnNoiseProfile(f.model, f.pool, id, options).error().code == ErrorCode::InvalidArgument);
    options.region = FrameRange{0, static_cast<std::int64_t>(0.2 * kRate)}; // long enough to measure, too little to learn from
    CHECK(learnNoiseProfile(f.model, f.pool, id, options).error().code == ErrorCode::InvalidArgument);
    const TrackId silent = f.add("Empty", makeSilence(kFrames, kRate));
    options.region = FrameRange{0, kFrames};
    CHECK(learnNoiseProfile(f.model, f.pool, silent, options).error().code == ErrorCode::InvalidArgument);

    // Hum detection measures the region too: hum only in the first two seconds
    // is found there, and not in a region after it.
    Fixture h;
    auto hummy = std::make_shared<AudioAsset>(*recording(0.001f));
    for (std::int64_t i = 0; i < static_cast<std::int64_t>(2.0 * kRate); ++i)
        for (auto& channel : hummy->channels)
            channel[static_cast<std::size_t>(i)] += 0.01f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 50.0 * i / kRate));
    const TrackId guitar = h.add("Guitar", hummy);
    const auto inHum = detectHum(h.model, h.pool, guitar, {}, FrameRange{0, static_cast<std::int64_t>(2.0 * kRate)});
    REQUIRE(inHum.ok());
    REQUIRE(inHum->has_value());
    CHECK((*inHum)->frequencyHz == 50.0);
    const auto after = detectHum(h.model, h.pool, guitar, {}, FrameRange{static_cast<std::int64_t>(3.0 * kRate), kFrames});
    REQUIRE(after.ok());
    CHECK_FALSE(after->has_value());
}

TEST_CASE("Noise tools refuse silent or missing tracks", "[session][noise]")
{
    Fixture f;
    const TrackId silent = f.add("Empty", makeSilence(kFrames, kRate));
    CHECK(learnNoiseProfile(f.model, f.pool, silent).error().code == ErrorCode::InvalidArgument);
    CHECK(detectHum(f.model, f.pool, silent).error().code == ErrorCode::InvalidArgument);
    CHECK(learnNoiseProfile(f.model, f.pool, TrackId("missing")).error().code == ErrorCode::NotFound);
}

TEST_CASE("Noise profiles follow the engine rate; listen-to-removed silences unprocessed tracks", "[session][noise][snapshot]")
{
    NoiseProfile profile;
    profile.sampleRate = 48000.0;
    profile.fftSize = SpectralDenoiser::kFftSize;
    for (int k = 0; k < SpectralDenoiser::kBins; ++k)
        profile.powerDb.push_back(-60.0f - 0.01f * static_cast<float>(k));

    const auto same = noiseProfileForEngine(profile, 48000.0);
    REQUIRE(same.size() == static_cast<std::size_t>(SpectralDenoiser::kBins));
    CHECK(10.0 * std::log10(same[100]) == Approx(-61.0).margin(1e-3));
    const auto doubled = noiseProfileForEngine(profile, 96000.0);
    CHECK(10.0 * std::log10(doubled[100]) == Approx(-62.0).margin(1e-3));  // 4.7 kHz: bin 200 of the 48 kHz profile
    CHECK(10.0 * std::log10(doubled.back()) == Approx(-70.24).margin(1e-3)); // above 24 kHz: the top value
    CHECK(noiseProfileForEngine({}, 48000.0).empty());

    Fixture f;
    const TrackId cleaned = f.add("Vocal", recording(0.005f));
    f.add("Drums", recording(0.005f));
    TrackCleanup& c = f.model.findTrack(cleaned)->cleanup;
    c.denoise.enabled = true;
    c.denoise.profile = profile;
    const auto snapshot = buildPlaybackSnapshot(f.model, f.pool, kRate, 1, nullptr, {.listenRemoved = true});
    REQUIRE(snapshot->tracks.size() == 2);
    CHECK(snapshot->tracks[0].cleanup.listenRemoved);
    REQUIRE(snapshot->tracks[0].cleanup.noiseProfile != nullptr);
    CHECK(snapshot->noiseProfiles.size() == 1);
    CHECK(snapshot->tracks[1].gainLeft == 0.0f); // no chain: nothing removed, nothing heard
    CHECK(snapshot->tracks[1].gainRight == 0.0f);
}
