// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/engine/AudioDeviceService.h>

#include <juce_events/juce_events.h>

namespace daw::detail {
const char* juceVersionString();
}

namespace daw {

struct ScopedAudioRuntime::Impl {
    juce::ScopedJuceInitialiser_GUI juceInit;
};

ScopedAudioRuntime::ScopedAudioRuntime() : impl_(std::make_unique<Impl>()) {}
ScopedAudioRuntime::~ScopedAudioRuntime() = default;

std::string ScopedAudioRuntime::juceVersion()
{
    return detail::juceVersionString();
}

} // namespace daw
