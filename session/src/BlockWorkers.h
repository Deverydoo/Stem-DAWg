// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// A small persistent thread team for block-synchronous offline work: run()
// executes task(0..count-1) spread over the team and the calling thread and
// returns when all are done. Used where every block needs all of its tasks
// finished before the next step (the stem report's per-stem measurements,
// which are then compared pairwise). Workers run below normal priority so
// they never compete with the audio thread.

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace daw::detail {

class BlockWorkers {
public:
    explicit BlockWorkers(std::size_t maxTasks)
    {
        const std::size_t hardware = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t threads = std::min(maxTasks, hardware > 2 ? hardware - 2 : 1) - (maxTasks > 0 ? 1 : 0);
        for (std::size_t i = 0; i < threads; ++i)
            threads_.emplace_back([this] { loop(); });
    }

    ~BlockWorkers()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        for (std::thread& t : threads_)
            t.join();
    }

    BlockWorkers(const BlockWorkers&) = delete;
    BlockWorkers& operator=(const BlockWorkers&) = delete;

    void run(std::size_t count, const std::function<void(std::size_t)>& task)
    {
        {
            std::lock_guard lock(mutex_);
            task_ = &task;
            count_ = count;
            next_.store(0);
            busy_ = threads_.size();
            ++generation_;
        }
        wake_.notify_all();
        work();
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return busy_ == 0; });
        task_ = nullptr;
    }

private:
    void work()
    {
        for (std::size_t i = next_.fetch_add(1); i < count_; i = next_.fetch_add(1))
            (*task_)(i);
    }

    void loop()
    {
#ifdef _WIN32
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        ::SetThreadDescription(::GetCurrentThread(), L"daw-block-worker");
#endif
        std::uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || generation_ != seen; });
                if (stopping_)
                    return;
                seen = generation_;
            }
            work();
            {
                std::lock_guard lock(mutex_);
                --busy_;
            }
            done_.notify_one();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(std::size_t)>* task_ = nullptr;
    std::size_t count_ = 0;
    std::atomic<std::size_t> next_{0};
    std::size_t busy_ = 0;
    std::uint64_t generation_ = 0;
    bool stopping_ = false;
};

} // namespace daw::detail
