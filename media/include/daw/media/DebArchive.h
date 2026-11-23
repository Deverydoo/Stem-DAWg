// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Extracts one file from a Debian package (.deb: an ar archive whose
// data.tar.xz holds the files). Used to install the FluidR3_GM SoundFont from
// Debian's package without external tools. Streams: memory use stays small
// whatever the file size.

#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

#include <filesystem>
#include <functional>
#include <string_view>

namespace daw {

/// Writes the member of data.tar.xz whose path ends with `memberPath`
/// (e.g. "usr/share/sounds/sf2/FluidR3_GM.sf2") to `output`, atomically.
/// `progress` receives the fraction of the package read so far.
Status extractDebMember(const std::filesystem::path& deb, std::string_view memberPath, const std::filesystem::path& output,
                        const CancellationToken& cancel = {}, const std::function<void(double)>& progress = {});

} // namespace daw
