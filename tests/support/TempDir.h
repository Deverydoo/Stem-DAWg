// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <filesystem>
#include <string>

namespace daw::test {

/// Unique temporary directory, removed recursively on destruction.
class TempDir {
public:
    explicit TempDir(const std::string& label = "daw");
    ~TempDir();

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    std::filesystem::path operator/(const std::filesystem::path& child) const { return path_ / child; }

private:
    std::filesystem::path path_;
};

} // namespace daw::test
