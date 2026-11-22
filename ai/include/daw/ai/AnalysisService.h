// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Asynchronous analysis job system: priority queue, worker pool (below-normal
// OS priority so it never competes with the audio thread), progress reporting,
// cooperative cancellation and completion callbacks.
//
// Completion callbacks run on the worker thread BEFORE the job's state turns
// terminal; wait()/waitIdle() therefore return only after callbacks finished.
// UI code must marshal callbacks to its own thread (Qt: queued invokeMethod).

#include <daw/ai/Analyzer.h>
#include <daw/ai/InferenceBackend.h>
#include <daw/core/AudioAsset.h>
#include <daw/core/Result.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace daw {

using JobId = std::uint64_t;

enum class JobState { Queued, Running, Succeeded, Failed, Cancelled };

const char* toString(JobState state) noexcept;

struct JobRequest {
    std::string analyzerId;
    AudioAssetPtr audio;
    nlohmann::json params = nlohmann::json::object();
    std::string tag;   // caller correlation, e.g. media id
    int priority = 0;  // higher runs first; FIFO within a priority
};

struct JobStatus {
    JobId id = 0;
    JobState state = JobState::Queued;
    float progress = 0.0f;
    std::string analyzerId;
    std::string tag;
    std::optional<Error> error;
};

struct JobOutcome {
    JobStatus status;
    std::optional<AnalysisResult> result;
};

using JobCallback = std::function<void(const JobOutcome&)>;

struct AnalysisServiceConfig {
    int workerCount = 0;              // 0 = clamp(hardware_concurrency / 4, 1, 4)
    bool belowNormalPriority = true;
    std::size_t retainedFinishedJobs = 512;
};

class AnalysisService {
public:
    explicit AnalysisService(AnalysisServiceConfig config = {}, std::shared_ptr<InferenceBackend> inference = nullptr);
    /// Cancels outstanding jobs and joins workers.
    ~AnalysisService();

    AnalysisService(const AnalysisService&) = delete;
    AnalysisService& operator=(const AnalysisService&) = delete;

    Status registerAnalyzer(std::shared_ptr<IAnalyzer> analyzer);
    std::vector<std::string> analyzerIds() const;

    Result<JobId> submit(JobRequest request, JobCallback onComplete = {});
    /// Requests cancellation. Queued jobs are cancelled immediately; running
    /// jobs stop at their next cancellation check. Returns false if unknown/finished.
    bool cancel(JobId id);
    void cancelAll();

    std::optional<JobStatus> status(JobId id) const;
    /// Blocks until the job is terminal (true) or the timeout expires (false).
    bool wait(JobId id, std::chrono::milliseconds timeout);
    /// Blocks until no job is queued or running.
    bool waitIdle(std::chrono::milliseconds timeout);

    InferenceBackend* inference() const noexcept { return inference_.get(); }
    int workerCount() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<InferenceBackend> inference_;
};

} // namespace daw
