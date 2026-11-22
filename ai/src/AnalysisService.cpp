// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/AnalysisService.h>

#include <daw/core/CancellationToken.h>
#include <daw/core/Logger.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace daw {

const char* toString(JobState state) noexcept
{
    switch (state) {
    case JobState::Queued: return "queued";
    case JobState::Running: return "running";
    case JobState::Succeeded: return "succeeded";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
    }
    return "unknown";
}

namespace {
bool isTerminal(JobState state)
{
    return state == JobState::Succeeded || state == JobState::Failed || state == JobState::Cancelled;
}
} // namespace

struct AnalysisService::Impl {
    struct Job {
        JobId id = 0;
        std::uint64_t sequence = 0;
        JobRequest request;
        JobCallback callback;
        CancellationSource cancel;
        std::atomic<float> progress{0.0f};
        JobState state = JobState::Queued; // guarded by mutex
        std::optional<Error> error;        // guarded by mutex
    };

    AnalysisServiceConfig config;
    InferenceBackend* inference = nullptr;

    mutable std::mutex mutex;
    std::condition_variable workAvailable;
    std::condition_variable jobFinished;
    std::map<std::string, std::shared_ptr<IAnalyzer>> analyzers;
    std::vector<std::shared_ptr<Job>> queue;
    std::map<JobId, std::shared_ptr<Job>> jobs;
    std::deque<JobId> finishedOrder;
    JobId nextId = 1;
    std::uint64_t nextSequence = 0;
    int active = 0;
    bool stopping = false;
    std::vector<std::thread> workers;

    JobStatus statusOf(const Job& job) const
    {
        JobStatus s;
        s.id = job.id;
        s.state = job.state;
        s.progress = job.progress.load(std::memory_order_relaxed);
        s.analyzerId = job.request.analyzerId;
        s.tag = job.request.tag;
        s.error = job.error;
        return s;
    }

    std::shared_ptr<Job> popNext()
    {
        auto best = std::max_element(queue.begin(), queue.end(), [](const auto& a, const auto& b) {
            if (a->request.priority != b->request.priority)
                return a->request.priority < b->request.priority;
            return a->sequence > b->sequence; // earlier sequence wins
        });
        auto job = *best;
        queue.erase(best);
        return job;
    }

    void retire(JobId id)
    {
        finishedOrder.push_back(id);
        while (finishedOrder.size() > config.retainedFinishedJobs) {
            jobs.erase(finishedOrder.front());
            finishedOrder.pop_front();
        }
    }

    // Runs the callback (unlocked), then publishes the terminal state.
    void finish(std::unique_lock<std::mutex>& lock, const std::shared_ptr<Job>& job, JobState state,
                std::optional<Error> error, std::optional<AnalysisResult> result)
    {
        JobOutcome outcome;
        outcome.status = statusOf(*job);
        outcome.status.state = state;
        outcome.status.error = error;
        if (state == JobState::Succeeded)
            outcome.status.progress = 1.0f;
        outcome.result = std::move(result);

        lock.unlock();
        if (job->callback) {
            try {
                job->callback(outcome);
            } catch (const std::exception& e) {
                logging::error("ai", "job completion callback threw", {{"job", job->id}, {"error", e.what()}});
            }
        }
        lock.lock();
        job->state = state;
        job->error = std::move(error);
        if (state == JobState::Succeeded)
            job->progress.store(1.0f);
        retire(job->id);
        jobFinished.notify_all();
    }

    void workerLoop(int index)
    {
#ifdef _WIN32
        if (config.belowNormalPriority)
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const std::wstring name = L"daw-ai-worker-" + std::to_wstring(index);
        ::SetThreadDescription(::GetCurrentThread(), name.c_str());
#else
        (void)index;
#endif
        std::unique_lock lock(mutex);
        for (;;) {
            workAvailable.wait(lock, [this] { return stopping || !queue.empty(); });
            if (queue.empty()) {
                if (stopping)
                    return;
                continue;
            }
            auto job = popNext();
            if (job->cancel.isCancelled()) {
                finish(lock, job, JobState::Cancelled, makeError(ErrorCode::Cancelled, "cancelled before start"), std::nullopt);
                continue;
            }
            const auto analyzerIt = analyzers.find(job->request.analyzerId);
            if (analyzerIt == analyzers.end()) {
                finish(lock, job, JobState::Failed, makeError(ErrorCode::NotFound, "unknown analyzer"), std::nullopt);
                continue;
            }
            std::shared_ptr<IAnalyzer> analyzer = analyzerIt->second;
            job->state = JobState::Running;
            ++active;
            lock.unlock();

            AnalysisContext context;
            context.cancel = job->cancel.token();
            context.progress = [job](float p) { job->progress.store(std::clamp(p, 0.0f, 1.0f), std::memory_order_relaxed); };
            context.inference = inference;

            const auto start = std::chrono::steady_clock::now();
            Result<AnalysisResult> result = makeError(ErrorCode::Unknown, "analyzer did not run");
            try {
                result = analyzer->analyze(*job->request.audio, job->request.params, context);
            } catch (const std::exception& e) {
                result = makeError(ErrorCode::Unknown, std::string("analyzer threw: ") + e.what());
            }
            const double elapsed =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

            lock.lock();
            --active;
            if (result) {
                AnalysisResult value = std::move(result).value();
                value.analyzerId = analyzer->id();
                value.analyzerVersion = analyzer->version();
                value.elapsedMs = elapsed;
                finish(lock, job, JobState::Succeeded, std::nullopt, std::move(value));
            } else if (result.error().code == ErrorCode::Cancelled || job->cancel.isCancelled()) {
                finish(lock, job, JobState::Cancelled, result.error(), std::nullopt);
            } else {
                logging::warn("ai", "analysis failed",
                              {{"job", job->id}, {"analyzer", job->request.analyzerId}, {"error", result.error().describe()}});
                finish(lock, job, JobState::Failed, result.error(), std::nullopt);
            }
        }
    }
};

