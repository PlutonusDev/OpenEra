#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Kernel/StubRegistry.h"

using namespace OpenEra::Kernel;
using OpenEra::Core::Logger;
using OpenEra::Core::LogLevel;
using OpenEra::Core::LogRecord;
using OpenEra::Core::LogSink;

namespace
{
class CaptureSink final : public LogSink
{
public:
    void Write(const LogRecord& record) override { m_entries.emplace_back(record.Level, record.Message); }

    [[nodiscard]] const std::vector<std::pair<LogLevel, std::string>>& Entries() const noexcept { return m_entries; }

    [[nodiscard]] size_t Count(LogLevel level) const
    {
        return static_cast<size_t>(
            std::ranges::count_if(m_entries, [level](const auto& entry) { return entry.first == level; }));
    }

private:
    std::vector<std::pair<LogLevel, std::string>> m_entries;
};

struct Fixture
{
    Logger Log;
    std::shared_ptr<CaptureSink> Sink = std::make_shared<CaptureSink>();
    StubRegistry Registry{Log};

    Fixture() { Log.AddSink(Sink); }
};
} // namespace

TEST_CASE("StubKind names and stub ids", "[StubRegistry]")
{
    CHECK(ToString(StubKind::NtSyscall) == "NtSyscall");
    CHECK(ToString(StubKind::EraApi) == "EraApi");
    CHECK(ToString(StubKind::GameCoreApi) == "GameCoreApi");
    CHECK(ToString(StubKind::Win32Import) == "Win32Import");

    StubRecord named;
    named.Module = "kernel32.dll";
    named.Symbol = "CreateFileW";
    CHECK(FormatStubId(named) == "kernel32.dll!CreateFileW");

    StubRecord byOrdinal;
    byOrdinal.Kind = StubKind::NtSyscall;
    byOrdinal.Module = "ntoskrnl";
    byOrdinal.Ordinal = 0x42;
    CHECK(FormatStubId(byOrdinal) == "ntoskrnl!#66");
}

TEST_CASE("RecordHit tracks named symbols and ordinals separately", "[StubRegistry]")
{
    Fixture fixture;
    auto& registry = fixture.Registry;

    CHECK(registry.RecordHit(StubKind::Win32Import, "kernel32.dll", "CreateFileW") == 1);
    CHECK(registry.RecordHit(StubKind::Win32Import, "kernel32.dll", 42U) == 1);
    CHECK(registry.RecordHit(StubKind::NtSyscall, "kernel32.dll", 42U) == 1); // Same id, different kind.

    CHECK(registry.GetHitCount(StubKind::Win32Import, "kernel32.dll", "CreateFileW") == 1);
    CHECK(registry.GetHitCount(StubKind::Win32Import, "kernel32.dll", 42U) == 1);
    CHECK(registry.GetHitCount(StubKind::NtSyscall, "kernel32.dll", 42U) == 1);
    CHECK(registry.GetHitCount(StubKind::Win32Import, "kernel32.dll", "ReadFile") == 0);
    CHECK(registry.GetHitCount(StubKind::Win32Import, "kernel32.dll", 43U) == 0);
    CHECK(registry.GetHitCount(StubKind::EraApi, "kernel32.dll", "CreateFileW") == 0);
    CHECK(registry.UniqueCount() == 3);
    CHECK(registry.TotalHits() == 3);
}

TEST_CASE("RecordHit aggregates repeated hits", "[StubRegistry]")
{
    Fixture fixture;
    auto& registry = fixture.Registry;

    for (uint64_t expected = 1; expected <= 5; ++expected)
    {
        CHECK(registry.RecordHit(StubKind::GameCoreApi, "xgameruntime.dll", "XGameGetXboxTitleId") == expected);
    }
    registry.RecordHit(StubKind::EraApi, "xboxkrnl", 7U);

    CHECK(registry.GetHitCount(StubKind::GameCoreApi, "xgameruntime.dll", "XGameGetXboxTitleId") == 5);
    CHECK(registry.UniqueCount() == 2);
    CHECK(registry.TotalHits() == 6);
}

TEST_CASE("RecordHit logs only the first hit of each stub", "[StubRegistry]")
{
    Fixture fixture;
    auto& registry = fixture.Registry;

    for (int hit = 0; hit < 10; ++hit)
    {
        registry.RecordHit(StubKind::Win32Import, "user32.dll", "MessageBoxW", "dialogs not supported");
    }
    registry.RecordHit(StubKind::NtSyscall, "ntoskrnl", 0x18U);
    registry.RecordHit(StubKind::NtSyscall, "ntoskrnl", 0x18U);

    const auto& entries = fixture.Sink->Entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries.at(0).first == LogLevel::Warn);
    CHECK(entries.at(0).second == "Unimplemented Win32Import user32.dll!MessageBoxW (dialogs not supported)");
    CHECK(entries.at(1).second == "Unimplemented NtSyscall ntoskrnl!#24");
}

