// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Stem triage on a synthetic "Suno-like" stem set whose problems are known:
// a bass with a noise bed, a percussion stem that doubles the bass's low end,
// a synth, a residue stem that only repeats the synth very quietly, and an
// empty stem. The mix of them clips.

#include <daw/session/ProjectOperations.h>
#include <daw/session/StemReport.h>

#include "TestSignals.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

using namespace daw;
using namespace daw::test;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr double kSeconds = 12.0;

struct Fixture {
    ProjectModel model = makeEmptyProject("stems");
    MediaPool pool;

    std::size_t add(const std::string& name, const AudioAssetPtr& asset)
    {
        MediaRef ref{MediaId::generate(), name, name + ".wav", MediaStorage::Copied, asset->sampleRate, asset->numChannels(),
                     asset->numFrames(), 1};
        model.media.push_back(ref);
        addMediaAsNewTrack(model, ref, 0);
        model.tracks.back().name = name;
        LoadedMedia loaded;
        loaded.id = ref.id;
        loaded.original = asset;
        pool.put(loaded);
        return model.tracks.size() - 1;
    }
};

/// 50 Hz sine switched on for half of every second (a bass line), plus an
/// optional uniform noise bed.
std::shared_ptr<AudioAsset> gatedBass(float amplitude, float noise)
{
    const auto frames = static_cast<std::int64_t>(kSeconds * kRate);
    auto asset = AudioAsset::create(2, frames, kRate);
    const auto noiseBed = makeNoise(frames, kRate, 2, noise, 7);
    for (std::int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const bool on = std::fmod(t, 1.0) < 0.5;
        const float tone = on ? amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * 50.0 * t)) : 0.0f;
        for (int c = 0; c < 2; ++c)
            asset->channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] =
                tone + noiseBed->channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)];
    }
    return asset;
}

/// A mono synth chord (440 + 660 + 1320 Hz).
std::shared_ptr<AudioAsset> synth(float amplitude)
{
    const auto frames = static_cast<std::int64_t>(kSeconds * kRate);
    auto asset = AudioAsset::create(1, frames, kRate);
    for (std::int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        asset->channels[0][static_cast<std::size_t>(i)] =
            amplitude * static_cast<float>((std::sin(2 * std::numbers::pi * 440 * t) + 0.6 * std::sin(2 * std::numbers::pi * 660 * t)
                                            + 0.3 * std::sin(2 * std::numbers::pi * 1320 * t)) / 1.9);
    }
    return asset;
}

const StemAdvice* findAdvice(const StemReport& report, const std::string& code, std::optional<std::size_t> track = {})
{
    for (const StemAdvice& a : report.advice)
        if (a.code == code && (!track || a.track == track))
            return &a;
    return nullptr;
}

} // namespace

