#include "Logger.h"

#include <iostream>

namespace OpenEra::Core
{
std::string FormatLogLine(const LogRecord& record)
{
    const auto timestamp = std::chrono::floor<std::chrono::milliseconds>(record.Timestamp);
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z [{:<5}] {}", timestamp, ToString(record.Level), record.Message);
}

void StderrSink::Write(const LogRecord& record)
{
    std::cerr << FormatLogLine(record) << '\n';
}

FileSink::FileSink(const std::filesystem::path& path, FileMode mode)
    : m_stream(path, std::ios::binary | (mode == FileMode::Truncate ? std::ios::trunc : std::ios::app))
{
    if (!m_stream)
    {
        throw std::system_error(std::make_error_code(std::errc::io_error), "cannot open log file " + path.string());
    }
}

void FileSink::Write(const LogRecord& record)
{
    m_stream << FormatLogLine(record) << '\n';
    if (record.Level >= LogLevel::Warn)
    {
        m_stream.flush();
    }
}

void Logger::SetMinimumLevel(LogLevel level) noexcept
{
    m_minimumLevel.store(level, std::memory_order_relaxed);
}

LogLevel Logger::MinimumLevel() const noexcept
{
    return m_minimumLevel.load(std::memory_order_relaxed);
}

bool Logger::IsEnabled(LogLevel level) const noexcept
{
    return level >= MinimumLevel();
}

void Logger::AddSink(std::shared_ptr<LogSink> sink)
{
    if (!sink)
    {
        return;
    }

    const std::scoped_lock lock(m_mutex);
    m_sinks.push_back(std::move(sink));
}

void Logger::ClearSinks()
{
    // Release the sinks after unlocking, so a sink destructor that logs cannot deadlock.
    std::vector<std::shared_ptr<LogSink>> released;
    {
        const std::scoped_lock lock(m_mutex);
        released.swap(m_sinks);
    }
}

uint64_t Logger::DroppedMessages() const noexcept
{
    return m_droppedMessages.load(std::memory_order_relaxed);
}

void Logger::Log(LogLevel level, std::string_view message) noexcept
{
    if (!IsEnabled(level))
    {
        return;
    }

    const LogRecord record{.Level = level, .Timestamp = std::chrono::system_clock::now(), .Message = message};

    try
    {
        const std::scoped_lock lock(m_mutex);
        for (const auto& sink : m_sinks)
        {
            try
            {
                sink->Write(record);
            }
            catch (...)
            {
                // One failing sink must not stop delivery to the others.
                m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    catch (...)
    {
        // std::mutex::lock failed (std::system_error).
        m_droppedMessages.fetch_add(1, std::memory_order_relaxed);
    }
}

Logger& DefaultLogger() noexcept
{
    static Logger logger;
    return logger;
}
} // namespace OpenEra::Core
