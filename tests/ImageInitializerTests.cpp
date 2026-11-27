// Starting images OpenEra maps itself: real test images built with the tests,
// linked through shims and host passthrough, then initialised and run.
#include <bit>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"
#include "Loader/HostModules.h"
#include "Loader/ImageInitializer.h"
#include "Loader/ImageMemory.h"
#include "Loader/ModuleManager.h"

using namespace OpenEra::Loader;
using OpenEra::Core::Logger;
using OpenEra::Kernel::ShimRegistry;
using OpenEra::Kernel::StubRegistry;

namespace
{
using CountFunction = int();

constexpr std::string_view kStartImage = "StartImage.dll";
constexpr std::string_view kEntryImage = "EntryImage.exe";

class StartFixture
{
public:
    StartFixture()
    {
        REQUIRE(RegisterLoaderShims(m_shims));
        m_manager = std::make_unique<ModuleManager>(DirectoryModuleReader(OPENERA_FIXTURE_DIR), m_shims, m_stubs,
                                                    *m_allocator, m_log, m_host.get());
        m_manager->SetInitializer(&m_initializer);
        ModuleManager::SetActive(m_manager.get());
    }

    ~StartFixture()
    {
        m_manager->Shutdown();
        ModuleManager::SetActive(nullptr);
    }

    StartFixture(const StartFixture&) = delete;
    StartFixture& operator=(const StartFixture&) = delete;
    StartFixture(StartFixture&&) = delete;
    StartFixture& operator=(StartFixture&&) = delete;

    template <typename Function>
    [[nodiscard]] Function* Export(std::string_view module, std::string_view symbol)
    {
        const auto address = m_manager->FindExport(module, symbol);
        REQUIRE(address.has_value());
        return std::bit_cast<Function*>(address.value_or(0));
    }

    [[nodiscard]] ModuleManager& Manager() const noexcept { return *m_manager; }
    [[nodiscard]] ImageInitializer& Initializer() noexcept { return m_initializer; }

private:
    Logger m_log;
    StubRegistry m_stubs{m_log};
    ShimRegistry m_shims;
    std::unique_ptr<ImageAllocator> m_allocator = CreateVirtualImageAllocator();
    std::unique_ptr<HostModules> m_host = HostModules::CreateDefault();
    ImageInitializer m_initializer{OPENERA_TLS_SLOT, std::filesystem::temp_directory_path() / "OpenEraTests", m_log};
    std::unique_ptr<ModuleManager> m_manager;
};
} // namespace

TEST_CASE("Attaching runs TLS callbacks and DllMain once", "[initializer]")
{
    StartFixture fixture;
    REQUIRE(fixture.Manager().Load(kStartImage) == ModuleError::None);
    CHECK(fixture.Export<CountFunction>(kStartImage, "ProcessAttachCount")() == 1);
    CHECK(fixture.Export<CountFunction>(kStartImage, "CallbackProcessAttachCount")() == 1);

    auto* const detachCount = fixture.Export<CountFunction>(kStartImage, "ProcessDetachCount");
    CHECK(detachCount() == 0);
    fixture.Manager().Shutdown();
    CHECK(detachCount() == 1);
}

TEST_CASE("Mapped images get their own static TLS on every thread", "[initializer]")
{
    StartFixture fixture;
    REQUIRE(fixture.Manager().Load(kStartImage) == ModuleError::None);
    auto* const nextCounter = fixture.Export<CountFunction>(kStartImage, "NextCounter");
    CHECK(nextCounter() == 6);
    CHECK(nextCounter() == 7);

    int otherThread = 0;
    std::thread thread([&otherThread, nextCounter] { otherThread = nextCounter(); });
    thread.join();
    CHECK(otherThread == 6); // A fresh copy of the template.
    CHECK(nextCounter() == 8);

    // The same for a thread the image creates itself.
    CHECK(fixture.Export<CountFunction>(kStartImage, "NextCounterOnNewThread")() == 6);
    CHECK(nextCounter() == 9);
}

TEST_CASE("New threads attach to mapped images", "[initializer]")
{
    StartFixture fixture;
    REQUIRE(fixture.Manager().Load(kStartImage) == ModuleError::None);
    auto* const threadAttach = fixture.Export<CountFunction>(kStartImage, "ThreadAttachCount");
    auto* const callbackThreadAttach = fixture.Export<CountFunction>(kStartImage, "CallbackThreadAttachCount");
    const int before = threadAttach();
    const int callbacksBefore = callbackThreadAttach();

    std::thread([] {}).join();
    CHECK(threadAttach() > before);
    CHECK(callbackThreadAttach() > callbacksBefore);
}

TEST_CASE("RunEntryPoint returns the executable's exit code", "[initializer]")
{
    StartFixture fixture;
    REQUIRE(fixture.Manager().Load(kEntryImage) == ModuleError::None);
    const auto* module = fixture.Manager().Find(kEntryImage);
    REQUIRE(module != nullptr);
    CHECK(fixture.Initializer().RunEntryPoint(*module) == std::optional<uint32_t>(42));
}