TEST_CASE("Stem report measures each stem and finds the known problems", "[session][stems]")
{
    Fixture f;
    f.model.sampleRate = kRate;
    const std::size_t bass = f.add("Bass", gatedBass(0.5f, 0.005f));   // bed: 0.005/sqrt(3) RMS = -50.8 dBFS
    const std::size_t perc = f.add("Percussion", gatedBass(0.3f, 0.0f)); // same low end, no bed
    const std::size_t lead = f.add("Synth", synth(0.7f));
    auto residueAsset = synth(0.02f);                                    // the synth again, 31 dB down
    const std::size_t residue = f.add("Other", residueAsset);
    const std::size_t empty = f.add("Empty", makeSilence(static_cast<std::int64_t>(kSeconds * kRate), kRate));

    double lastProgress = 0.0;
    StemReportOptions options;
    options.progress = [&](double p) { lastProgress = p; };
    const auto result = analyzeStems(f.model, f.pool, options);
    REQUIRE(result.ok());
    const StemReport& report = *result;
    CHECK(lastProgress == 1.0);
    REQUIRE(report.tracks.size() == 5);
    REQUIRE(report.pairs.size() == 10);

    // Levels.
    double shares = 0.0;
    for (const auto& t : report.tracks)
        shares += t.levelShare;
    CHECK(shares == Approx(1.0).margin(1e-9));
    CHECK(report.tracks[empty].silent);
    CHECK(report.tracks[empty].levelShare == 0.0);
    CHECK(report.tracks[lead].levelShare > report.tracks[residue].levelShare * 100.0);

    // Noise bed and activity of the bass.
    const auto& b = report.tracks[bass];
    REQUIRE(b.noiseFloorDbfs.has_value());
    CHECK(*b.noiseFloorDbfs == Approx(-50.8).margin(1.5));
    CHECK(b.activeFraction == Approx(0.5).margin(0.06));
    CHECK(b.lowEndShare > 0.9);
    CHECK(b.bandShare[static_cast<std::size_t>(StemBand::Sub)] > 0.9);
    // Percussion rests in exact digital silence, so it has no bed to report.
    CHECK_FALSE(report.tracks[perc].noiseFloorDbfs.has_value());
    CHECK(report.tracks[perc].silentFraction == Approx(0.5).margin(0.06));

    // Stereo.
    CHECK(report.tracks[lead].stereoCorrelation.value() == Approx(1.0).margin(1e-6));
    CHECK(report.tracks[lead].sideRatio == Approx(0.0).margin(1e-9));

    // Pairs.
    const auto* lowPair = report.pair(bass, perc);
    REQUIRE(lowPair != nullptr);
    CHECK(lowPair->correlation[static_cast<std::size_t>(PairBand::Low)] > 0.9);
    const auto* residuePair = report.pair(residue, lead);
    REQUIRE(residuePair != nullptr);
    CHECK(residuePair->correlation[static_cast<std::size_t>(PairBand::Full)] > 0.99);
    CHECK(std::abs(report.pair(bass, lead)->correlation[static_cast<std::size_t>(PairBand::Full)]) < 0.05);

    // Mix: 0.5 + 0.3 + 0.7 on top of each other clips.
    CHECK(report.mixOvers > 0);
    CHECK(report.mix.truePeakDbtp > 0.0);
    CHECK(report.mix.integratedLufs > -30.0);

    // Advice.
    REQUIRE_FALSE(report.advice.empty());
    CHECK(report.advice.front().code == "mix-clipping");
    CHECK(report.advice.front().severity == AdviceSeverity::Warning);
    CHECK(findAdvice(report, "mix-loudness") != nullptr);
    CHECK(findAdvice(report, "silent", empty) != nullptr);
    CHECK(findAdvice(report, "noise-bed", bass) != nullptr);
    CHECK(findAdvice(report, "noise-bed", perc) == nullptr);
    const auto* shared = findAdvice(report, "shared-low-end");
    REQUIRE(shared != nullptr);
    CHECK(shared->track == perc);       // the stem to high-pass
    CHECK(shared->otherTrack == bass);  // the owner of the low end
    const auto* leftovers = findAdvice(report, "residue", residue);
    REQUIRE(leftovers != nullptr);
    CHECK(leftovers->otherTrack == lead);
    CHECK(findAdvice(report, "mono", lead) != nullptr);
    for (std::size_t i = 1; i < report.advice.size(); ++i)
        CHECK(report.advice[i - 1].severity <= report.advice[i].severity);

    const auto json = toJson(report);
    CHECK(json["format"] == "stem-dawg-stem-report");
    CHECK(json["tracks"].size() == 5);
    CHECK(json["advice"].size() == report.advice.size());
    CHECK(json["tracks"][empty]["noiseFloorDbfs"].is_null());
}

