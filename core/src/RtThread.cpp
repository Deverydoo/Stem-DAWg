// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/RtThread.h>

namespace daw::rt {

namespace {
thread_local bool tlsAudioThread = false;
}

bool isAudioThread() noexcept
{
    return tlsAudioThread;
}

ScopedAudioThread::ScopedAudioThread() noexcept : previous_(tlsAudioThread)
{
    tlsAudioThread = true;
}

ScopedAudioThread::~ScopedAudioThread()
{
    tlsAudioThread = previous_;
}

} // namespace daw::rt
