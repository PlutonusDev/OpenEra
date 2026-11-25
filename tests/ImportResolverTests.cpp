#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"
#include "Loader/HostModules.h"
#include "Loader/ImageMapper.h"
#include "Loader/ImageMemory.h"
#include "Loader/ImportResolver.h"
#include "Loader/PeReader.h"
#include "Support/FakeImageMemory.h"

using namespace OpenEra::Loader;
using OpenEra::Core::Logger;
using OpenEra::Core::LogRecord;
using OpenEra::Core::LogSink;
using OpenEra::Kernel::ShimModule;
using OpenEra::Kernel::ShimRegistry;
using OpenEra::Kernel::StubKind;
using OpenEra::Kernel::StubRegistry;
using OpenEra::Kernel::ToShimProc;
using OpenEra::Tests::FakeAllocator;
using OpenEra::Tests::ProtectCall;

namespace
{
constexpr uint64_t kImageBase = 0x1'4000'0000ULL;
constexpr uint32_t kSizeOfImage = 0x3000;
constexpr uint32_t kMathIat = 0x2000;
constexpr uint32_t kMissingIat = 0x2020;
constexpr uint32_t kXboxIat = 0x2040;
constexpr uint32_t kDelayIat = 0x2060;

int Add(int left, int right)
{
    return left + right;
}

int Negate(int value)
{
    return -value;
}

int There()
{
    return 1;
}

PeImportModule MakeModule(std::string_view name, uint32_t iat, std::vector<PeImportEntry> entries)
{
    return {.DllName = std::string(name), .ImportAddressTableRva = iat, .Entries = std::move(entries)};
}

PeImportEntry Named(std::string_view name)
{
    return {.Ordinal = std::nullopt, .Hint = 0, .Name = std::string(name)};
}

PeImportEntry ByOrdinal(uint16_t ordinal)
{
    return {.Ordinal = ordinal, .Hint = 0, .Name = {}};
}

PeImage MakeImage()
{
    PeImage pe;
    pe.OptionalHeader.ImageBase = kImageBase;
    pe.OptionalHeader.SizeOfImage = kSizeOfImage;
    pe.OptionalHeader.SizeOfHeaders = 0x400;
    pe.OptionalHeader.SectionAlignment = 0x1000;

    SectionHeader text{};
    text.VirtualAddress = 0x1000;
    text.VirtualSize = 0x100;
    text.Characteristics = kSectionMemoryExecute | kSectionMemoryRead;
    SectionHeader data{};
    data.VirtualAddress = 0x2000;
    data.VirtualSize = 0x400;
    data.Characteristics = kSectionMemoryRead | kSectionMemoryWrite;
    pe.Sections = {text, data};

    pe.Imports = {
        MakeModule("Math.dll", kMathIat, {Named("Add"), ByOrdinal(7)}),
        MakeModule("Missing.dll", kMissingIat, {Named("Gone"), ByOrdinal(3)}),
        MakeModule("xbox.dll", kXboxIat, {Named("NotThere")}),
    };
    pe.DelayImports = {MakeModule("MATH", kDelayIat, {Named("Add")})};
    return pe;
}

ShimRegistry MakeShims()
{
    ShimRegistry shims;
    ShimModule math("Math.dll", StubKind::Win32Import);
    REQUIRE(math.Export("Add", ToShimProc(&Add)));
    REQUIRE(math.ExportOrdinal(7, ToShimProc(&Negate)));
    const bool mathRegistered = shims.Register(std::move(math));
    REQUIRE(mathRegistered);

    ShimModule xbox("Xbox.dll", StubKind::GameCoreApi);
    REQUIRE(xbox.Export("There", ToShimProc(&There)));
    const bool xboxRegistered = shims.Register(std::move(xbox));
    REQUIRE(xboxRegistered);
    return shims;
}

uint64_t ReadSlot(const MappedImage& image, uint32_t rva)
{
    std::array<std::byte, sizeof(uint64_t)> raw{};
    std::ranges::copy(image.Memory->Bytes().subspan(rva, raw.size()), raw.begin());
    return std::bit_cast<uint64_t>(raw);
}

class CaptureSink final : public LogSink
{
public:
    void Write(const LogRecord& record) override { m_messages.emplace_back(record.Message); }

