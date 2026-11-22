// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Error handling primitives. Expected failures (missing files, bad formats,
// device errors, cancellation) are reported through Result<T> / Status rather
// than exceptions, so error paths are explicit at module boundaries.

#include <concepts>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace daw {

enum class ErrorCode {
    Unknown,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    IoError,
    ParseError,
    UnsupportedFormat,
    VersionTooNew,
    MigrationFailed,
    LimitExceeded,
    Corrupt,
    Stale,
    DeviceError,
    Cancelled,
    InferenceError,
    Unavailable,
};

const char* toString(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;

    std::string describe() const;
};

inline Error makeError(ErrorCode code, std::string message)
{
    return Error{code, std::move(message)};
}

template <typename T>
class [[nodiscard]] Result {
public:
    template <typename U>
        requires(std::constructible_from<T, U&&>
                 && !std::same_as<std::remove_cvref_t<U>, Error>
                 && !std::same_as<std::remove_cvref_t<U>, Result>)
    Result(U&& value) : storage_(std::in_place_index<0>, std::forward<U>(value))
    {
    }

    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { return std::get<0>(storage_); }
    const T& value() const& { return std::get<0>(storage_); }
    T&& value() && { return std::get<0>(std::move(storage_)); }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

    const Error& error() const { return std::get<1>(storage_); }

private:
    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), ok_(false) {}

    static Result success() { return {}; }

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    const Error& error() const { return error_; }

private:
    Error error_;
    bool ok_ = true;
};

using Status = Result<void>;

} // namespace daw
