// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Marks the real-time audio thread so non-RT-safe facilities (logging, debug
// assertions) can detect and refuse misuse instead of blocking audio.

namespace daw::rt {

bool isAudioThread() noexcept;

class ScopedAudioThread {
public:
    ScopedAudioThread() noexcept;
    ~ScopedAudioThread();

    ScopedAudioThread(const ScopedAudioThread&) = delete;
    ScopedAudioThread& operator=(const ScopedAudioThread&) = delete;

private:
    bool previous_;
};

} // namespace daw::rt