TEST_CASE("Stem report advice carries clean-up fixes; applied, the problems leave the report", "[session][stems][cleanup]")
{
    Fixture f;
    f.model.sampleRate = kRate;
    const std::size_t bass = f.add("Bass", gatedBass(0.5f, 0.005f));
    const std::size_t perc = f.add("Percussion", gatedBass(0.3f, 0.0f));
    f.add("Synth", synth(0.7f));

    auto result = analyzeStems(f.model, f.pool);
    REQUIRE(result.ok());
    const StemReport before = *result;

    const auto* bed = findAdvice(before, "noise-bed", bass);
    REQUIRE(bed != nullptr);
    REQUIRE(bed->fixes.size() == 1);
    const CleanupFix& expand = bed->fixes[0];
    CHECK(expand.track == bass);
    REQUIRE(expand.expander.has_value());
    CHECK(expand.expander->enabled);
    CHECK(expand.expander->thresholdDb == std::round(*before.tracks[bass].noiseFloorDbfs + 10.0));
    CHECK(expand.expander->ratio == Approx(3.4)); // 24 dB range over the 10 dB margin
    CHECK_FALSE(expand.highPass.has_value());

    const auto* shared = findAdvice(before, "shared-low-end");
    REQUIRE(shared != nullptr);
    REQUIRE(shared->fixes.size() == 1);
    CHECK(shared->fixes[0].track == perc);
    REQUIRE(shared->fixes[0].highPass.has_value());
    CHECK(shared->fixes[0].highPass->frequencyHz == kLowEndHz);

    CHECK(findAdvice(before, "mix-clipping")->fixes.empty());
    const auto json = toJson(before);
    bool listed = false;
    for (const auto& a : json["advice"])
        if (a["code"] == "noise-bed")
            listed = a["fixes"][0]["expander"]["thresholdDb"] == expand.expander->thresholdDb && !a["fixes"][0].contains("highPass");
    CHECK(listed);

    // Applying every fix changes the two tracks (a bypassed chain is switched back on).
    f.model.tracks[bass].cleanup.bypassed = true;
    const auto fixes = allCleanupFixes(before);
    CHECK(applyCleanupFixes(f.model, before, fixes) == 2);
    CHECK(f.model.tracks[bass].cleanup.active());
    CHECK(f.model.tracks[bass].cleanup.expander.thresholdDb == expand.expander->thresholdDb);
    CHECK(f.model.tracks[perc].cleanup.highPass.enabled);
    CHECK(f.model.tracks[2].cleanup == TrackCleanup{});
    CHECK(applyCleanupFixes(f.model, before, fixes) == 0); // already applied
    CHECK(describeCleanup(f.model.tracks[perc].cleanup) == "HP 120 Hz 24 dB/oct");

    // Measured again (after the chains), the bed is gone and the low end is no longer doubled.
    result = analyzeStems(f.model, f.pool);
    REQUIRE(result.ok());
    const StemReport& after = *result;
    CHECK(after.tracks[bass].cleanup == f.model.tracks[bass].cleanup);
    CHECK(findAdvice(after, "noise-bed", bass) == nullptr);
    CHECK(findAdvice(after, "shared-low-end") == nullptr);
    CHECK(after.tracks[perc].levelShare < before.tracks[perc].levelShare / 50.0); // 50 Hz under a 120 Hz high-pass
    if (after.tracks[bass].noiseFloorDbfs)
        CHECK(*after.tracks[bass].noiseFloorDbfs < *before.tracks[bass].noiseFloorDbfs - 18.0);
    CHECK(after.tracks[bass].loudness.integratedLufs == Approx(before.tracks[bass].loudness.integratedLufs).margin(0.5));
    CHECK(after.tracks[bass].activeFraction == Approx(before.tracks[bass].activeFraction).margin(0.03)); // cleaning does not make it "play more"
}

TEST_CASE("Stem report covers muted stems; the mix follows mute, fader and master", "[session][stems]")
{
    Fixture f;
    f.model.sampleRate = kRate;
    const std::size_t a = f.add("A", synth(0.3f));
    f.add("B", gatedBass(0.3f, 0.0f));
    const auto full = analyzeStems(f.model, f.pool);
    REQUIRE(full.ok());
    CHECK(findAdvice(*full, "mix-clipping") == nullptr);

    f.model.tracks[a].muted = true;
    const auto muted = analyzeStems(f.model, f.pool);
    REQUIRE(muted.ok());
    CHECK(muted->tracks[a].muted);
    CHECK(muted->tracks[a].loudness.integratedLufs == Approx(full->tracks[a].loudness.integratedLufs).margin(1e-9));
    CHECK(muted->mix.integratedLufs < full->mix.integratedLufs - 0.5);

    f.model.tracks[a].muted = false;
    f.model.tracks[a].gainDb = -6.0;
    const auto faded = analyzeStems(f.model, f.pool);
    REQUIRE(faded.ok());
    CHECK(faded->tracks[a].loudness.integratedLufs == Approx(full->tracks[a].loudness.integratedLufs - 6.0).margin(0.05));

    f.model.tracks[a].gainDb = 0.0;
    f.model.masterGainDb = -10.0;
    const auto quieter = analyzeStems(f.model, f.pool);
    REQUIRE(quieter.ok());
    CHECK(quieter->mix.integratedLufs == Approx(full->mix.integratedLufs - 10.0).margin(0.05));
    CHECK(quieter->tracks[a].loudness.integratedLufs == Approx(full->tracks[a].loudness.integratedLufs).margin(1e-9));
}

