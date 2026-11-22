// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Transport state machine. Owned by the audio thread while the device runs;
// control threads talk to it only through TransportCommands queued by the
// AudioEngine. Positions are integer sample frames at the engine sample rate:
// no floating-point accumulation, so there is no drift over long sessions.

#include <algorithm>
#include <cstdint>

namespace daw {

enum class TransportCommandType : std::uint8_t {
    Play,
    Stop,       // pause at the current position
    Seek,       // value = absolute frame (clamped to >= 0)
    SetStopAtEnd, // value != 0 -> stop automatically at the end of content
};

struct TransportCommand {
    TransportCommandType type = TransportCommandType::Stop;
    std::int64_t value = 0;
};

struct TransportState {
    std::int64_t playhead = 0;
    bool playing = false;

    friend bool operator==(const TransportState&, const TransportState&) = default;
};

class Transport {
public:
    void apply(const TransportCommand& command) noexcept
    {
        switch (command.type) {
        case TransportCommandType::Play: playing_ = true; break;
        case TransportCommandType::Stop: playing_ = false; break;
        case TransportCommandType::Seek: playhead_ = std::max<std::int64_t>(0, command.value); break;
        case TransportCommandType::SetStopAtEnd: stopAtEnd_ = command.value != 0; break;
        }
    }

    /// Advances the playhead by `frames` if playing.
    void advance(std::int64_t frames) noexcept
    {
        if (playing_)
            playhead_ += frames;
    }

    void stopAt(std::int64_t frame) noexcept
    {
        playing_ = false;
        playhead_ = std::max<std::int64_t>(0, frame);
    }

    std::int64_t playhead() const noexcept { return playhead_; }
    bool playing() const noexcept { return playing_; }
    bool stopAtEnd() const noexcept { return stopAtEnd_; }
    TransportState state() const noexcept { return {playhead_, playing_}; }

private:
    std::int64_t playhead_ = 0;
    bool playing_ = false;
    bool stopAtEnd_ = true;
};

} // namespace daw
