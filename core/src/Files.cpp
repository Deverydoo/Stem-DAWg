// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/Files.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace daw {

std::string pathToUtf8(const fs::path& path)
{
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string pathToGenericUtf8(const fs::path& path)
{
    const std::u8string u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

fs::path utf8ToPath(std::string_view utf8)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

Result<std::string> readFileBytes(const fs::path& path)
{
    std::error_code ec;
    if (!fs::exists(path, ec))
        return makeError(ErrorCode::NotFound, "file not found: " + pathToUtf8(path));

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return makeError(ErrorCode::IoError, "cannot open: " + pathToUtf8(path));
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad())
        return makeError(ErrorCode::IoError, "read failed: " + pathToUtf8(path));
    return buffer.str();
}

#ifdef _WIN32

namespace {
std::string lastErrorText(const char* what)
{
    return std::format("{} (Win32 error {})", what, ::GetLastError());
}
} // namespace

Status writeFileAtomically(const fs::path& path, std::string_view bytes)
{
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);

    const fs::path temp = path.parent_path()
                          / (path.filename().wstring() + L".tmp-" + std::to_wstring(::GetCurrentProcessId()) + L"-"
                             + std::to_wstring(::GetCurrentThreadId()));

    HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return makeError(ErrorCode::IoError, lastErrorText("cannot create temporary file") + ": " + pathToUtf8(temp));

    std::size_t written = 0;
    bool ok = true;
    while (written < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, 1u << 24));
        DWORD done = 0;
        if (!::WriteFile(file, bytes.data() + written, chunk, &done, nullptr) || done == 0) {
            ok = false;
            break;
        }
        written += done;
    }
    ok = ok && ::FlushFileBuffers(file);
    ::CloseHandle(file);

    if (!ok) {
        ::DeleteFileW(temp.c_str());
        return makeError(ErrorCode::IoError, lastErrorText("write failed") + ": " + pathToUtf8(path));
    }
    if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const std::string message = lastErrorText("atomic replace failed");
        ::DeleteFileW(temp.c_str());
        return makeError(ErrorCode::IoError, message + ": " + pathToUtf8(path));
    }
    return Status::success();
}

fs::path appDataDirectory()
{
    fs::path base;
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"))
        base = local;
    else
        base = fs::temp_directory_path();
    fs::path dir = base / L"StemDAWg";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

#else

Status writeFileAtomically(const fs::path& path, std::string_view bytes)
{
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return makeError(ErrorCode::IoError, "cannot create temporary file: " + temp.string());
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out)
            return makeError(ErrorCode::IoError, "write failed: " + temp.string());
    }
    fs::rename(temp, path, ec);
    if (ec)
        return makeError(ErrorCode::IoError, "atomic replace failed: " + ec.message());
    return Status::success();
}

fs::path appDataDirectory()
{
    const char* home = std::getenv("HOME");
    fs::path dir = fs::path(home ? home : "/tmp") / ".stem-dawg";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

#endif

std::string utcTimestampIso8601()
{
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", now);
}

std::string utcTimestampForFilename()
{
    const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const auto millis = (now - seconds).count();
    return std::format("{:%Y%m%d-%H%M%S}-{:03}", seconds, millis);
}

} // namespace daw