TEST_CASE("Stem report refuses empty projects and honours cancellation", "[session][stems]")
{
    Fixture f;
    f.model.sampleRate = kRate;
    CHECK_FALSE(analyzeStems(f.model, f.pool).ok());

    f.add("A", synth(0.3f));
    CancellationSource cancel;
    cancel.cancel();
    const auto result = analyzeStems(f.model, f.pool, {}, cancel.token());
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == ErrorCode::Cancelled);
}

TEST_CASE("Advice rules: a small low-end stem and Suno's Other stem", "[session][stems]")
{
    StemReport report;
    report.mix.integratedLufs = -14.0;
    report.mix.truePeakDbtp = -1.5;
    auto stem = [](const char* name, double lufs, double share, double lowEnd, double centroid) {
        TrackStemReport t;
        t.name = name;
        t.loudness.integratedLufs = lufs;
        t.levelShare = share;
        t.lowEndShare = lowEnd;
        t.centroidHz = centroid;
        return t;
    };
    report.tracks = {stem("Drums", -18.0, 0.60, 0.70, 900.0), stem("Percussion", -30.0, 0.02, 0.75, 1000.0),
                     stem("Other", -27.0, 0.03, 0.05, 2100.0), stem("Brother Synth", -20.0, 0.35, 0.10, 800.0)};
    for (std::size_t a = 0; a < report.tracks.size(); ++a)
        for (std::size_t b = a + 1; b < report.tracks.size(); ++b)
            report.pairs.push_back({a, b, {}});

    const auto advice = adviseStems(report);
    report.advice = advice;
    CHECK(findAdvice(report, "mix-clipping") == nullptr);
    CHECK(findAdvice(report, "mix-ceiling") == nullptr);
    const auto* extra = findAdvice(report, "extra-low-end", 1);
    REQUIRE(extra != nullptr);
    CHECK(extra->otherTrack == 0u);
    CHECK(findAdvice(report, "extra-low-end", 0) == nullptr);
    CHECK(findAdvice(report, "other-stem", 2) != nullptr);
    CHECK(findAdvice(report, "other-stem", 3) == nullptr); // "Brother" is not "Other"

    report.mix.truePeakDbtp = -0.4;
    report.advice = adviseStems(report);
    REQUIRE(findAdvice(report, "mix-ceiling") != nullptr);
    CHECK(findAdvice(report, "mix-ceiling")->severity == AdviceSeverity::Suggestion);
}

namespace {

/// Noise hits every 250 ms that rise linearly over `riseMs` and decay with a
/// 60 ms time constant, over a bed 60 dB down.
std::shared_ptr<AudioAsset> hits(double riseMs, unsigned seed)
{
    const auto frames = static_cast<std::int64_t>(kSeconds * kRate);
    auto asset = AudioAsset::create(2, frames, kRate);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const double rise = riseMs * 0.001 * kRate;
    const auto spacing = static_cast<std::int64_t>(0.25 * kRate);
    for (std::int64_t i = 0; i < frames; ++i) {
        const auto t = static_cast<double>(i % spacing);
        const double envelope = 0.5 * (t < rise ? t / rise : std::exp(-(t - rise) / (0.06 * kRate)));
        for (auto& channel : asset->channels)
            channel[static_cast<std::size_t>(i)] = static_cast<float>(envelope + 0.0005) * noise(rng);
    }
    return asset;
}

} // namespace

