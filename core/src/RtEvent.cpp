// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/RtEvent.h>

namespace daw {

const char* toString(RtEventCode code) noexcept
{
    switch (code) {
    case RtEventCode::None: return "none";
    case RtEventCode::DeadlineMiss: return "deadline-miss";
    case RtEventCode::CallbackGap: return "callback-gap";
    case RtEventCode::SampleRateMismatch: return "sample-rate-mismatch";
    case RtEventCode::SnapshotAdopted: return "snapshot-adopted";
    case RtEventCode::TransportStoppedAtEnd: return "transport-stopped-at-end";
    case RtEventCode::CommandQueueOverflow: return "command-queue-overflow";
    }
    return "unknown";
}

} // namespace daw
