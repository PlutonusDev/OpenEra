// Exceptions in an image OpenEra maps itself: a real DLL built with the tests,
// linked through shims and host passthrough, then called.
#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"
#include "Loader/HostModules.h"
#include "Loader/ImageMemory.h"
#include "Loader/ImportResolver.h"
#include "Loader/ModuleManager.h"

using namespace OpenEra::Loader;
using OpenEra::Core::Logger;
using OpenEra::Kernel::FromShimProc;
using OpenEra::Kernel::ShimRegistry;
using OpenEra::Kernel::StubRegistry;

namespace
{
using CatchIntFunction = int(int);
using ThrowIntFunction = void(int);
using RaiseAndHandleFunction = int();
using RtlPcToFileHeaderFunction = PVOID NTAPI(PVOID, PVOID*);

constexpr std::string_view kImage = "EhImage.dll";

class EhFixture
{
public:
    EhFixture()
    {
        REQUIRE(RegisterLoaderShims(m_shims));
        m_manager = std::make_unique<ModuleManager>(DirectoryModuleReader(OPENERA_EH_IMAGE_DIR), m_shims, m_stubs,
                                                    *m_allocator, m_log, m_host.get());
        ModuleManager::SetActive(m_manager.get());
        REQUIRE(m_manager->Load(kImage) == ModuleError::None);
    }

    ~EhFixture() { ModuleManager::SetActive(nullptr); }

    EhFixture(const EhFixture&) = delete;
    EhFixture& operator=(const EhFixture&) = delete;
    EhFixture(EhFixture&&) = delete;
    EhFixture& operator=(EhFixture&&) = delete;

    template <typename Function>
    [[nodiscard]] Function* Export(std::string_view symbol)
    {
        const auto address = m_manager->FindExport(kImage, symbol);
        REQUIRE(address.has_value());
        return std::bit_cast<Function*>(address.value_or(0));
    }

    [[nodiscard]] const ShimRegistry& Shims() const noexcept { return m_shims; }
    [[nodiscard]] ModuleManager& Manager() const noexcept { return *m_manager; }

private:
    Logger m_log;
    StubRegistry m_stubs{m_log};
    ShimRegistry m_shims;
    std::unique_ptr<ImageAllocator> m_allocator = CreateVirtualImageAllocator();
    std::unique_ptr<HostModules> m_host = HostModules::CreateDefault();
    std::unique_ptr<ModuleManager> m_manager;
};

int ThrowAndCatch(ThrowIntFunction* throwInt, int value)
{
    try
    {
        throwInt(value);
    }
    catch (const int caught)
    {
        return caught;
    }
    return 0;
}
} // namespace

TEST_CASE("Mapped images link against the C runtime without stubs", "[exceptions]")
{
    const EhFixture fixture;
    const auto* module = fixture.Manager().Find(kImage);
    REQUIRE(module != nullptr);
    REQUIRE(module->Imports.has_value());
    CHECK(module->Imports->CountFrom(ImportSource::Stub) == 0);
    CHECK(module->Imports->CountFrom(ImportSource::Shim) >= 1); // _CxxThrowException
    CHECK(module->Unwind != nullptr);
}

TEST_CASE("C++ exceptions are caught inside a mapped image", "[exceptions]")
{
    EhFixture fixture;
    auto* const catchInt = fixture.Export<CatchIntFunction>("CatchInt");
    CHECK(catchInt(5) == 6);
    CHECK(catchInt(0) == 0);
}

TEST_CASE("C++ exceptions propagate out of a mapped image", "[exceptions]")
{
    EhFixture fixture;
    auto* const throwInt = fixture.Export<ThrowIntFunction>("ThrowInt");
    CHECK(ThrowAndCatch(throwInt, 42) == 42);
}

TEST_CASE("Structured exceptions are handled inside a mapped image", "[exceptions]")
{
    EhFixture fixture;
    auto* const raiseAndHandle = fixture.Export<RaiseAndHandleFunction>("RaiseAndHandle");
    CHECK(raiseAndHandle() == 7);
}

TEST_CASE("RtlPcToFileHeader finds mapped and host images", "[exceptions]")
{
    const EhFixture fixture;
    const auto shim = fixture.Shims().Resolve("ntdll.dll", "RtlPcToFileHeader");
    REQUIRE(shim.has_value());
    auto* const pcToFileHeader = FromShimProc<RtlPcToFileHeaderFunction>(shim->Export->Proc);

    const auto* module = fixture.Manager().Find(kImage);
    REQUIRE(module != nullptr);
    const auto inImage = fixture.Manager().FindExport(kImage, "CatchInt");
    REQUIRE(inImage.has_value());
    PVOID base = nullptr;
    CHECK(std::bit_cast<uint64_t>(pcToFileHeader(std::bit_cast<PVOID>(inImage.value_or(0)), &base)) == module->Base());
    CHECK(std::bit_cast<uint64_t>(base) == module->Base());

    // Anything else goes to the OS: this test binary's own code.
    const HMODULE self = GetModuleHandleW(nullptr);
    const auto hostCode = std::bit_cast<uint64_t>(&ThrowAndCatch);
    CHECK(pcToFileHeader(std::bit_cast<PVOID>(hostCode), &base) == self);
    CHECK(fixture.Manager().ImageBaseFor(hostCode) == 0);
}
