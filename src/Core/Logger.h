#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenEra::Core
{
enum class LogLevel : uint8_t
{
    Trace,
    Info,
    Warn,
    Error,
};

[[nodiscard]] constexpr std::string_view ToString(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

// One log message as handed to sinks. `Message` is only valid during LogSink::Write.
struct LogRecord
{
    LogLevel Level = LogLevel::Info;
    std::chrono::system_clock::time_point Timestamp;
    std::string_view Message;
};

// Formats a record as one line without a trailing newline, with a UTC timestamp:
// "2026-11-25T14:03:07.123Z [WARN ] message"
[[nodiscard]] std::string FormatLogLine(const LogRecord& record);

// Destination for log records. The logger serializes calls to Write, so a sink
// does not need its own locking. A sink may throw; the logger catches it and
// counts the message as dropped. A sink must not log through the same logger.
class LogSink
{
public:
    LogSink() = default;
    virtual ~LogSink() = default;

    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;
    LogSink(LogSink&&) = delete;
    LogSink& operator=(LogSink&&) = delete;

    virtual void Write(const LogRecord& record) = 0;
};

// Writes FormatLogLine output to stderr, one line per record.
class StderrSink final : public LogSink
{
public:
    void Write(const LogRecord& record) override;
};

enum class FileMode : uint8_t
{
    Append,   // Keep what the file already holds.
    Truncate, // Start the file afresh.
};

// Writes FormatLogLine output to a file (UTF-8), one line per record. Lines are
// buffered and flushed at Warn and above, so problems reach the disk at once
// while chatty Trace / Info output stays cheap; everything is flushed on destruction.
class FileSink final : public LogSink
{
public:
    // Throws std::system_error if the file can't be opened.
    explicit FileSink(const std::filesystem::path& path, FileMode mode = FileMode::Append);
    void Write(const LogRecord& record) override;

private:
    std::ofstream m_stream;
};

// Sends FormatLogLine output to an attached debugger (OutputDebugStringW),
// converting from UTF-8. Costs almost nothing when no debugger is attached.
class DebuggerSink final : public LogSink
{
public:
    void Write(const LogRecord& record) override;
};

// Thread-safe logger that fans records out to its sinks.
//
// - Messages below the minimum level are discarded before formatting, so
//   disabled levels cost one atomic load.
// - Formatting happens outside the lock; only sink dispatch is serialized,
//   so lines from different threads never interleave.
// - Logging never throws. Messages lost to formatting or sink errors are
//   counted in DroppedMessages().
class Logger
{
public:
    Logger() noexcept = default;

    void SetMinimumLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel MinimumLevel() const noexcept;
    [[nodiscard]] bool IsEnabled(LogLevel level) const noexcept;

    // Null sinks are ignored. Like ClearSinks, may throw std::bad_alloc or
    // std::system_error; call these during setup and teardown, not while logging.
    void AddSink(std::shared_ptr<LogSink> sink);
    void ClearSinks();

    // Number of messages (or per-sink deliveries) lost to exceptions.
    [[nodiscard]] uint64_t DroppedMessages() const noexcept;

    // Logs an already formatted message.
    void Log(LogLevel level, std::string_view message) noexcept;

    template <typename... Args>
    void Trace(std::format_string<Args...> format, Args&&... args) noexcept
    {
        Write(LogLevel::Trace, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Info(std::format_string<Args...> format, Args&&... args) noexcept
    {
        Write(LogLevel::Info, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Warn(std::format_string<Args...> format, Args&&... args) noexcept
    {
        Write(LogLevel::Warn, format, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void Error(std::format_string<Args...> format, Args&&... args) noexcept
    {
        Write(LogLevel::Error, format, std::forward<Args>(args)...);
    }

private:
    template <typename... Args>
    void Write(LogLevel level, std::format_string<Args...> format, Args&&... args) noexcept
    {
        if (!IsEnabled(level))
        {
            return;
        }

        try
        {
            Log(level, std::format(format, std::forward<Args>(args)...));
        }
        catch (...)
        {
            // Out of memory, or a user formatter threw.
            m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::atomic<LogLevel> m_minimumLevel{LogLevel::Info};
    std::atomic<uint64_t> m_droppedMessages{0};
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<LogSink>> m_sinks;
};

// Process-wide logger. Starts with no sinks and a minimum level of Info.
[[nodiscard]] Logger& DefaultLogger() noexcept;

template <typename... Args>
void LogTrace(std::format_string<Args...> format, Args&&... args) noexcept
{
    DefaultLogger().Trace(format, std::forward<Args>(args)...);
}

template <typename... Args>
void LogInfo(std::format_string<Args...> format, Args&&... args) noexcept
{
    DefaultLogger().Info(format, std::forward<Args>(args)...);
}

template <typename... Args>
void LogWarn(std::format_string<Args...> format, Args&&... args) noexcept
{
    DefaultLogger().Warn(format, std::forward<Args>(args)...);
}

template <typename... Args>
void LogError(std::format_string<Args...> format, Args&&... args) noexcept
{
    DefaultLogger().Error(format, std::forward<Args>(args)...);
}
} // namespace OpenEra::Core
