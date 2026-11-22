// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/AnalysisService.h>
#include <daw/ai/Analyzers.h>

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <mutex>
#include <thread>

using namespace daw;
using namespace daw::test;
using namespace std::chrono_literals;

namespace {

// Spins until cancelled, reporting progress; used to test cancellation.
class SlowAnalyzer final : public IAnalyzer {
public:
    std::atomic<bool> started{false};
    std::string id() const override { return "test.slow"; }
    int version() const override { return 1; }
    std::string displayName() const override { return "slow"; }
    Result<AnalysisResult> analyze(const AudioAsset&, const nlohmann::json&, AnalysisContext& context) override
    {
        started = true;
        for (int i = 0; i < 10000; ++i) {
            if (context.cancel.isCancelled())
                return makeError(ErrorCode::Cancelled, "stopped");
            context.progress(static_cast<float>(i) / 10000.0f);
            std::this_thread::sleep_for(1ms);
        }
        return AnalysisResult{};
    }
};

class ThrowingAnalyzer final : public IAnalyzer {
public:
    std::string id() const override { return "test.throws"; }
    int version() const override { return 1; }
    std::string displayName() const override { return "throws"; }
    Result<AnalysisResult> analyze(const AudioAsset&, const nlohmann::json&, AnalysisContext&) override
    {
        throw std::runtime_error("boom");
    }
};

} // namespace

TEST_CASE("Analysis jobs complete with results and callbacks", "[ai][service]")
{
    AnalysisService service(AnalysisServiceConfig{2});
    REQUIRE(registerBuiltinAnalyzers(service).ok());
    REQUIRE(service.analyzerIds().size() == 4);

    std::mutex mutex;
    std::vector<JobOutcome> outcomes;
    const auto audio = makeSine(440.0, 1.0, 48000.0);
    std::vector<JobId> ids;
    for (const char* analyzer : {kLevelAnalyzerId, kTransientAnalyzerId, kSpectralAnalyzerId}) {
        auto id = service.submit({analyzer, audio, {}, "media-1"}, [&](const JobOutcome& outcome) {
            std::lock_guard lock(mutex);
            outcomes.push_back(outcome);
        });
        REQUIRE(id.ok());
        ids.push_back(*id);
    }
    REQUIRE(service.waitIdle(30s));

    REQUIRE(outcomes.size() == 3);
    for (const auto& outcome : outcomes) {
        REQUIRE(outcome.status.state == JobState::Succeeded);
        REQUIRE(outcome.status.tag == "media-1");
        REQUIRE(outcome.result.has_value());
        REQUIRE(outcome.result->analyzerVersion == 1);
        REQUIRE(outcome.result->analyzerId == outcome.status.analyzerId);
    }
    for (JobId id : ids) {
        const auto status = service.status(id);
        REQUIRE(status->state == JobState::Succeeded);
        REQUIRE(status->progress == 1.0f);
    }
}

TEST_CASE("Running and queued jobs can be cancelled", "[ai][service]")
{
    AnalysisService service(AnalysisServiceConfig{1});
    auto slow = std::make_shared<SlowAnalyzer>();
    REQUIRE(service.registerAnalyzer(slow).ok());
    const auto audio = makeSilence(10, 48000.0, 1);

    const JobId running = *service.submit({"test.slow", audio});
    const JobId queued = *service.submit({"test.slow", audio});
    while (!slow->started)
        std::this_thread::yield();

    REQUIRE(service.cancel(queued));
    REQUIRE(service.cancel(running));
    REQUIRE(service.wait(running, 10s));
    REQUIRE(service.wait(queued, 10s));
    REQUIRE(service.status(running)->state == JobState::Cancelled);
    REQUIRE(service.status(queued)->state == JobState::Cancelled);
    REQUIRE_FALSE(service.cancel(running)); // already finished
}

TEST_CASE("Analyzer failures and bad requests are reported", "[ai][service]")
{
    AnalysisService service(AnalysisServiceConfig{1});
    REQUIRE(service.registerAnalyzer(std::make_shared<ThrowingAnalyzer>()).ok());
    REQUIRE(service.registerAnalyzer(std::make_shared<ThrowingAnalyzer>()).error().code == ErrorCode::AlreadyExists);

    const auto audio = makeSilence(10, 48000.0, 1);
    const JobId id = *service.submit({"test.throws", audio});
    REQUIRE(service.wait(id, 10s));
    const auto status = service.status(id);
    REQUIRE(status->state == JobState::Failed);
    REQUIRE(status->error->message.find("boom") != std::string::npos);

    REQUIRE(service.submit({"no.such.analyzer", audio}).error().code == ErrorCode::NotFound);
    REQUIRE(service.submit({"test.throws", nullptr}).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("Higher-priority jobs run first", "[ai][service]")
{
    AnalysisService service(AnalysisServiceConfig{1});
    auto slow = std::make_shared<SlowAnalyzer>();
    REQUIRE(service.registerAnalyzer(slow).ok());
    REQUIRE(registerBuiltinAnalyzers(service).ok());
    const auto audio = makeSilence(4800, 48000.0, 1);

    const JobId blocker = *service.submit({"test.slow", audio});
    while (!slow->started)
        std::this_thread::yield();

    std::mutex mutex;
    std::vector<std::string> order;
    auto record = [&](const JobOutcome& o) {
        std::lock_guard lock(mutex);
        order.push_back(o.status.tag);
    };
    REQUIRE(service.submit({kLevelAnalyzerId, audio, {}, "low", 0}, record).ok());
    REQUIRE(service.submit({kLevelAnalyzerId, audio, {}, "high", 10}, record).ok());
    service.cancel(blocker);
    REQUIRE(service.waitIdle(10s));
    REQUIRE(order == std::vector<std::string>{"high", "low"});
}

TEST_CASE("Destroying the service with outstanding work does not hang", "[ai][service]")
{
    auto slow = std::make_shared<SlowAnalyzer>();
    const auto start = std::chrono::steady_clock::now();
    {
        AnalysisService service(AnalysisServiceConfig{2});
        REQUIRE(service.registerAnalyzer(slow).ok());
        for (int i = 0; i < 5; ++i)
            REQUIRE(service.submit({"test.slow", makeSilence(10, 48000.0, 1)}).ok());
        while (!slow->started)
            std::this_thread::yield();
    }
    REQUIRE(std::chrono::steady_clock::now() - start < 5s);
}