TEST_CASE("Stem report measures attack times and suggests the transient shaper for soft drums", "[session][stems][transient]")
{
    Fixture f;
    f.model.sampleRate = kRate;
    const std::size_t drums = f.add("Drums", hits(5.0, 1));
    const std::size_t hats = f.add("Hi Hats", hits(0.0, 2));
    const std::size_t vocal = f.add("Lead Vocals", hits(5.0, 3));

    const auto result = analyzeStems(f.model, f.pool);
    REQUIRE(result.ok());
    const StemReport& before = *result;
    // A 5 ms linear rise measures about 4 ms from 10 % to 90 %; an instant one under 1 ms.
    REQUIRE(before.tracks[drums].attackMs.has_value());
    CHECK(*before.tracks[drums].attackMs > 3.0);
    CHECK(*before.tracks[drums].attackMs < 5.0);
    REQUIRE(before.tracks[hats].attackMs.has_value());
    CHECK(*before.tracks[hats].attackMs < 1.0);
    CHECK(before.tracks[drums].hitsPerMinute == Approx(240.0).margin(12.0));

    // Only the soft drum stem gets the advice: the hats are sharp already, and
    // a vocal's slow consonants are not drums.
    const StemAdvice* soft = findAdvice(before, "soft-attacks", drums);
    REQUIRE(soft != nullptr);
    REQUIRE(soft->fixes.size() == 1);
    REQUIRE(soft->fixes[0].transient.has_value());
    CHECK(soft->fixes[0].transient->enabled);
    CHECK(soft->fixes[0].transient->attackDb > 0.0);
    CHECK(findAdvice(before, "soft-attacks", hats) == nullptr);
    CHECK(findAdvice(before, "soft-attacks", vocal) == nullptr);
    CHECK(describeCleanup(soft->fixes[0].applyTo({})).find("transients") != std::string::npos);

    // Applied, the hits rise faster and the advice is gone. Their level never
    // passes the top they head for; single samples of a noise hit may poke
    // out by about a dB (0.1 dB on a real drum stem).
    CHECK(applyCleanupFixes(f.model, before, soft->fixes) == 1);
    const auto after = analyzeStems(f.model, f.pool);
    REQUIRE(after.ok());
    REQUIRE(after->tracks[drums].attackMs.has_value());
    CHECK(*after->tracks[drums].attackMs < 0.75 * *before.tracks[drums].attackMs);
    CHECK(after->tracks[drums].loudness.samplePeakDbfs < before.tracks[drums].loudness.samplePeakDbfs + 1.5);
    CHECK(findAdvice(*after, "soft-attacks", drums) == nullptr);
}

namespace {

/// Gaussian noise (independent channels) with a steady sine standing `toneDb`
/// above it in one bin of 2048-point square-root-Hann frames, plus an
/// optional low sine (a bass note that dominates the stem's level).
std::shared_ptr<AudioAsset> noiseWithTone(double rms, double toneHz, double toneDb, unsigned seed, double lowAmplitude = 0.0)
{
    const auto frames = static_cast<std::int64_t>(kSeconds * kRate);
    auto asset = AudioAsset::create(2, frames, kRate);
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, static_cast<float>(rms));
    const double n = 2048.0;
    const double tone = toneDb > -90.0 ? rms * std::sqrt(std::pow(10.0, toneDb / 10.0) * std::numbers::pi * std::numbers::pi / (2.0 * n)) : 0.0;
    for (std::int64_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double common = tone * std::sin(2.0 * std::numbers::pi * toneHz * t) + lowAmplitude * std::sin(2.0 * std::numbers::pi * 100.0 * t);
        for (auto& channel : asset->channels)
            channel[static_cast<std::size_t>(i)] = static_cast<float>(common) + noise(rng);
    }
    return asset;
}

} // namespace

