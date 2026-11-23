// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Field parsing shared by the archive readers (ar and tar headers).

#include <cstddef>
#include <cstdint>
#include <string>

namespace daw::archive {

/// A fixed-width header field without its trailing spaces, NULs and slashes.
inline std::string trimField(const char* data, std::size_t size)
{
    std::string text(data, size);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\0' || text.back() == '/'))
        text.pop_back();
    return text;
}

/// An octal number field (tar sizes), stopping at the first space or NUL.
inline std::uint64_t parseOctal(const char* data, std::size_t size)
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size && data[i] != '\0' && data[i] != ' '; ++i) {
        if (data[i] < '0' || data[i] > '7')
            break;
        value = value * 8 + static_cast<std::uint64_t>(data[i] - '0');
    }
    return value;
}

} // namespace daw::archive
