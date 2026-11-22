// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/Logger.h>

#include <daw/core/Files.h>
#include <daw/core/RtThread.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace daw {

const char* toString(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    }
    return "info";
}

std::string LogRecord::toJsonLine() const
{
    nlohmann::json j;
    j["ts"] = timestamp;
    j["level"] = toString(level);
    j["cat"] = category;
    j["msg"] = message;
    j["tid"] = threadId;
    if (fields.is_object() && !fields.empty())
        j["fields"] = fields;
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

namespace {
std::uint32_t currentThreadId()
{
#ifdef _WIN32
    return static_cast<std::uint32_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}
} // namespace

struct Logger::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable drained;
    std::deque<LogRecord> queue;
    bool stopping = false;
    bool busy = false;

    std::mutex sinkMutex; // held while sinks run; removeSink() synchronises on it
    std::map<int, Sink> sinks;
    int nextSinkId = 1;
    std::ofstream file;

    std::atomic<int> minLevel{static_cast<int>(LogLevel::Debug)};
    std::atomic<std::uint64_t> droppedRt{0};
    std::thread writer;

    Impl() : writer([this] { run(); }) {}

    ~Impl()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (writer.joinable())
            writer.join();
    }

    void run()
    {
#ifdef _WIN32
        ::SetThreadDescription(::GetCurrentThread(), L"daw-log-writer");
#endif
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [this] { return stopping || !queue.empty(); });
            if (queue.empty() && stopping)
                return;
            std::deque<LogRecord> batch;
            batch.swap(queue);
            busy = true;
            lock.unlock();
            {
                std::lock_guard sinkLock(sinkMutex);
                for (const LogRecord& record : batch) {
                    if (file.is_open())
                        file << record.toJsonLine() << '\n';
                    for (auto& [id, sink] : sinks) {
                        try {
                            sink(record);
                        } catch (...) {
                            // A failing sink must never take down logging.
                        }
                    }
                }
                if (file.is_open())
                    file.flush();
            }
            lock.lock();
            busy = false;
            if (queue.empty())
                drained.notify_all();
        }
    }
};

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

Logger::Logger() : impl_(std::make_unique<Impl>()) {}
Logger::~Logger() = default;

Status Logger::openFile(const std::filesystem::path& path)
{
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    std::lock_guard sinkLock(impl_->sinkMutex);
    impl_->file.close();
    impl_->file.open(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!impl_->file)
        return makeError(ErrorCode::IoError, "cannot open log file: " + pathToUtf8(path));
    return Status::success();
}

int Logger::addSink(Sink sink)
{
    std::lock_guard sinkLock(impl_->sinkMutex);
    const int id = impl_->nextSinkId++;
    impl_->sinks.emplace(id, std::move(sink));
    return id;
}

void Logger::removeSink(int id)
{
    std::lock_guard sinkLock(impl_->sinkMutex);
    impl_->sinks.erase(id);
}

void Logger::setMinLevel(LogLevel level)
{
    impl_->minLevel.store(static_cast<int>(level));
}

LogLevel Logger::minLevel() const
{
    return static_cast<LogLevel>(impl_->minLevel.load());
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message, nlohmann::json fields)
{
    if (rt::isAudioThread()) {
        impl_->droppedRt.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (static_cast<int>(level) < impl_->minLevel.load(std::memory_order_relaxed))
        return;

    LogRecord record;
    record.timestamp = utcTimestampIso8601();
    record.level = level;
    record.category = std::string(category);
    record.message = std::string(message);
    record.fields = std::move(fields);
    record.threadId = currentThreadId();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->queue.push_back(std::move(record));
    }
    impl_->wake.notify_one();
}

void Logger::flush()
{
    std::unique_lock lock(impl_->mutex);
    impl_->drained.wait(lock, [this] { return impl_->queue.empty() && !impl_->busy; });
}

std::uint64_t Logger::droppedFromAudioThread() const
{
    return impl_->droppedRt.load(std::memory_order_relaxed);
}

} // namespace daw
