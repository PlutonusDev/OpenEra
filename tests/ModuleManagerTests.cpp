#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"
#include "Loader/HostModules.h"
#include "Loader/ImportResolver.h"
#include "Loader/ModuleManager.h"
#include "Support/FakeImageMemory.h"
#include "Support/SyntheticPe.h"

using namespace OpenEra::Loader;
using OpenEra::Core::Logger;
using OpenEra::Core::LogRecord;
using OpenEra::Core::LogSink;
using OpenEra::Kernel::FromShimProc;
using OpenEra::Kernel::ShimModule;
using OpenEra::Kernel::ShimRegistry;
using OpenEra::Kernel::StubKind;
using OpenEra::Kernel::StubRegistry;
using OpenEra::Kernel::ToShimProc;
using OpenEra::Tests::FakeAllocator;
using OpenEra::Tests::SyntheticPe;

namespace
{
constexpr uint64_t kGameBase = 0x1'4000'0000ULL;
constexpr uint64_t kLibABase = 0x1'8000'0000ULL;
constexpr uint64_t kLibBBase = 0x1'8010'0000ULL;
constexpr uint64_t kLibCBase = 0x1'8020'0000ULL;

int TestHello()
{
    return 7;
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

class RecordingInitializer final : public ModuleInitializer
{
public:
    std::vector<std::string> Events;
    std::string FailOn;

    bool Attach(const LoadedModule& module) override
    {
        Events.push_back("attach " + module.Name);
        return module.Name != FailOn;
    }

    void Detach(const LoadedModule& module) override { Events.push_back("detach " + module.Name); }
};

// liba.dll and libb.dll import from each other; game.exe imports from both,
// from kernel32 and from a module the title doesn't ship.
struct Package
{
    SyntheticPe LibA{kLibABase, "liba.dll"};
    SyntheticPe LibB{kLibBBase, "libb.dll"};
    SyntheticPe Game{kGameBase, "game.exe", false};
    SyntheticPe LibC{kLibCBase, "libc.dll"};
    uint32_t AddRva = LibA.AddData(16, 0xA11);
    uint32_t NoNameRva = LibA.AddData(16, 0x2222);
    uint32_t CounterRva = LibA.AddData(16, 42);
    uint32_t DelayValueRva = LibA.AddData(16, 0x6666);
    uint32_t MulRva = LibB.AddData(16, 0xB0B);
    uint32_t HelloRva = LibC.AddData(16, 1);
    std::map<std::string, std::vector<std::byte>> Files;

    Package()
    {
        LibA.Export("Add", AddRva, 1);
        LibA.ExportOrdinalOnly(NoNameRva, 2);
        LibA.Export("Counter", CounterRva, 3);
        LibA.Forward("Forwarded", "libb.Mul", 4);
        LibA.Export("DelayValue", DelayValueRva, 6);
        LibA.Import("libb.dll", "Mul");

        LibB.Export("Mul", MulRva, 1);
        LibB.Forward("Chain", "liba.Forwarded", 2);
        LibB.Forward("Ticks", "KERNEL32.GetTickCount64", 3);
        LibB.Forward("Loop", "libb.Loop", 4);
        LibB.Forward("ByOrdinal", "liba.#2", 5);
        LibB.Import("liba.dll", "Add");

        Game.Import("liba.dll", "Add");
        Game.Import("liba.dll", uint16_t{2});
        Game.Import("liba.dll", "Counter");
        for (const char* symbol : {"Chain", "Ticks", "Loop", "ByOrdinal"})
        {
            Game.Import("libb.dll", symbol);
        }
        Game.Import("KERNEL32.dll", "GetTickCount64");
        Game.Import("Missing.dll", "Nope");
        Game.DelayImport("liba.dll", 6);
        Game.DelayImport("libc.dll", 1);
        Game.DelayImport("absent.dll", 1);

        LibC.Export("Hello", HelloRva, 1);

        Files["liba.dll"] = LibA.Build();
        Files["libb.dll"] = LibB.Build();
        Files["game.exe"] = Game.Build();
        Files["libc.dll"] = LibC.Build();
        Files["kernel32.dll"] = Files["libc.dll"]; // Must never be loaded as a title module.
        Files["bad.dll"] = std::vector<std::byte>(64, std::byte{0x5A});
    }

    [[nodiscard]] ModuleReader Reader() const
    {
        return [this](std::string_view name) -> std::optional<std::vector<std::byte>>
        {
            std::string key(name);
            std::ranges::transform(key, key.begin(),
                                   [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
            const auto it = Files.find(key);
            return it == Files.end() ? std::nullopt : std::optional(it->second);
        };
    }
};

struct Fixture
{
    Package Files;
    Logger Log;
    std::shared_ptr<CaptureSink> Sink = std::make_shared<CaptureSink>();
    StubRegistry Stubs{Log};
    ShimRegistry Shims;
    FakeAllocator Allocator;
    std::unique_ptr<HostModules> Host = HostModules::CreateDefault();
    std::unique_ptr<ModuleManager> Manager;

    Fixture()
    {
        Log.AddSink(Sink);
        REQUIRE(RegisterLoaderShims(Shims));
        ShimModule test("testshim.dll", StubKind::GameCoreApi);
        REQUIRE(test.Export("Hello", ToShimProc(&TestHello)));
        const bool registered = Shims.Register(std::move(test));
        REQUIRE(registered);
        Manager = std::make_unique<ModuleManager>(Files.Reader(), Shims, Stubs, Allocator, Log, Host.get());
    }

    [[nodiscard]] uint64_t Slot(std::string_view module, const SyntheticPe& image, const std::string& dll,
                                const std::string& symbol) const
    {
        const auto* loaded = Manager->Find(module);
        REQUIRE(loaded != nullptr);
        std::array<std::byte, sizeof(uint64_t)> raw{};
        std::ranges::copy(loaded->Image.Memory->Bytes().subspan(image.SlotRva(dll, symbol), raw.size()), raw.begin());
        return std::bit_cast<uint64_t>(raw);
    }

    [[nodiscard]] ImportSource SourceOf(std::string_view module, std::string_view symbol) const
    {
        const auto& bindings = Manager->Find(module)->Imports->Bindings;
        const auto it =
            std::ranges::find_if(bindings, [symbol](const ImportBinding& binding) { return binding.Symbol == symbol; });
        REQUIRE(it != bindings.end());
        return it->Source;
    }

    template <typename Function>
    [[nodiscard]] Function* Shim(std::string_view name) const
    {
        const auto resolved = Shims.Resolve("kernel32.dll", name);
        REQUIRE(resolved.has_value());
        return FromShimProc<Function>(resolved->Export->Proc);
    }
};

uint64_t RealExport(const char* name)
{
    return std::bit_cast<uint64_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), name));
}

HMODULE AsModule(uint64_t base)
{
    return std::bit_cast<HMODULE>(base);
}

const char* OrdinalName(uintptr_t ordinal)
{
    return std::bit_cast<const char*>(ordinal);
}
} // namespace

