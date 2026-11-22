// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Anchor translation unit for the daw_juce static library. The JUCE module
// sources themselves are injected by the juce::juce_* interface targets.
#include <juce_core/juce_core.h>

namespace daw::detail {

const char* juceVersionString()
{
    static const std::string version = juce::SystemStats::getJUCEVersion().toStdString();
    return version.c_str();
}

} // namespace daw::detail
