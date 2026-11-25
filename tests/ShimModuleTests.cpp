#include <bit>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"

using namespace OpenEra::Kernel;
using OpenEra::Core::LogRecord;
using OpenEra::Core::LogSink;

namespace
{
int Add(int left, int right)
{
    return left + right;
}

int Negate(int value)
{
    return -value;
}

class CaptureSink final : public LogSink
{
public:
    void Write(const LogRecord& record) override { m_messages.emplace_back(record.Message); }

    [[nodiscard]] const std::vector<std::string>& Messages() const noexcept { return m_messages; }

private:
    std::vector<std::string> m_messages;
};

// Captures DefaultLogger output for the lifetime of the test.
struct DefaultLogCapture
{
    std::shared_ptr<CaptureSink> Sink = std::make_shared<CaptureSink>();

    DefaultLogCapture() { OpenEra::Core::DefaultLogger().AddSink(Sink); }
    ~DefaultLogCapture() { OpenEra::Core::DefaultLogger().ClearSinks(); }

    DefaultLogCapture(const DefaultLogCapture&) = delete;
    DefaultLogCapture& operator=(const DefaultLogCapture&) = delete;
    DefaultLogCapture(DefaultLogCapture&&) = delete;
    DefaultLogCapture& operator=(DefaultLogCapture&&) = delete;
};

ShimModule MakeMathModule()
{
    ShimModule module("Math.dll", StubKind::Win32Import);
    REQUIRE(module.Export("Add", ToShimProc(&Add), uint16_t{1}));
    REQUIRE(module.ExportOrdinal(7, ToShimProc(&Negate)));
    module.AddAlias("api-ms-win-test-math-l1-1-0");
    return module;
}
} // namespace

TEST_CASE("ShimModule exports by name and ordinal", "[ShimModule]")
{
    auto module = MakeMathModule();
    CHECK(module.Name() == "Math.dll");
    CHECK(module.Kind() == StubKind::Win32Import);
    CHECK(module.ExportCount() == 2);

    const auto* add = module.Find("Add");
    REQUIRE(add != nullptr);
    CHECK(add->Ordinal == uint16_t{1});
    CHECK(module.Find(uint16_t{1}) == add); // Named export also reachable by its ordinal.
    CHECK(FromShimProc<int(int, int)>(add->Proc)(2, 3) == 5);

    const auto* negate = module.Find(uint16_t{7});
    REQUIRE(negate != nullptr);
    CHECK(negate->Name.empty());
    CHECK(FromShimProc<int(int)>(negate->Proc)(4) == -4);

    // Symbol names are case-sensitive, as in PE export tables.
    CHECK(module.Find("add") == nullptr);
    CHECK(module.Find("Missing") == nullptr);
    CHECK(module.Find(uint16_t{2}) == nullptr);
}

TEST_CASE("ShimModule rejects duplicate exports", "[ShimModule]")
{
    auto module = MakeMathModule();
    CHECK_FALSE(module.Export("Add", ToShimProc(&Negate)));
    CHECK_FALSE(module.Export("Other", ToShimProc(&Negate), uint16_t{7}));
    CHECK_FALSE(module.ExportOrdinal(1, ToShimProc(&Negate)));
    CHECK_FALSE(module.Export("", ToShimProc(&Negate)));
    CHECK(module.ExportCount() == 2);
    CHECK(module.Find("Other") == nullptr);
    CHECK(FromShimProc<int(int, int)>(module.Find("Add")->Proc)(1, 1) == 2);
}

TEST_CASE("NormalizeModuleName strips a .dll suffix in any case", "[ShimModule]")
{
    CHECK(NormalizeModuleName("KERNEL32.dll") == "KERNEL32");
    CHECK(NormalizeModuleName("kernel32.DLL") == "kernel32");
    CHECK(NormalizeModuleName("kernel32") == "kernel32");
    CHECK(NormalizeModuleName("api-ms-win-core-file-l1-1-0.dll") == "api-ms-win-core-file-l1-1-0");
    CHECK(NormalizeModuleName(".dll") == ".dll");
    CHECK(NormalizeModuleName("").empty());
}

TEST_CASE("ShimRegistry resolves modules case-insensitively, with aliases", "[ShimModule]")
{
    ShimRegistry registry;
    REQUIRE(registry.Register(MakeMathModule()));
    CHECK(registry.ModuleCount() == 1);

    const auto* module = registry.FindModule("Math.dll");
    REQUIRE(module != nullptr);
    CHECK(registry.FindModule("MATH.DLL") == module);
    CHECK(registry.FindModule("math") == module);
    CHECK(registry.FindModule("API-MS-WIN-TEST-MATH-L1-1-0.dll") == module);
    CHECK(registry.FindModule("api-ms-win-test-math-l1-1-1") == nullptr); // Versions are not merged.
    CHECK(registry.FindModule("Other.dll") == nullptr);

    const auto byName = registry.Resolve("math.DLL", "Add");
    REQUIRE(byName.has_value());
    CHECK(byName->Module == module);
    CHECK(FromShimProc<int(int, int)>(byName->Export->Proc)(20, 22) == 42);

    const auto byOrdinal = registry.Resolve("Math", uint16_t{7});
    REQUIRE(byOrdinal.has_value());
    CHECK(FromShimProc<int(int)>(byOrdinal->Export->Proc)(9) == -9);

    CHECK_FALSE(registry.Resolve("Math.dll", "Missing").has_value());
    CHECK_FALSE(registry.Resolve("Math.dll", uint16_t{99}).has_value());
    CHECK_FALSE(registry.Resolve("Other.dll", "Add").has_value());
}