TEST_CASE("System module names are reserved", "[ModuleManager]")
{
    for (const char* reserved :
         {"KERNEL32.dll", "ntdll", "ucrtbase.dll", "d3d12.dll", "xgameruntime.dll", "GameInput.dll", "XAudio2_9.dll",
          "api-ms-win-crt-runtime-l1-1-0.dll", "ext-ms-win-anything-l1-1-0.dll", R"(C:\pkg\kernel32.dll)"})
    {
        INFO(reserved);
        CHECK(IsReservedModuleName(reserved));
    }
    for (const char* title : {"liba.dll", "game.exe", "physics.dll"})
    {
        INFO(title);
        CHECK_FALSE(IsReservedModuleName(title));
    }
}

TEST_CASE("ModuleManager links title modules to each other", "[ModuleManager]")
{
    const Fixture fixture;
    REQUIRE(fixture.Manager->Load("game.exe") == ModuleError::None);
    CHECK(fixture.Manager->ModuleCount() == 3); // libc.dll and absent.dll are only delay-imported.
    CHECK(fixture.Manager->Find("LIBA.DLL")->Base() == kLibABase);
    CHECK(fixture.Manager->MainModuleHandle() == kGameBase);

    const auto& game = fixture.Files.Game;
    // By name, by ordinal (a no-name export) and a data export.
    CHECK(fixture.Slot("game.exe", game, "liba.dll", "Add") == kLibABase + fixture.Files.AddRva);
    CHECK(fixture.Slot("game.exe", game, "liba.dll", "#2") == kLibABase + fixture.Files.NoNameRva);
    CHECK(fixture.Slot("game.exe", game, "liba.dll", "Counter") == kLibABase + fixture.Files.CounterRva);
    // Delay imports are left for the image's own helper.
    CHECK(fixture.Slot("game.exe", game, "liba.dll", "#6") == 0);
    CHECK(fixture.Slot("game.exe", game, "libc.dll", "#1") == 0);

    // Forwarders: a two-hop chain, an ordinal target, and one into the host's kernel32.
    CHECK(fixture.Slot("game.exe", game, "libb.dll", "Chain") == kLibBBase + fixture.Files.MulRva);
    CHECK(fixture.Slot("game.exe", game, "libb.dll", "ByOrdinal") == kLibABase + fixture.Files.NoNameRva);
    CHECK(fixture.Slot("game.exe", game, "libb.dll", "Ticks") == RealExport("GetTickCount64"));

    // A self-referencing forwarder stops at the depth limit and becomes a stub.
    CHECK(fixture.SourceOf("game.exe", "Loop") == ImportSource::Stub);
    CHECK(fixture.Sink->Contains("forwarder chain deeper than 16"));

    // Import cycle: each library is bound to the other.
    CHECK(fixture.Slot("liba.dll", fixture.Files.LibA, "libb.dll", "Mul") == kLibBBase + fixture.Files.MulRva);
    CHECK(fixture.Slot("libb.dll", fixture.Files.LibB, "liba.dll", "Add") == kLibABase + fixture.Files.AddRva);

    // kernel32 comes from the host even though the package contains a "kernel32.dll".
    CHECK(fixture.SourceOf("game.exe", "GetTickCount64") == ImportSource::Host);
    CHECK(fixture.Slot("game.exe", game, "KERNEL32.dll", "GetTickCount64") == RealExport("GetTickCount64"));
    CHECK(fixture.SourceOf("game.exe", "Nope") == ImportSource::Stub);
    CHECK(fixture.SourceOf("game.exe", "Add") == ImportSource::TitleModule);
}

