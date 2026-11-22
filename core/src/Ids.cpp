// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/Ids.h>

#include <array>
#include <cstdint>
#include <random>

namespace daw {

namespace {

std::mt19937_64& threadRng()
{
    thread_local std::mt19937_64 rng = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    return rng;
}

} // namespace

std::string generateUuid()
{
    auto& rng = threadRng();
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 8) {
        const std::uint64_t value = rng();
        for (std::size_t b = 0; b < 8; ++b)
            bytes[i + b] = static_cast<std::uint8_t>(value >> (b * 8));
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40); // version 4
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80); // RFC 4122 variant

    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            text.push_back('-');
        text.push_back(hex[bytes[i] >> 4]);
        text.push_back(hex[bytes[i] & 0x0F]);
    }
    return text;
}

bool isCanonicalUuid(std::string_view text) noexcept
{
    if (text.size() != 36)
        return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

} // namespace daw
