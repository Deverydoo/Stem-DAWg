// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Structured logging (JSON lines). Records are formatted on the calling thread
// and written by a dedicated writer thread; sinks (e.g. the Qt diagnostics
// panel) are invoked on the writer thread.
//
// Real-time contract: NEVER call from the audio thread. If it happens anyway
// the record is dropped and counted rather than blocking audio. The engine
// reports through RtEvent queues instead.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <daw/core/Result.h>

namespace daw {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

const char* toString(LogLevel level) noexcept;

struct LogRecord {
    std::string timestamp;
    LogLevel level = LogLevel::Info;
    std::string category;
    std::string message;
    nlohmann::json fields;
    std::uint32_t threadId = 0;

    std::string toJsonLine() const;
};

class Logger {
public:
    using Sink = std::function<void(const LogRecord&)>;

    static Logger& instance();

    /// Appends JSON lines to `file` (directories are created).
    Status openFile(const std::filesystem::path& file);

    int addSink(Sink sink);
    /// After this returns the sink is guaranteed not to be running or called again.
    void removeSink(int id);

    void setMinLevel(LogLevel level);
    LogLevel minLevel() const;

    void log(LogLevel level, std::string_view category, std::string_view message,
             nlohmann::json fields = nlohmann::json::object());

    /// Blocks until all queued records have been written and delivered.
    void flush();

    std::uint64_t droppedFromAudioThread() const;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger();
    ~Logger();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

namespace logging {
inline void debug(std::string_view category, std::string_view message, nlohmann::json fields = nlohmann::json::object())
{
    Logger::instance().log(LogLevel::Debug, category, message, std::move(fields));
}
inline void info(std::string_view category, std::string_view message, nlohmann::json fields = nlohmann::json::object())
{
    Logger::instance().log(LogLevel::Info, category, message, std::move(fields));
}
inline void warn(std::string_view category, std::string_view message, nlohmann::json fields = nlohmann::json::object())
{
    Logger::instance().log(LogLevel::Warn, category, message, std::move(fields));
}
inline void error(std::string_view category, std::string_view message, nlohmann::json fields = nlohmann::json::object())
{
    Logger::instance().log(LogLevel::Error, category, message, std::move(fields));
}
} // namespace logging

} // namespace daw
