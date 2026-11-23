// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Extracts named files from a gzip-compressed tar archive (.tar.gz). Used to
// install the DeepFilterNet3 voice model, which is published as one. Meant for
// small archives: it decompresses in memory, up to a size limit.

#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace daw {

/// Writes each regular file of the archive whose file name (the last path
/// component) is in `fileNames` to `outputDir/<file name>`. All or nothing:
/// fails, leaving no new files, unless every name is found. Refuses archives
/// that decompress to more than `maxBytes`.
Status extractTarGzFiles(const std::filesystem::path& archive, const std::vector<std::string>& fileNames,
                         const std::filesystem::path& outputDir, std::size_t maxBytes = std::size_t{256} << 20,
                         const CancellationToken& cancel = {});

} // namespace daw