TEST_CASE("GetAllRecords returns sorted records and keeps the first note", "[StubRegistry]")
{
    Fixture fixture;
    auto& registry = fixture.Registry;
    CHECK(registry.GetAllRecords().empty());

    registry.RecordHit(StubKind::Win32Import, "b.dll", "Second");
    registry.RecordHit(StubKind::Win32Import, "a.dll", "First", "");
    registry.RecordHit(StubKind::Win32Import, "a.dll", "First", "first note");
    registry.RecordHit(StubKind::Win32Import, "a.dll", "First", "later note");
    registry.RecordHit(StubKind::NtSyscall, "ntoskrnl", 3U);

    const auto records = registry.GetAllRecords();
    REQUIRE(records.size() == 3);

    // Ordered by kind, then module, then symbol or ordinal.
    CHECK(records.at(0).Kind == StubKind::NtSyscall);
    CHECK(records.at(0).Ordinal == 3U);
    CHECK(records.at(0).Symbol.empty());
    CHECK(records.at(0).HitCount == 1);

    CHECK(records.at(1).Module == "a.dll");
    CHECK(records.at(1).Symbol == "First");
    CHECK_FALSE(records.at(1).Ordinal.has_value());
    CHECK(records.at(1).HitCount == 3);
    CHECK(records.at(1).Note == "first note");

    CHECK(records.at(2).Module == "b.dll");
    CHECK(records.at(2).Note.empty());
}

TEST_CASE("LogSummary reports each stub once, most-hit first", "[StubRegistry]")
{
    Fixture fixture;
    auto& registry = fixture.Registry;

    SECTION("empty registry")
    {
        registry.LogSummary();
        REQUIRE(fixture.Sink->Entries().size() == 1);
        CHECK(fixture.Sink->Entries().at(0).second == "Stub summary: no unimplemented calls");
    }

    SECTION("several stubs")
    {
        registry.RecordHit(StubKind::Win32Import, "kernel32.dll", "Sleep");
        for (int hit = 0; hit < 3; ++hit)
        {
            registry.RecordHit(StubKind::GameCoreApi, "xgameruntime.dll", "XUserAddAsync", "no user model");
        }
        registry.RecordHit(StubKind::NtSyscall, "ntoskrnl", 0x55U);
        registry.RecordHit(StubKind::NtSyscall, "ntoskrnl", 0x55U);

        const size_t warningsBefore = fixture.Sink->Count(LogLevel::Warn);
        registry.LogSummary();
        CHECK(fixture.Sink->Count(LogLevel::Warn) == warningsBefore);

        // Three first-hit warnings, then the four summary lines.
        const auto& entries = fixture.Sink->Entries();
        REQUIRE(entries.size() == 7);
        REQUIRE(fixture.Sink->Count(LogLevel::Info) == 4);
        CHECK(entries.at(3).second == "Stub summary: 3 unimplemented stubs, 6 hits");
        CHECK(entries.at(4).second == "         3  GameCoreApi  xgameruntime.dll!XUserAddAsync - no user model");
        CHECK(entries.at(5).second == "         2  NtSyscall    ntoskrnl!#85");
        CHECK(entries.at(6).second == "         1  Win32Import  kernel32.dll!Sleep");
    }
}

TEST_CASE("Reset clears every record", "[StubRegistry]")
{
    Fixture fixture;
    auto& registry = fixture.Registry;
    registry.RecordHit(StubKind::EraApi, "xboxkrnl", "KeQueryPerformanceCounter");
    registry.RecordHit(StubKind::EraApi, "xboxkrnl", "KeQueryPerformanceCounter");

    registry.Reset();
    CHECK(registry.UniqueCount() == 0);
    CHECK(registry.TotalHits() == 0);
    CHECK(registry.GetAllRecords().empty());
    CHECK(registry.GetHitCount(StubKind::EraApi, "xboxkrnl", "KeQueryPerformanceCounter") == 0);

    // A stub hit after a reset is new again, so it is logged again.
    CHECK(registry.RecordHit(StubKind::EraApi, "xboxkrnl", "KeQueryPerformanceCounter") == 1);
    CHECK(fixture.Sink->Count(LogLevel::Warn) == 2);
    CHECK(registry.InternalErrors() == 0);
}

TEST_CASE("StubRegistry aggregates concurrent hits", "[StubRegistry][Threading]")
{
    constexpr size_t kThreadCount = 8;
    constexpr uint64_t kHitsPerThread = 1000;
    constexpr std::array<std::string_view, 4> kSymbols = {"A", "B", "C", "D"};

    Fixture fixture;
    auto& registry = fixture.Registry;
    {
        std::vector<std::jthread> threads;
        threads.reserve(kThreadCount);
        for (size_t thread = 0; thread < kThreadCount; ++thread)
        {
            threads.emplace_back(
                [&registry, &kSymbols, thread]
                {
                    for (uint64_t hit = 0; hit < kHitsPerThread; ++hit)
                    {
                        registry.RecordHit(StubKind::Win32Import, "racy.dll", kSymbols.at((thread + hit) % 4));
                        registry.RecordHit(StubKind::NtSyscall, "ntoskrnl", static_cast<uint32_t>(hit % 2));
                    }
                });
        }
    }

    const uint64_t perSymbol = kThreadCount * kHitsPerThread / kSymbols.size();
    for (const auto symbol : kSymbols)
    {
        CHECK(registry.GetHitCount(StubKind::Win32Import, "racy.dll", symbol) == perSymbol);
    }
    CHECK(registry.GetHitCount(StubKind::NtSyscall, "ntoskrnl", 0U) == kThreadCount * kHitsPerThread / 2);
    CHECK(registry.GetHitCount(StubKind::NtSyscall, "ntoskrnl", 1U) == kThreadCount * kHitsPerThread / 2);
    CHECK(registry.UniqueCount() == 6);
    CHECK(registry.TotalHits() == 2 * kThreadCount * kHitsPerThread);

    // Each stub was logged exactly once despite the contention.
    CHECK(fixture.Sink->Count(LogLevel::Warn) == 6);
}

TEST_CASE("DefaultStubRegistry is a single shared instance", "[StubRegistry]")
{
    CHECK(&DefaultStubRegistry() == &DefaultStubRegistry());
}
