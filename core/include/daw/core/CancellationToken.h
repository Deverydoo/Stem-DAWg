// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Cooperative cancellation for background work (decoding, peak building,
// resampling, AI analysis). Long-running loops poll isCancelled() and return
// ErrorCode::Cancelled.

#include <atomic>
#include <memory>

namespace daw {

class CancellationToken {
public:
    /// A default token is never cancelled.
    CancellationToken() = default;

    bool isCancelled() const noexcept { return flag_ && flag_->load(std::memory_order_relaxed); }

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {}

    std::shared_ptr<std::atomic<bool>> flag_;
};

class CancellationSource {
public:
    CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() noexcept { flag_->store(true, std::memory_order_relaxed); }
    bool isCancelled() const noexcept { return flag_->load(std::memory_order_relaxed); }
    CancellationToken token() const { return CancellationToken(flag_); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

} // namespace daw
