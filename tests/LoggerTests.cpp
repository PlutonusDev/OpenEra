#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"

using OpenEra::Core::Logger;
using OpenEra::Core::LogLevel;
using OpenEra::Core::LogRecord;
using OpenEra::Core::LogSink;

namespace
{
struct CapturedEntry
{
    LogLevel Level = LogLevel::Info;
    std::string Message;
};

// Not internally synchronized: the logger must serialize calls to Write.
class CaptureSink final : public LogSink
{
public:
    void Write(const LogRecord& record) override
    {
        m_entries.push_back({.Level = record.Level, .Message = std::string(record.Message)});
    }

    [[nodiscard]] const std::vector<CapturedEntry>& Entries() const noexcept { return m_entries; }

private:
    std::vector<CapturedEntry> m_entries;
};

class ThrowingSink final : public LogSink
{
public:
    void Write(const LogRecord& /*record*/) override { throw std::runtime_error("sink failure"); }
};

// Counts how often it is formatted, to prove disabled levels skip formatting.
struct CountingArg
{
    std::atomic<int>* Count = nullptr;
};

struct ThrowingArg
{
};

// Parses "thread <T> message <M>".
std::optional<std::pair<size_t, size_t>> ParseThreadMessage(std::string_view text) noexcept
{
    constexpr std::string_view kThreadPrefix = "thread ";
    constexpr std::string_view kMessageSeparator = " message ";

    if (!text.starts_with(kThreadPrefix))
    {
        return std::nullopt;
    }
    text.remove_prefix(kThreadPrefix.size());

    const auto separator = text.find(kMessageSeparator);
    if (separator == std::string_view::npos)
    {
        return std::nullopt;
    }

    const auto parse = [](std::string_view digits) noexcept -> std::optional<size_t>
    {
        size_t value = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (error != std::errc{} || end != digits.data() + digits.size())
        {
            return std::nullopt;
        }
        return value;
    };

    const auto thread = parse(text.substr(0, separator));
    const auto message = parse(text.substr(separator + kMessageSeparator.size()));
    if (!thread || !message)
    {
        return std::nullopt;
    }
    return std::pair{*thread, *message};
}
} // namespace

template <>
struct std::formatter<CountingArg> : std::formatter<int>
{
    auto format(const CountingArg& arg, std::format_context& context) const
    {
        return std::formatter<int>::format(++*arg.Count, context);
    }
};

template <>
struct std::formatter<ThrowingArg> : std::formatter<int>
{
    static auto format(const ThrowingArg& /*arg*/, std::format_context& /*context*/) -> std::format_context::iterator
    {
        throw std::runtime_error("formatter failure");
    }
};

TEST_CASE("LogLevel names", "[Logger]")
{
    using OpenEra::Core::ToString;
    STATIC_REQUIRE(ToString(LogLevel::Trace) == "TRACE");
    STATIC_REQUIRE(ToString(LogLevel::Info) == "INFO");
    STATIC_REQUIRE(ToString(LogLevel::Warn) == "WARN");
    STATIC_REQUIRE(ToString(LogLevel::Error) == "ERROR");
    STATIC_REQUIRE(LogLevel::Trace < LogLevel::Info);
    STATIC_REQUIRE(LogLevel::Info < LogLevel::Warn);
    STATIC_REQUIRE(LogLevel::Warn < LogLevel::Error);
}

TEST_CASE("FormatLogLine writes a UTC timestamp, padded level and message", "[Logger]")
{
    using namespace std::chrono;
    const auto timestamp =
        sys_days{year{2026} / November / 25} + hours{14} + minutes{3} + seconds{7} + milliseconds{123};

    CHECK(OpenEra::Core::FormatLogLine({.Level = LogLevel::Warn, .Timestamp = timestamp, .Message = "hello"}) ==
          "2026-11-25T14:03:07.123Z [WARN ] hello");
    CHECK(OpenEra::Core::FormatLogLine({.Level = LogLevel::Error, .Timestamp = timestamp, .Message = ""}) ==
          "2026-11-25T14:03:07.123Z [ERROR] ");

    // Sub-millisecond precision is truncated, not rounded.
    CHECK(OpenEra::Core::FormatLogLine(
              {.Level = LogLevel::Info, .Timestamp = timestamp + microseconds{999}, .Message = "x"}) ==
          "2026-11-25T14:03:07.123Z [INFO ] x");
}

TEST_CASE("Logger formats arguments and delivers them with the right level", "[Logger]")
{
    Logger logger;
    logger.SetMinimumLevel(LogLevel::Trace);
    const auto sink = std::make_shared<CaptureSink>();
    logger.AddSink(sink);

    logger.Trace("trace {}", 1);
    logger.Info("info {} {}", "two", 3.5);
    logger.Warn("warn {:#x}", 0x1000);
    logger.Error("error");
    logger.Log(LogLevel::Info, "preformatted {not a format}");

    const auto& entries = sink->Entries();
    REQUIRE(entries.size() == 5);
    CHECK(entries.at(0).Level == LogLevel::Trace);
    CHECK(entries.at(0).Message == "trace 1");
    CHECK(entries.at(1).Level == LogLevel::Info);
    CHECK(entries.at(1).Message == "info two 3.5");
    CHECK(entries.at(2).Level == LogLevel::Warn);
    CHECK(entries.at(2).Message == "warn 0x1000");
    CHECK(entries.at(3).Level == LogLevel::Error);
    CHECK(entries.at(3).Message == "error");
    CHECK(entries.at(4).Message == "preformatted {not a format}");
    CHECK(logger.DroppedMessages() == 0);
}

