// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Plain-data events emitted by the audio thread through an SpscQueue and
// drained/logged on the control thread. The audio thread never logs directly.

#include <cstdint>

namespace daw {

enum class RtEventCode : std::uint16_t {
    None,
    DeadlineMiss,          // a = elapsed microseconds, b = budget microseconds
    CallbackGap,           // a = gap microseconds, b = expected microseconds
    SampleRateMismatch,    // a = snapshot rate (Hz), b = engine rate (Hz)
    SnapshotAdopted,       // a = snapshot version
    TransportStoppedAtEnd, // a = playhead
    CommandQueueOverflow,
};

struct RtEvent {
    RtEventCode code = RtEventCode::None;
    std::int64_t a = 0;
    std::int64_t b = 0;
};

const char* toString(RtEventCode code) noexcept;

} // namespace daw
