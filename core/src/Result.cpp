// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/Result.h>

namespace daw {

const char* toString(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::Unknown: return "unknown";
    case ErrorCode::InvalidArgument: return "invalid-argument";
    case ErrorCode::NotFound: return "not-found";
    case ErrorCode::AlreadyExists: return "already-exists";
    case ErrorCode::IoError: return "io-error";
    case ErrorCode::ParseError: return "parse-error";
    case ErrorCode::UnsupportedFormat: return "unsupported-format";
    case ErrorCode::VersionTooNew: return "version-too-new";
    case ErrorCode::MigrationFailed: return "migration-failed";
    case ErrorCode::LimitExceeded: return "limit-exceeded";
    case ErrorCode::Corrupt: return "corrupt";
    case ErrorCode::Stale: return "stale";
    case ErrorCode::DeviceError: return "device-error";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::InferenceError: return "inference-error";
    case ErrorCode::Unavailable: return "unavailable";
    }
    return "unknown";
}

std::string Error::describe() const
{
    std::string text = toString(code);
    if (!message.empty()) {
        text += ": ";
        text += message;
    }
    return text;
}

} // namespace daw