TEST_CASE("Logger drops messages below the minimum level without formatting them", "[Logger]")
{
    Logger logger;
    CHECK(logger.MinimumLevel() == LogLevel::Info);

    const auto sink = std::make_shared<CaptureSink>();
    logger.AddSink(sink);
    std::atomic<int> formatCount{0};

    SECTION("default minimum is Info")
    {
        CHECK_FALSE(logger.IsEnabled(LogLevel::Trace));
        CHECK(logger.IsEnabled(LogLevel::Info));

        logger.Trace("{}", CountingArg{&formatCount});
        logger.Info("{}", CountingArg{&formatCount});

        REQUIRE(sink->Entries().size() == 1);
        CHECK(sink->Entries().at(0).Message == "1");
        CHECK(formatCount == 1);
    }

    SECTION("raising the minimum to Error")
    {
        logger.SetMinimumLevel(LogLevel::Error);
        CHECK(logger.MinimumLevel() == LogLevel::Error);

        logger.Info("{}", CountingArg{&formatCount});
        logger.Warn("{}", CountingArg{&formatCount});
        logger.Log(LogLevel::Warn, "also dropped");
        logger.Error("kept");

        REQUIRE(sink->Entries().size() == 1);
        CHECK(sink->Entries().at(0).Message == "kept");
        CHECK(formatCount == 0);
    }

    CHECK(logger.DroppedMessages() == 0);
}

TEST_CASE("Logger fans out to every sink and supports clearing them", "[Logger]")
{
    Logger logger;
    const auto first = std::make_shared<CaptureSink>();
    const auto second = std::make_shared<CaptureSink>();

    logger.AddSink(first);
    logger.AddSink(nullptr); // ignored
    logger.AddSink(second);
    logger.Info("one");

    CHECK(first->Entries().size() == 1);
    CHECK(second->Entries().size() == 1);

    logger.ClearSinks();
    logger.Info("two"); // no sinks: silently discarded

    CHECK(first->Entries().size() == 1);
    CHECK(second->Entries().size() == 1);
    CHECK(first.use_count() == 1); // the logger released its reference
    CHECK(logger.DroppedMessages() == 0);
}

TEST_CASE("Logger contains failures from sinks and formatters", "[Logger]")
{
    Logger logger;
    const auto before = std::make_shared<CaptureSink>();
    const auto after = std::make_shared<CaptureSink>();
    logger.AddSink(before);
    logger.AddSink(std::make_shared<ThrowingSink>());
    logger.AddSink(after);

    SECTION("a throwing sink does not block the other sinks")
    {
        logger.Info("message");

        CHECK(before->Entries().size() == 1);
        CHECK(after->Entries().size() == 1);
        CHECK(logger.DroppedMessages() == 1);
    }

    SECTION("a throwing formatter drops only that message")
    {
        logger.Error("{}", ThrowingArg{});
        CHECK(before->Entries().empty());
        CHECK(logger.DroppedMessages() == 1);

        logger.Error("next");
        CHECK(before->Entries().size() == 1);
    }
}

TEST_CASE("Logger serializes concurrent writers", "[Logger][Threading]")
{
    constexpr size_t kThreadCount = 8;
    constexpr size_t kMessagesPerThread = 500;

    Logger logger;
    const auto sink = std::make_shared<CaptureSink>();
    logger.AddSink(sink);

    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreadCount);
        for (size_t thread = 0; thread < kThreadCount; ++thread)
        {
            threads.emplace_back(
                [&logger, thread]
                {
                    for (size_t message = 0; message < kMessagesPerThread; ++message)
                    {
                        logger.Info("thread {} message {}", thread, message);
                    }
                });
        }
    } // jthreads join here

    const auto& entries = sink->Entries();
    REQUIRE(entries.size() == kThreadCount * kMessagesPerThread);

    // Each thread's messages arrive intact and in that thread's order.
    std::vector<size_t> nextExpected(kThreadCount, 0);
    for (const auto& entry : entries)
    {
        const auto parsed = ParseThreadMessage(entry.Message);
        REQUIRE(parsed.has_value());
        const auto [thread, message] = *parsed;
        REQUIRE(thread < kThreadCount);
        REQUIRE(entry.Message == std::format("thread {} message {}", thread, message));
        CHECK(message == nextExpected.at(thread));
        nextExpected.at(thread) = message + 1;
    }
    CHECK(logger.DroppedMessages() == 0);
}

TEST_CASE("DefaultLogger is a single shared instance used by the free functions", "[Logger]")
{
    auto& logger = OpenEra::Core::DefaultLogger();
    CHECK(&logger == &OpenEra::Core::DefaultLogger());

    const auto previousLevel = logger.MinimumLevel();
    const auto sink = std::make_shared<CaptureSink>();
    logger.SetMinimumLevel(LogLevel::Trace);
    logger.AddSink(sink);

    OpenEra::Core::LogTrace("t{}", 1);
    OpenEra::Core::LogInfo("i{}", 2);
    OpenEra::Core::LogWarn("w{}", 3);
    OpenEra::Core::LogError("e{}", 4);

    logger.ClearSinks();
    logger.SetMinimumLevel(previousLevel);

    const auto& entries = sink->Entries();
    REQUIRE(entries.size() == 4);
    CHECK(entries.at(0).Level == LogLevel::Trace);
    CHECK(entries.at(0).Message == "t1");
    CHECK(entries.at(1).Level == LogLevel::Info);
    CHECK(entries.at(1).Message == "i2");
    CHECK(entries.at(2).Level == LogLevel::Warn);
    CHECK(entries.at(2).Message == "w3");
    CHECK(entries.at(3).Level == LogLevel::Error);
    CHECK(entries.at(3).Message == "e4");
}