TEST_CASE("ShimRegistry rejects name and alias collisions", "[ShimModule]")
{
    ShimRegistry registry;
    REQUIRE(registry.Register(MakeMathModule()));

    CHECK_FALSE(registry.Register(ShimModule("MATH", StubKind::EraApi)));

    ShimModule aliasClash("Different.dll", StubKind::EraApi);
    aliasClash.AddAlias("Math.dll");
    const bool aliasClashRegistered = registry.Register(std::move(aliasClash));
    CHECK_FALSE(aliasClashRegistered);

    ShimModule aliasNameClash("api-ms-win-test-math-l1-1-0", StubKind::EraApi);
    const bool aliasNameClashRegistered = registry.Register(std::move(aliasNameClash));
    CHECK_FALSE(aliasNameClashRegistered);

    CHECK(registry.ModuleCount() == 1);
    CHECK(registry.FindModule("Different") == nullptr);
}

TEST_CASE("ShimRegistry keeps earlier lookups valid as modules are added", "[ShimModule]")
{
    ShimRegistry registry;
    REQUIRE(registry.Register(MakeMathModule()));
    const auto first = registry.Resolve("Math.dll", "Add");
    REQUIRE(first.has_value());

    for (int index = 0; index < 100; ++index)
    {
        ShimModule filler("Filler" + std::to_string(index), StubKind::GameCoreApi);
        REQUIRE(filler.Export("Fn", ToShimProc(&Negate)));
        const bool registered = registry.Register(std::move(filler));
        REQUIRE(registered);
    }

    CHECK(registry.FindModule("Math.dll") == first->Module);
    CHECK(registry.Resolve("Math.dll", "Add")->Export == first->Export);
    CHECK(FromShimProc<int(int, int)>(first->Export->Proc)(1, 2) == 3);
}

TEST_CASE("Status codes match the documented values", "[ShimGuard]")
{
    STATIC_REQUIRE(std::bit_cast<uint32_t>(kHResultNotImplemented) == 0x80004001U);
    STATIC_REQUIRE(std::bit_cast<uint32_t>(kHResultFail) == 0x80004005U);
    STATIC_REQUIRE(std::bit_cast<uint32_t>(kHResultOutOfMemory) == 0x8007000EU);
    STATIC_REQUIRE(std::bit_cast<uint32_t>(kHResultInvalidArgument) == 0x80070057U);
    STATIC_REQUIRE(std::bit_cast<uint32_t>(kStatusUnsuccessful) == 0xC0000001U);
    STATIC_REQUIRE(std::bit_cast<uint32_t>(kStatusNotImplemented) == 0xC0000002U);
    STATIC_REQUIRE(std::bit_cast<uint32_t>(kStatusInvalidParameter) == 0xC000000DU);
    STATIC_REQUIRE(std::bit_cast<uint32_t>(kStatusNoMemory) == 0xC0000017U);
    STATIC_REQUIRE(kHResultFail < 0); // Failure HRESULTs and NTSTATUS errors are negative.
    STATIC_REQUIRE(kStatusNoMemory < 0);
}

TEST_CASE("GuardShim translates exceptions into status codes", "[ShimGuard]")
{
    const DefaultLogCapture capture;

    SECTION("success passes the body's result through")
    {
        CHECK(GuardHResult("Ok", [] { return kHResultOk; }) == kHResultOk);
        CHECK(GuardNtStatus("Ok", [] { return kStatusNotImplemented; }) == kStatusNotImplemented);
        CHECK(capture.Sink->Messages().empty());
    }

    SECTION("std::exception becomes the error value and is logged")
    {
        CHECK(GuardHResult("CreateThing", []() -> HResult { throw std::runtime_error("boom"); }) == kHResultFail);
        CHECK(GuardNtStatus("NtThing", []() -> NtStatus { throw std::logic_error("bad"); }) == kStatusUnsuccessful);
        REQUIRE(capture.Sink->Messages().size() == 2);
        CHECK(capture.Sink->Messages().at(0) == "Shim CreateThing threw: boom");
        CHECK(capture.Sink->Messages().at(1) == "Shim NtThing threw: bad");
    }

    SECTION("std::bad_alloc becomes the out-of-memory value")
    {
        CHECK(GuardHResult("Alloc", []() -> HResult { throw std::bad_alloc(); }) == kHResultOutOfMemory);
        CHECK(GuardNtStatus("Alloc", []() -> NtStatus { throw std::bad_alloc(); }) == kStatusNoMemory);
        CHECK(capture.Sink->Messages().at(0) == "Shim Alloc threw: out of memory");
    }

    SECTION("non-standard exceptions are caught too")
    {
        CHECK(GuardShim<int>("Weird", -1, -2, []() -> int { throw 42; }) == -1);
        CHECK(capture.Sink->Messages().at(0) == "Shim Weird threw: unknown exception");
    }
}

TEST_CASE("ReportPartialShim records a stub hit", "[ShimGuard]")
{
    auto& stubs = DefaultStubRegistry();
    stubs.Reset();

    ReportPartialShim(StubKind::GameCoreApi, "xgameruntime.dll", "XUserAddAsync", "offline user only");
    ReportPartialShim(StubKind::GameCoreApi, "xgameruntime.dll", "XUserAddAsync", "offline user only");

    CHECK(stubs.GetHitCount(StubKind::GameCoreApi, "xgameruntime.dll", "XUserAddAsync") == 2);
    const auto records = stubs.GetAllRecords();
    REQUIRE(records.size() == 1);
    CHECK(records.at(0).Note == "offline user only");
    stubs.Reset();
}