TEST_CASE("Stem report finds resonances and generator tones and suggests harshness and shimmer control",
          "[session][stems][resonance]")
{
    Fixture f;
    f.model.sampleRate = kRate;
    const std::size_t pad = f.add("Pad", noiseWithTone(0.05, 3700.0, 14.0, 1));
    const std::size_t hats = f.add("Hats", noiseWithTone(0.05, 16000.0, 14.0, 2));
    const std::size_t plain = f.add("Noise", noiseWithTone(0.05, 3700.0, -99.0, 3));
    // The same kind of peak in a stem that is almost all bass: it adds about
    // 1 % of the mix's energy around 3.7 kHz, where the others mask it.
    const std::size_t bass = f.add("Bass", noiseWithTone(0.005, 3700.0, 14.0, 4, 0.3));

    const auto result = analyzeStems(f.model, f.pool);
    REQUIRE(result.ok());
    const StemReport& before = *result;
    const ResonanceFinding& found = before.tracks[pad].harshness;
    CHECK(found.hz == Approx(3700.0).margin(30.0));
    CHECK(found.excessDb > 10.0);
    CHECK(found.ringingFraction > 0.8);
    CHECK(found.cutDb > 4.0);
    CHECK(before.tracks[hats].shimmer.hz == Approx(16000.0).margin(30.0));
    CHECK(before.tracks[plain].harshness.cutDb < 0.5);
    CHECK(before.tracks[plain].shimmer.cutDb < 0.5);

    const StemAdvice* harsh = findAdvice(before, "harsh-resonance", pad);
    REQUIRE(harsh != nullptr);
    CHECK(harsh->message.find("3.7 kHz") != std::string::npos);
    REQUIRE(harsh->fixes.size() == 1);
    REQUIRE(harsh->fixes[0].resonance.has_value());
    CHECK(harsh->fixes[0].resonance->harshDb == kResonanceProbeDb);
    CHECK(harsh->fixes[0].resonance->shimmerDb == 0.0);
    const StemAdvice* shimmer = findAdvice(before, "shimmer-tones", hats);
    REQUIRE(shimmer != nullptr);
    CHECK(shimmer->message.find("16.0 kHz") != std::string::npos);
    CHECK(findAdvice(before, "harsh-resonance", hats) == nullptr);
    CHECK(findAdvice(before, "harsh-resonance", plain) == nullptr);
    CHECK(findAdvice(before, "shimmer-tones", plain) == nullptr);
    CHECK(before.tracks[bass].harshness.cutDb > 4.0); // measured...
    CHECK(findAdvice(before, "harsh-resonance", bass) == nullptr); // ...but masked in the mix

    // Both fixes on one track merge instead of replacing each other.
    CleanupFix shimmerOnPad = shimmer->fixes[0];
    shimmerOnPad.track = pad;
    const TrackCleanup both = shimmerOnPad.applyTo(harsh->fixes[0].applyTo({}));
    CHECK(both.resonance.enabled);
    CHECK(both.resonance.harshDb == kResonanceProbeDb);
    CHECK(both.resonance.shimmerDb == kResonanceProbeDb);
    CHECK(describeCleanup(both) == "harshness 6 dB, shimmer 6 dB");

    const auto json = toJson(before);
    CHECK(json["tracks"][pad]["harshness"]["hz"].get<double>() == Approx(3700.0).margin(30.0));
    CHECK(json["tracks"][plain]["harshness"].is_object());
    bool listed = false;
    for (const auto& item : json["advice"])
        if (item["code"] == "harsh-resonance")
            listed = item["fixes"][0]["resonance"]["harshDb"] == kResonanceProbeDb;
    CHECK(listed);

    // Applied, the peaks come down by the depth and the advice is gone.
    CHECK(applyCleanupFixes(f.model, before, {harsh->fixes[0], shimmer->fixes[0]}) == 2);
    const auto after = analyzeStems(f.model, f.pool);
    REQUIRE(after.ok());
    CHECK(after->tracks[pad].harshness.excessDb < found.excessDb - 4.0);
    CHECK(after->tracks[hats].shimmer.excessDb < before.tracks[hats].shimmer.excessDb - 4.0);
    CHECK(findAdvice(*after, "harsh-resonance", pad) == nullptr);
    CHECK(findAdvice(*after, "shimmer-tones", hats) == nullptr);
}
