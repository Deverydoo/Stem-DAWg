// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/Analyzers.h>

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace daw;
using namespace daw::test;
using Catch::Matchers::WithinAbs;

namespace {
Result<AnalysisResult> run(IAnalyzer& analyzer, const AudioAsset& audio, nlohmann::json params = nlohmann::json::object())
{
    AnalysisContext context;
    context.progress = [](float) {};
    return analyzer.analyze(audio, params, context);
}
} // namespace

TEST_CASE("Level analyzer measures a full-scale sine", "[ai][levels]")
{
    auto analyzer = createLevelAnalyzer();
    const auto result = run(*analyzer, *makeSine(1000.0, 1.0, 48000.0, 2, 1.0f));
    REQUIRE(result.ok());
    const auto& ch = result->data["channels"][0];
    REQUIRE_THAT(ch["peakDbfs"].get<double>(), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(ch["rmsDbfs"].get<double>(), WithinAbs(-3.0103, 0.01));
    REQUIRE_THAT(ch["crestDb"].get<double>(), WithinAbs(3.0103, 0.02));
    REQUIRE_THAT(ch["dcOffset"].get<double>(), WithinAbs(0.0, 1e-4));
    REQUIRE(ch["clippedSamples"].get<std::int64_t>() > 0); // the sine reaches +/-1.0
    REQUIRE_THAT(result->data["overall"]["stereoCorrelation"].get<double>(), WithinAbs(1.0, 1e-9));
}

TEST_CASE("Level analyzer detects inverted channels and silence", "[ai][levels]")
{
    auto analyzer = createLevelAnalyzer();
    auto inverted = makeSine(200.0, 0.5, 48000.0, 2, 0.5f);
    for (auto& s : inverted->channels[1])
        s = -s;
    const auto result = run(*analyzer, *inverted);
    REQUIRE_THAT(result->data["overall"]["stereoCorrelation"].get<double>(), WithinAbs(-1.0, 1e-9));
    REQUIRE(result->data["overall"]["clippedSamples"] == 0);

    const auto silent = run(*analyzer, *makeSilence(1000, 48000.0, 1));
    REQUIRE(silent.ok());
    REQUIRE(silent->data["channels"][0]["peakDbfs"] == -200.0);
    REQUIRE(silent->data["overall"]["stereoCorrelation"].is_null()); // mono
}

TEST_CASE("Transient analyzer finds isolated hits at the right positions", "[ai][transients]")
{
    auto analyzer = createTransientAnalyzer();
    const std::vector<double> onsets{0.5, 1.0, 1.75, 2.5};
    const double rate = 44100.0;
    const auto result = run(*analyzer, *makeClickTrack(3.0, rate, onsets));
    REQUIRE(result.ok());
    const auto frames = onsetFrames(result->data);
    REQUIRE(frames.size() == onsets.size());
    for (std::size_t i = 0; i < onsets.size(); ++i) {
        const double errorMs = std::abs(static_cast<double>(frames[i]) / rate - onsets[i]) * 1000.0;
        INFO("onset " << i << " error " << errorMs << " ms");
        REQUIRE(errorMs < 5.0);
    }
}

TEST_CASE("Transient analyzer finds every kick over a sustained drone (Suno-style bleed)", "[ai][transients][regression]")
{
    auto analyzer = createTransientAnalyzer();
    const double rate = 44100.0;
    const auto result = run(*analyzer, *makeDroneWithKicks(12.0, rate, 0.5));
    REQUIRE(result.ok());
    INFO(result->data["onsets"].dump());
    const auto frames = onsetFrames(result->data);
    REQUIRE(frames.size() == 24);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const double errorMs = std::abs(static_cast<double>(frames[i]) / rate - 0.5 * static_cast<double>(i)) * 1000.0;
        INFO("hit " << i << " error " << errorMs << " ms");
        REQUIRE(errorMs < 3.0);
    }
}

TEST_CASE("Transient analyzer ignores silence and steady tones", "[ai][transients]")
{
    auto analyzer = createTransientAnalyzer();
    REQUIRE(onsetFrames(run(*analyzer, *makeSilence(88200, 44100.0, 2))->data).empty());
    const auto tone = run(*analyzer, *makeSine(440.0, 2.0, 44100.0, 1, 0.5f));
    INFO(tone->data["onsets"].dump());
    REQUIRE(onsetFrames(tone->data).size() <= 1);
    REQUIRE(run(*analyzer, *makeSilence(1000, 44100.0, 1), {{"frameSize", 1000}}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Spectral analyzer locates energy in the right band", "[ai][spectrum]")
{
    auto analyzer = createSpectralAnalyzer();
    auto bandFraction = [](const nlohmann::json& data, const std::string& name) {
        for (const auto& band : data["bands"])
            if (band["name"] == name)
                return band["fraction"].get<double>();
        return -1.0;
    };

    const auto bass = run(*analyzer, *makeSine(100.0, 1.0, 48000.0, 1));
    REQUIRE(bass.ok());
    REQUIRE(bandFraction(bass->data, "bass") > 0.95);
    REQUIRE(bass->data["harshnessRatio"].get<double>() < 0.01);

    const auto harsh = run(*analyzer, *makeSine(3000.0, 1.0, 48000.0, 1));
    REQUIRE(bandFraction(harsh->data, "upperMid") > 0.95);
    REQUIRE(harsh->data["harshnessRatio"].get<double>() > 0.95);
    REQUIRE_THAT(harsh->data["centroidHz"].get<double>(), WithinAbs(3000.0, 50.0));
}

TEST_CASE("Analyzers stop when cancelled", "[ai][cancel]")
{
    CancellationSource cancel;
    cancel.cancel();
    AnalysisContext context;
    context.cancel = cancel.token();
    context.progress = [](float) {};
    const auto audio = makeNoise(480000, 48000.0, 2, 0.5f);
    for (auto analyzer : {createLevelAnalyzer(), createTransientAnalyzer(), createSpectralAnalyzer()})
        REQUIRE(analyzer->analyze(*audio, nlohmann::json::object(), context).error().code == ErrorCode::Cancelled);
}