    [[nodiscard]] bool Contains(std::string_view text) const
    {
        return std::ranges::any_of(m_messages, [text](const std::string& message)
                                   { return message.find(text) != std::string::npos; });
    }

private:
    std::vector<std::string> m_messages;
};

// An image mapped into fake memory, plus everything ResolveImports needs.
struct Fixture
{
    Logger Log;
    std::shared_ptr<CaptureSink> Sink = std::make_shared<CaptureSink>();
    StubRegistry Stubs{Log};
    ShimRegistry Shims = MakeShims();
    PeImage Pe = MakeImage();
    FakeAllocator ImageFake;
    MappedImage Image;
    std::unique_ptr<OpenEra::Loader::ImageAllocator> StubAllocator = CreateVirtualImageAllocator();

    Fixture()
    {
        Log.AddSink(Sink);
        Image.Memory = ImageFake.Allocate(kImageBase, kSizeOfImage);
        Image.PreferredBase = kImageBase;
    }

    ResolveResult Resolve() { return ResolveImports(Image, Pe, Shims, Stubs, *StubAllocator, Log); }

    ResolveResult Resolve(OpenEra::Loader::ImageAllocator& stubAllocator)
    {
        return ResolveImports(Image, Pe, Shims, Stubs, stubAllocator, Log);
    }