TEST_CASE("ModuleManager attaches dependencies first and detaches in reverse", "[ModuleManager]")
{
    const Fixture fixture;
    RecordingInitializer initializer;
    fixture.Manager->SetInitializer(&initializer);

    REQUIRE(fixture.Manager->Load("game.exe") == ModuleError::None);
    CHECK(fixture.Manager->AttachOrder() == std::vector<std::string>{"libb.dll", "liba.dll", "game.exe"});

    // Loading something already loaded attaches nothing new.
    REQUIRE(fixture.Manager->Load("liba.dll") == ModuleError::None);
    CHECK(initializer.Events.size() == 3);

    fixture.Manager->Shutdown();
    CHECK(initializer.Events == std::vector<std::string>{"attach libb.dll", "attach liba.dll", "attach game.exe",
                                                         "detach game.exe", "detach liba.dll", "detach libb.dll"});
    CHECK(fixture.Manager->AttachOrder().empty());
}

TEST_CASE("ModuleManager reports load failures", "[ModuleManager]")
{
    const Fixture fixture;

    SECTION("module not in the package")
    {
        CHECK(fixture.Manager->Load("missing.dll") == ModuleError::NotFound);
    }

    SECTION("system module name, even when the package has the file")
    {
        CHECK(fixture.Manager->Load("kernel32.dll") == ModuleError::Reserved);
        CHECK(fixture.Sink->Contains("never loaded from the title"));
        CHECK(fixture.Manager->ModuleCount() == 0);
    }

    SECTION("not a PE image")
    {
        CHECK(fixture.Manager->Load("bad.dll") == ModuleError::BadImage);
    }

    SECTION("an initializer refuses a module")
    {
        RecordingInitializer initializer;
        initializer.FailOn = "libb.dll";
        fixture.Manager->SetInitializer(&initializer);
        CHECK(fixture.Manager->Load("game.exe") == ModuleError::AttachFailed);
        CHECK(fixture.Sink->Contains("initialising libb.dll failed"));
    }
}

