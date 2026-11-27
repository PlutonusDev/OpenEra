// Synchronization, time and process functions called from an image OpenEra
// maps itself (tests/Fixtures/SyncImage.cpp), through the import binding a
// title gets: OpenEra shims first, then the host's Windows DLLs.
#include <algorithm>
#include <bit>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Kernel/DebugOutputShims.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"
#include "Loader/HostModules.h"
#include "Loader/ImageMemory.h"
#include "Loader/ImportResolver.h"
#include "Loader/ModuleManager.h"

using namespace OpenEra::Loader;
using OpenEra::Core::Logger;
using OpenEra::Core::LogRecord;
using OpenEra::Core::LogSink;
using OpenEra::Kernel::DebugOutputLine;
using OpenEra::Kernel::ShimRegistry;
using OpenEra::Kernel::StubRegistry;

namespace
{
using CheckFunction = unsigned();
using HelloFunction = void();

constexpr std::string_view kImage = "SyncImage.dll";

class CaptureSink final : public LogSink
{
public:
    void Write(const LogRecord& record) override { m_messages.emplace_back(record.Message); }
    [[nodiscard]] const std::vector<std::string>& Messages() const noexcept { return m_messages; }

private:
    std::vector<std::string> m_messages;
};

class SyncFixture
{
public:
    SyncFixture()
    {
        REQUIRE(RegisterLoaderShims(m_shims));
        m_manager = std::make_unique<ModuleManager>(DirectoryModuleReader(OPENERA_FIXTURE_DIR), m_shims, m_stubs,
                                                    *m_allocator, m_log, m_host.get());
        ModuleManager::SetActive(m_manager.get());
        REQUIRE(m_manager->Load(kImage) == ModuleError::None);
    }

    ~SyncFixture() { ModuleManager::SetActive(nullptr); }

    SyncFixture(const SyncFixture&) = delete;
    SyncFixture& operator=(const SyncFixture&) = delete;
    SyncFixture(SyncFixture&&) = delete;
    SyncFixture& operator=(SyncFixture&&) = delete;

    template <typename Function>
    [[nodiscard]] Function* Export(std::string_view symbol) const
    {
        const auto address = m_manager->FindExport(kImage, symbol);
        REQUIRE(address.has_value());
        return std::bit_cast<Function*>(address.value_or(0));
    }

    [[nodiscard]] const LoadedModule& Module() const
    {
        const auto* module = m_manager->Find(kImage);
        REQUIRE(module != nullptr);
        return *module;
    }

private:
    Logger m_log;
    StubRegistry m_stubs{m_log};
    ShimRegistry m_shims;
    std::unique_ptr<ImageAllocator> m_allocator = CreateVirtualImageAllocator();
    std::unique_ptr<HostModules> m_host = HostModules::CreateDefault();
    std::unique_ptr<ModuleManager> m_manager;
};
} // namespace

TEST_CASE("The test image links its synchronization and time imports without stubs", "[timesync]")
{
    const SyncFixture fixture;
    const auto& imports = fixture.Module().Imports;
    REQUIRE(imports.has_value());
    CHECK(imports->CountFrom(ImportSource::Stub) == 0);
    CHECK(imports->CountFrom(ImportSource::Host) > 0);
}

TEST_CASE("Critical sections, SRW locks, events and waits work in mapped code", "[timesync]")
{
    const SyncFixture fixture;
    CHECK(fixture.Export<CheckFunction>("RunSyncChecks")() == 0);
}

TEST_CASE("Time functions work in mapped code", "[timesync]")
{
    const SyncFixture fixture;
    CHECK(fixture.Export<CheckFunction>("RunTimeChecks")() == 0);
}

TEST_CASE("Threads, TLS slots, priorities, affinity and names work in mapped code", "[timesync][threads]")
{
    const SyncFixture fixture;
    CHECK(fixture.Export<CheckFunction>("RunThreadChecks")() == 0);
}

TEST_CASE("Mapped code finds itself through the loader shims", "[timesync]")
{
    const SyncFixture fixture;
    CHECK(fixture.Export<CheckFunction>("RunProcessChecks")() == 0);
}

TEST_CASE("Title debug output reaches the OpenEra log", "[timesync]")
{
    const SyncFixture fixture;
    const auto sink = std::make_shared<CaptureSink>();
    auto& logger = OpenEra::Core::DefaultLogger();
    logger.AddSink(sink);
    fixture.Export<HelloFunction>("SayHello")();
    logger.ClearSinks();

    const auto& messages = sink->Messages();
    CHECK(std::ranges::find(messages, "Title: hello from a mapped image") != messages.end());
    CHECK(std::ranges::find(messages, "Title: narrow hello") != messages.end());
}

TEST_CASE("Debug output lines drop trailing line breaks", "[timesync]")
{
    CHECK(DebugOutputLine("frame 12\r\n") == "Title: frame 12");
    CHECK(DebugOutputLine("a\nb\n") == "Title: a\nb");
    CHECK(DebugOutputLine("\r\n").empty());
    CHECK(DebugOutputLine("").empty());
}
