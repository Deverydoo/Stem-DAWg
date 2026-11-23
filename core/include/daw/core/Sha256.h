// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// SHA-256 (FIPS 180-4), used to verify downloaded files (the SoundFont
// package) before they are trusted.

#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace daw {

class Sha256 {
public:
    Sha256() noexcept;

    void update(const void* data, std::size_t size) noexcept;
    void update(std::string_view text) noexcept { update(text.data(), text.size()); }
    /// Finishes the hash; the object must not be updated afterwards.
    std::array<std::uint8_t, 32> finish() noexcept;
    /// finish() as 64 lowercase hex digits.
    std::string finishHex();

private:
    void block(const std::uint8_t* data) noexcept;

    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0; // bytes
};

std::string sha256Hex(std::string_view data);

/// Hash of a file's contents (streamed), as lowercase hex.
Result<std::string> sha256File(const std::filesystem::path& path, const CancellationToken& cancel = {});

} // namespace daw
