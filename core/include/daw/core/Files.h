// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Filesystem helpers. All persisted paths are UTF-8; on Windows paths are
// converted through std::filesystem's wide representation so non-ASCII project
// locations work.

#include <daw/core/Result.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace daw {

std::string pathToUtf8(const std::filesystem::path& path);
/// UTF-8 with forward slashes (portable form stored in project files).
std::string pathToGenericUtf8(const std::filesystem::path& path);
std::filesystem::path utf8ToPath(std::string_view utf8);

Result<std::string> readFileBytes(const std::filesystem::path& path);

/// Writes `bytes` to a temporary sibling, flushes it to disk and atomically
/// replaces `path`. Readers see either the old or the new file, never a torn one.
Status writeFileAtomically(const std::filesystem::path& path, std::string_view bytes);

/// %LOCALAPPDATA%\StemDAWg (created on demand).
std::filesystem::path appDataDirectory();

/// "2026-11-21T21:30:00.123Z"
std::string utcTimestampIso8601();
/// "20261121-213000-123" (UTC, filename safe)
std::string utcTimestampForFilename();

} // namespace daw