AnalysisService::AnalysisService(AnalysisServiceConfig config, std::shared_ptr<InferenceBackend> inference)
    : impl_(std::make_unique<Impl>()), inference_(std::move(inference))
{
    impl_->config = config;
    impl_->inference = inference_.get();
    int count = config.workerCount;
    if (count <= 0)
        count = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) / 4, 1, 4);
    for (int i = 0; i < count; ++i)
        impl_->workers.emplace_back([this, i] { impl_->workerLoop(i); });
}

AnalysisService::~AnalysisService()
{
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        for (auto& [id, job] : impl_->jobs)
            job->cancel.cancel();
    }
    impl_->workAvailable.notify_all();
    for (auto& worker : impl_->workers)
        worker.join();
}

Status AnalysisService::registerAnalyzer(std::shared_ptr<IAnalyzer> analyzer)
{
    if (!analyzer)
        return makeError(ErrorCode::InvalidArgument, "null analyzer");
    std::lock_guard lock(impl_->mutex);
    const std::string id = analyzer->id();
    if (impl_->analyzers.contains(id))
        return makeError(ErrorCode::AlreadyExists, "analyzer already registered: " + id);
    impl_->analyzers.emplace(id, std::move(analyzer));
    return Status::success();
}

std::vector<std::string> AnalysisService::analyzerIds() const
{
    std::lock_guard lock(impl_->mutex);
    std::vector<std::string> ids;
    for (const auto& [id, analyzer] : impl_->analyzers)
        ids.push_back(id);
    return ids;
}

Result<JobId> AnalysisService::submit(JobRequest request, JobCallback onComplete)
{
    if (!request.audio)
        return makeError(ErrorCode::InvalidArgument, "analysis job has no audio");
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping)
        return makeError(ErrorCode::Unavailable, "analysis service is shutting down");
    if (!impl_->analyzers.contains(request.analyzerId))
        return makeError(ErrorCode::NotFound, "unknown analyzer: " + request.analyzerId);

    if (!request.params.is_object())
        request.params = nlohmann::json::object(); // analyzers may call params.value(...)

    auto job = std::make_shared<Impl::Job>();
    job->id = impl_->nextId++;
    job->sequence = impl_->nextSequence++;
    job->request = std::move(request);
    job->callback = std::move(onComplete);
    impl_->jobs.emplace(job->id, job);
    impl_->queue.push_back(job);
    impl_->workAvailable.notify_one();
    return job->id;
}

bool AnalysisService::cancel(JobId id)
{
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->jobs.find(id);
    if (it == impl_->jobs.end() || isTerminal(it->second->state))
        return false;
    it->second->cancel.cancel();
    return true;
}

void AnalysisService::cancelAll()
{
    std::lock_guard lock(impl_->mutex);
    for (auto& [id, job] : impl_->jobs)
        if (!isTerminal(job->state))
            job->cancel.cancel();
}

std::optional<JobStatus> AnalysisService::status(JobId id) const
{
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->jobs.find(id);
    if (it == impl_->jobs.end())
        return std::nullopt;
    return impl_->statusOf(*it->second);
}

bool AnalysisService::wait(JobId id, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(impl_->mutex);
    return impl_->jobFinished.wait_for(lock, timeout, [&] {
        const auto it = impl_->jobs.find(id);
        return it == impl_->jobs.end() || isTerminal(it->second->state);
    });
}

bool AnalysisService::waitIdle(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(impl_->mutex);
    return impl_->jobFinished.wait_for(lock, timeout, [&] {
        if (!impl_->queue.empty() || impl_->active > 0)
            return false;
        return std::all_of(impl_->jobs.begin(), impl_->jobs.end(),
                           [](const auto& entry) { return isTerminal(entry.second->state); });
    });
}

int AnalysisService::workerCount() const noexcept
{
    return static_cast<int>(impl_->workers.size());
}

} // namespace daw
