// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// FNV-1a 64-bit hash: cache keys and file checksums (not cryptographic).

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace daw {

class Fnv1a64 {
public:
    void update(const void* data, std::size_t size) noexcept
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash_ ^= bytes[i];
            hash_ *= 1099511628211ull;
        }
    }

    void update(std::string_view text) noexcept { update(text.data(), text.size()); }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void updateValue(const T& value) noexcept
    {
        update(&value, sizeof(T));
    }

    std::uint64_t digest() const noexcept { return hash_; }

private:
    std::uint64_t hash_ = 14695981039346656037ull;
};

inline std::uint64_t fnv1a64(std::string_view text) noexcept
{
    Fnv1a64 h;
    h.update(text);
    return h.digest();
}

} // namespace daw
