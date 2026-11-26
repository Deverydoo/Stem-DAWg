// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// The single translation unit that compiles TinySoundFont (MIT,
// https://github.com/schellingb/TinySoundFont). Third-party code: its
// warnings are not ours. MIDI files are parsed by MidiFile.cpp.

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif

#define TSF_IMPLEMENTATION
#include <tsf.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif
