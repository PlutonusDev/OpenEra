#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include "Loader/HostModules.h"

using OpenEra::Loader::HostModules;
using OpenEra::Loader::ModuleKey;

namespace
{
uint64_t RealExport(const wchar_t* module, const char* name)
{
    return std::bit_cast<uint64_t>(GetProcAddress(GetModuleHandleW(module), name));
}
} // namespace

TEST_CASE("ModuleKey lowercases and drops .dll", "[HostModules]")
{
    CHECK(ModuleKey("KERNEL32.dll") == "kernel32");
    CHECK(ModuleKey("Api-Ms-Win-Crt-Runtime-L1-1-0.DLL") == "api-ms-win-crt-runtime-l1-1-0");
    CHECK(ModuleKey("ws2_32") == "ws2_32");
}

TEST_CASE("The default allowlist covers Win32 and the C runtime only", "[HostModules]")
{
    const auto host = HostModules::CreateDefault();
    for (const char* allowed : {"KERNEL32.dll", "kernelbase", "USER32.dll", "WS2_32.dll", "ADVAPI32.dll",
                                "MSVCP140.dll", "MSVCP140_ATOMIC_WAIT.dll", "VCRUNTIME140_1.dll",
                                "api-ms-win-crt-runtime-l1-1-0.dll", "api-ms-win-core-synch-l1-2-0.dll"})
    {
        INFO(allowed);
        CHECK(host->IsAllowed(allowed));
    }
    for (const char* refused :
         {"d3d12_x.dll", "XAudio2_9.dll", "GameInput.dll", "xgameruntime.dll", "ntdll.dll", "game.dll", ""})
    {
        INFO(refused);
        CHECK_FALSE(host->IsAllowed(refused));
    }
}

TEST_CASE("Names with a path are never loaded", "[HostModules]")
{
    const auto host = HostModules::CreateDefault();
    for (const char* name : {R"(C:\Windows\System32\kernel32.dll)", R"(..\kernel32.dll)", "sub/kernel32.dll",
                             R"(\\server\share\kernel32.dll)", "kernel32..dll"})
    {
        INFO(name);
        CHECK_FALSE(host->IsAllowed(name));
        CHECK_FALSE(host->Handle(name).has_value());
        CHECK_FALSE(host->FindExport(name, "GetTickCount64").has_value());
    }
}

TEST_CASE("FindExport binds to the host's real exports", "[HostModules]")
{
    const auto host = HostModules::CreateDefault();

    const auto byName = host->FindExport("KERNEL32.dll", "GetTickCount64");
    REQUIRE(byName.has_value());
    CHECK(*byName == RealExport(L"kernel32.dll", "GetTickCount64"));
    CHECK(std::bit_cast<ULONGLONG(WINAPI*)()>(*byName)() > 0);

    // Ordinal lookups match GetProcAddress(MAKEINTRESOURCEA(n)).
    for (const uint16_t ordinal : {uint16_t{1}, uint16_t{2}, uint16_t{100}})
    {
        INFO(ordinal);
        const uintptr_t ordinalName = ordinal;
        const auto expected = RealExport(L"kernel32.dll", std::bit_cast<const char*>(ordinalName));
        CHECK(host->FindExport("kernel32", ordinal).value_or(0) == expected);
    }

    CHECK_FALSE(host->FindExport("kernel32.dll", "NoSuchExportInKernel32").has_value());
    CHECK_FALSE(host->FindExport("kernel32.dll", "").has_value());
    CHECK_FALSE(host->FindExport("xaudio2_9.dll", "XAudio2Create").has_value()); // Exists, but not allowed.
}

TEST_CASE("Handle caches modules and maps handles back to names", "[HostModules]")
{
    const auto host = HostModules::CreateDefault();
    const auto first = host->Handle("Kernel32.DLL");
    REQUIRE(first.has_value());
    CHECK(host->Handle("kernel32") == first);
    CHECK(*first == std::bit_cast<uint64_t>(GetModuleHandleW(L"kernel32.dll")));
    CHECK(host->ModuleForHandle(*first) == "kernel32");
    CHECK_FALSE(host->ModuleForHandle(0x1234).has_value());

    const HostModules custom({"openera-no-such-module"}, {});
    CHECK(custom.IsAllowed("openera-no-such-module.dll"));
}

TEST_CASE("Allowed modules are loaded from System32 only", "[HostModules]")
{
    // A real DLL under a name that exists only in a temporary directory, which
    // is also made the current directory: it must not be found.
    using std::filesystem::path;
    const path directory = std::filesystem::temp_directory_path() / "openera-host-modules-test";
    std::filesystem::create_directories(directory);
    const path copy = directory / "openera_not_in_system32.dll";
    std::array<wchar_t, MAX_PATH> system{};
    REQUIRE(GetSystemDirectoryW(system.data(), MAX_PATH) != 0);
    std::filesystem::copy_file(path(system.data()) / "version.dll", copy,
                               std::filesystem::copy_options::overwrite_existing);

    const path previous = std::filesystem::current_path();
    std::filesystem::current_path(directory);
    {
        HostModules host({"openera_not_in_system32"}, {});
        CHECK(host.IsAllowed("openera_not_in_system32.dll"));
        CHECK_FALSE(host.Handle("openera_not_in_system32.dll").has_value());
    }
    std::filesystem::current_path(previous);

    // Sanity check: the copy itself is loadable by full path.
    HMODULE direct = LoadLibraryExW(copy.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    CHECK(direct != nullptr);
    if (direct != nullptr)
    {
        FreeLibrary(direct);
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}