TEST_CASE("Loader shims back LoadLibrary, GetModuleHandle and GetProcAddress", "[ModuleManager]")
{
    const Fixture fixture;
    REQUIRE(fixture.Manager->Load("game.exe") == ModuleError::None);
    ModuleManager::SetActive(fixture.Manager.get());

    auto* loadW = fixture.Shim<HMODULE(LPCWSTR)>("LoadLibraryW");
    auto* loadA = fixture.Shim<HMODULE(LPCSTR)>("LoadLibraryA");
    auto* loadExW = fixture.Shim<HMODULE(LPCWSTR, HANDLE, DWORD)>("LoadLibraryExW");
    auto* moduleW = fixture.Shim<HMODULE(LPCWSTR)>("GetModuleHandleW");
    auto* getProc = fixture.Shim<FARPROC(HMODULE, LPCSTR)>("GetProcAddress");
    auto* freeLibrary = fixture.Shim<BOOL(HMODULE)>("FreeLibrary");

    // Title modules: already loaded, or loaded on demand (directories are ignored).
    CHECK(loadW(L"liba.dll") == AsModule(kLibABase));
    CHECK(loadA("LIBA") == AsModule(kLibABase));
    CHECK(loadExW(LR"(D:\package\libc.dll)", nullptr, 0) == AsModule(kLibCBase));
    CHECK(fixture.Manager->ModuleCount() == 4);
    CHECK(moduleW(nullptr) == AsModule(kGameBase));
    CHECK(moduleW(L"libb.dll") == AsModule(kLibBBase));

    // GetProcAddress on title modules: by name, by ordinal, through a forwarder.
    const auto procOf = [&](HMODULE module, const char* name)
    { return std::bit_cast<uint64_t>(getProc(module, name)); };
    CHECK(procOf(AsModule(kLibABase), "Add") == kLibABase + fixture.Files.AddRva);
    CHECK(procOf(AsModule(kLibABase), OrdinalName(2)) == kLibABase + fixture.Files.NoNameRva);
    CHECK(procOf(AsModule(kLibABase), "Forwarded") == kLibBBase + fixture.Files.MulRva);
    CHECK(procOf(AsModule(kLibCBase), "Hello") == kLibCBase + fixture.Files.HelloRva);

    // Misses return null, set the error and are recorded.
    SetLastError(0);
    CHECK(getProc(AsModule(kLibABase), "Nope") == nullptr);
    CHECK(GetLastError() == ERROR_PROC_NOT_FOUND);
    CHECK(fixture.Stubs.GetHitCount(StubKind::Win32Import, "liba.dll", "Nope") == 1);

    // Shim-only modules get a pseudo-handle whose exports are the shims.
    const HMODULE shim = loadW(L"testshim.dll");
    REQUIRE(shim != nullptr);
    CHECK(procOf(shim, "Hello") == std::bit_cast<uint64_t>(&TestHello));

    // Host modules get their real handle; OpenEra shims still override.
    const HMODULE kernel = loadW(L"KERNEL32.dll");
    CHECK(kernel == GetModuleHandleW(L"kernel32.dll"));
    CHECK(procOf(kernel, "GetTickCount64") == RealExport("GetTickCount64"));
    CHECK(procOf(kernel, "GetProcAddress") == std::bit_cast<uint64_t>(getProc));

    // Unknown modules fail like Windows does.
    SetLastError(0);
    CHECK(loadW(L"nothing.dll") == nullptr);
    CHECK(GetLastError() == ERROR_MOD_NOT_FOUND);
    CHECK(moduleW(L"notloaded.dll") == nullptr);
    CHECK(freeLibrary(AsModule(kLibABase)) == TRUE);

    ModuleManager::SetActive(nullptr);
    SetLastError(0);
    CHECK(loadW(L"liba.dll") == nullptr);
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
}

TEST_CASE("DirectoryModuleReader reads plain names from one directory", "[ModuleManager]")
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "openera-module-reader-test";
    std::filesystem::create_directories(directory);
    const std::string content = "MZ synthetic";
    for (const char* name : {"liba.dll", "kernel32.dll"})
    {
        std::ofstream(directory / name, std::ios::binary) << content;
    }

    const auto reader = DirectoryModuleReader(directory);
    const auto bytes = reader("liba.dll");
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() == content.size());
    CHECK_FALSE(reader("kernel32.dll").has_value()); // Reserved, although the file exists.
    CHECK_FALSE(reader(R"(..\liba.dll)").has_value());
    CHECK_FALSE(reader("sub/liba.dll").has_value());
    CHECK_FALSE(reader("absent.dll").has_value());

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST_CASE("Module enum names", "[ModuleManager]")
{
    CHECK(ToString(ModuleError::Reserved) == "Reserved");
}
