// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "TempDir.h"

#include <daw/core/Files.h>
#include <daw/core/Ids.h>

namespace daw::test {

TempDir::TempDir(const std::string& label)
    : path_(std::filesystem::temp_directory_path() / "stem-dawg-tests" / utf8ToPath(label + "-" + generateUuid()))
{
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir()
{
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

} // namespace daw::test