    ResolveResult Resolve(ImportSources sources)
    {
        return ResolveImports(Image, Pe, Shims, Stubs, *StubAllocator, Log, sources);
    }
};

// Returns fixed addresses for one module, to stand in for title modules.
class FixedSource final : public SymbolSource
{
public:
    std::optional<uint64_t> FindExport(std::string_view module, std::string_view symbol) override
    {
        return module == "Missing.dll" && symbol == "Gone" ? std::optional<uint64_t>(0x7777'0000) : std::nullopt;
    }

    std::optional<uint64_t> FindExport(std::string_view module, uint16_t ordinal) override
    {
        return module == "Math.dll" && ordinal == 7 ? std::optional<uint64_t>(0x7777'0007) : std::nullopt;
    }
};
} // namespace

TEST_CASE("DefaultStubReturnValue picks a not-implemented value per stub kind", "[ImportResolver]")
{
    CHECK(DefaultStubReturnValue(StubKind::Win32Import) == 0);
    CHECK(DefaultStubReturnValue(StubKind::GameCoreApi) == 0x80004001U);
    CHECK(DefaultStubReturnValue(StubKind::EraApi) == 0x80004001U);
    CHECK(DefaultStubReturnValue(StubKind::NtSyscall) == 0xC0000002U);
}

TEST_CASE("ResolveImports binds shims by name, ordinal and delay-load", "[ImportResolver]")
{
    Fixture fixture;
    const auto result = fixture.Resolve();
    REQUIRE(result.Error == ResolveError::None);
    const auto& imports = result.Imports.value();

    CHECK(ReadSlot(fixture.Image, kMathIat) == std::bit_cast<uint64_t>(&Add));
    CHECK(std::bit_cast<int (*)(int, int)>(ReadSlot(fixture.Image, kMathIat))(2, 3) == 5);
    CHECK(std::bit_cast<int (*)(int)>(ReadSlot(fixture.Image, kMathIat + 8))(4) == -4);
    CHECK(ReadSlot(fixture.Image, kDelayIat) == std::bit_cast<uint64_t>(&Add)); // Delay imports bind eagerly.

    REQUIRE(imports.Bindings.size() == 6);
    CHECK(imports.StubbedCount() == 3);
    CHECK(imports.Bindings.at(0).Source == ImportSource::Shim);
    CHECK(imports.Bindings.at(1).Ordinal == uint16_t{7});
    CHECK(imports.Bindings.at(2).Source == ImportSource::Stub);
    CHECK(imports.Bindings.at(2).Symbol == "Gone");
    CHECK(imports.Bindings.at(5).Delayed);
    CHECK(fixture.Sink->Contains("Imports: 0 from title modules, 3 shimmed, 0 passed through, 3 stubbed"));
}

TEST_CASE("Unresolved imports call stubs that record hits and return a failure value", "[ImportResolver]")
{
    Fixture fixture;
    const auto result = fixture.Resolve();
    REQUIRE(result.Error == ResolveError::None);

    // Win32 import from an unknown module: returns 0, records the symbol. Arguments are ignored.
    const auto gone = std::bit_cast<uint64_t (*)(int, int)>(ReadSlot(fixture.Image, kMissingIat));
    CHECK(gone(1, 2) == 0);
    CHECK(gone(3, 4) == 0);
    CHECK(fixture.Stubs.GetHitCount(StubKind::Win32Import, "Missing.dll", "Gone") == 2);

    const auto byOrdinal = std::bit_cast<uint64_t (*)()>(ReadSlot(fixture.Image, kMissingIat + 8));
    CHECK(byOrdinal() == 0);
    CHECK(fixture.Stubs.GetHitCount(StubKind::Win32Import, "Missing.dll", uint32_t{3}) == 1);

    // Missing symbol in a registered GameCore module: takes the module's kind and return value.
    const auto notThere = std::bit_cast<uint64_t (*)()>(ReadSlot(fixture.Image, kXboxIat));
    CHECK(static_cast<uint32_t>(notThere()) == 0x80004001U);
    CHECK(fixture.Stubs.GetHitCount(StubKind::GameCoreApi, "xbox.dll", "NotThere") == 1);

    CHECK(fixture.Sink->Contains("Unimplemented Win32Import Missing.dll!Gone (unresolved import)"));
    CHECK(fixture.Stubs.UniqueCount() == 3);
}

TEST_CASE("ResolveImports restores the image's protections after writing", "[ImportResolver]")
{
    Fixture fixture;
    REQUIRE(fixture.Resolve().Error == ResolveError::None);
    CHECK(fixture.ImageFake.Calls == std::vector<ProtectCall>{
                                         {.Offset = 0, .Size = kSizeOfImage, .Protection = PageProtection::ReadWrite},
                                         {.Offset = 0, .Size = 0x1000, .Protection = PageProtection::ReadOnly},
                                         {.Offset = 0x1000, .Size = 0x1000, .Protection = PageProtection::ExecuteRead},
                                         {.Offset = 0x2000, .Size = 0x1000, .Protection = PageProtection::ReadWrite},
                                     });
}

TEST_CASE("ResolveImports skips work it doesn't need", "[ImportResolver]")
{
    Fixture fixture;
    FakeAllocator stubAllocator;

    SECTION("no imports")
    {
        fixture.Pe.Imports.clear();
        fixture.Pe.DelayImports.clear();
        const auto result = fixture.Resolve(stubAllocator);
        REQUIRE(result.Error == ResolveError::None);
        CHECK(result.Imports->Bindings.empty());
        CHECK(fixture.ImageFake.Calls.empty());
    }

    SECTION("everything resolves")
    {
        fixture.Pe.Imports.resize(1);
        const auto result = fixture.Resolve(stubAllocator);
        REQUIRE(result.Error == ResolveError::None);
        CHECK(result.Imports->StubbedCount() == 0);
    }

    CHECK(stubAllocator.RequestedSize == 0); // No stub memory allocated.
}

TEST_CASE("ResolveImports reports failures without partial writes", "[ImportResolver]")
{
    Fixture fixture;
    FakeAllocator stubAllocator;

    SECTION("address table outside the image")
    {
        fixture.Pe.Imports.at(1).ImportAddressTableRva = kSizeOfImage - 8;
        CHECK(fixture.Resolve().Error == ResolveError::BadImportTable);
        CHECK(fixture.Sink->Contains("slot at RVA 0x3000 is outside the image"));
        CHECK(ReadSlot(fixture.Image, kMathIat) == 0);
    }

    SECTION("image not mapped")
    {
        fixture.Image.Memory.reset();
        CHECK(fixture.Resolve().Error == ResolveError::BadImportTable);
    }

    SECTION("stub memory allocation fails")
    {
        stubAllocator.Fail = true;
        CHECK(fixture.Resolve(stubAllocator).Error == ResolveError::StubAllocationFailed);
        CHECK(fixture.Sink->Contains("could not allocate 0x1000 bytes for 3 stubs"));
        CHECK(ReadSlot(fixture.Image, kMathIat) == 0);
    }

    SECTION("stub memory can't be made executable")
    {
        stubAllocator.FailProtect = true;
        CHECK(fixture.Resolve(stubAllocator).Error == ResolveError::ProtectionFailed);
        CHECK(ReadSlot(fixture.Image, kMathIat) == 0);
    }

    SECTION("image can't be made writable")
    {
        auto failing = FakeAllocator{};
        failing.FailProtect = true;
        fixture.Image.Memory = failing.Allocate(kImageBase, kSizeOfImage);
        CHECK(fixture.Resolve().Error == ResolveError::ProtectionFailed);
        CHECK(fixture.Sink->Contains("could not make the image writable"));
    }
}

TEST_CASE("Resolver enum names", "[ImportResolver]")
{
    CHECK(ToString(ResolveError::StubAllocationFailed) == "StubAllocationFailed");
    CHECK(ToString(ImportSource::Host) == "Host");
}

TEST_CASE("Imports resolve as title module, then shim, then host, then stub", "[ImportResolver]")
{
    Fixture fixture;
    constexpr uint32_t kKernelIat = 0x2080;
    fixture.Pe.Imports.push_back(
        MakeModule("KERNEL32.dll", kKernelIat, {Named("GetTickCount64"), Named("NoSuchExport")}));
    const auto host = HostModules::CreateDefault();
    const auto realTicks = host->FindExport("kernel32", "GetTickCount64");
    REQUIRE(realTicks.has_value());

    SECTION("host passthrough for allowed modules")
    {
        const auto result = fixture.Resolve({.TitleModules = nullptr, .Host = host.get()});
        REQUIRE(result.Error == ResolveError::None);
        const auto& imports = result.Imports.value();

        CHECK(ReadSlot(fixture.Image, kKernelIat) == *realTicks);
        CHECK(imports.Bindings.at(5).Source == ImportSource::Host);
        CHECK(imports.Bindings.at(6).Source == ImportSource::Stub); // Not exported by the host.
        CHECK(imports.Bindings.at(4).Source == ImportSource::Stub); // xbox.dll: no shim and not allowed.
        CHECK(imports.CountFrom(ImportSource::Shim) == 3);
        CHECK(imports.CountFrom(ImportSource::Host) == 1);
        CHECK(fixture.Sink->Contains("Imports: 0 from title modules, 3 shimmed, 1 passed through, 4 stubbed"));
    }

    SECTION("a shim overrides the host export")
    {
        ShimModule kernel("kernel32.dll", StubKind::Win32Import);
        REQUIRE(kernel.Export("GetTickCount64", ToShimProc(&There)));
        const bool registered = fixture.Shims.Register(std::move(kernel));
        REQUIRE(registered);

        const auto result = fixture.Resolve({.TitleModules = nullptr, .Host = host.get()});
        REQUIRE(result.Error == ResolveError::None);
        CHECK(ReadSlot(fixture.Image, kKernelIat) == std::bit_cast<uint64_t>(&There));
        CHECK(result.Imports->Bindings.at(5).Source == ImportSource::Shim);
    }

    SECTION("title modules win over shims and stubs")
    {
        FixedSource title;
        const auto result = fixture.Resolve({.TitleModules = &title, .Host = host.get()});
        REQUIRE(result.Error == ResolveError::None);
        CHECK(ReadSlot(fixture.Image, kMissingIat) == 0x7777'0000);
        CHECK(ReadSlot(fixture.Image, kMathIat + 8) == 0x7777'0007); // Over the Math.dll ordinal 7 shim.
        CHECK(result.Imports->Bindings.at(1).Source == ImportSource::TitleModule);
        CHECK(result.Imports->CountFrom(ImportSource::TitleModule) == 2);
    }

    SECTION("without a host source, allowed modules still stub")
    {
        const auto result = fixture.Resolve();
        REQUIRE(result.Error == ResolveError::None);
        CHECK(result.Imports->Bindings.at(5).Source == ImportSource::Stub);
    }
}
