// The title as the process image: module paths, command line and image name.
#include <array>
#include <bit>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>
#include <shellapi.h>

#include "Core/Logger.h"
#include "Kernel/ProcessShims.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"
#include "Loader/HostModules.h"
#include "Loader/ImageMemory.h"
#include "Loader/ModuleManager.h"

using namespace OpenEra::Kernel;
using OpenEra::Core::Logger;
using OpenEra::Loader::ModuleManager;

namespace
{
using GetModuleFileNameFunction = DWORD WINAPI(HMODULE, LPWSTR, DWORD);
using GetCommandLineFunction = LPWSTR WINAPI();
using GetCommandLineAFunction = LPSTR WINAPI();
using QueryImageNameFunction = BOOL WINAPI(HANDLE, DWORD, LPWSTR, PDWORD);

template <typename Function>
Function* Shim(const ShimRegistry& shims, std::string_view symbol)
{
    const auto resolved = shims.Resolve("kernel32.dll", symbol);
    REQUIRE(resolved.has_value());
    return FromShimProc<Function>(resolved->Export->Proc);
}

// What CommandLineToArgvW reads back from a command line.
std::vector<std::wstring> Parse(const std::wstring& commandLine)
{
    int count = 0;
    LPWSTR* const raw = CommandLineToArgvW(commandLine.c_str(), &count);
    REQUIRE(raw != nullptr);
    const std::span<LPWSTR> arguments(raw, static_cast<size_t>(count));
    std::vector<std::wstring> result(arguments.begin(), arguments.end());
    LocalFree(static_cast<HLOCAL>(raw));
    return result;
}

class IdentityScope
{
public:
    explicit IdentityScope(TitleProcessIdentity identity) { SetTitleProcess(std::move(identity)); }
    ~IdentityScope() { SetTitleProcess(std::nullopt); }

    IdentityScope(const IdentityScope&) = delete;
    IdentityScope& operator=(const IdentityScope&) = delete;
    IdentityScope(IdentityScope&&) = delete;
    IdentityScope& operator=(IdentityScope&&) = delete;
};
} // namespace

TEST_CASE("Arguments are quoted so Windows parses them back unchanged", "[process]")
{
    CHECK(QuoteArgument(L"plain") == L"plain");
    CHECK(QuoteArgument(L"") == L"\"\"");
    CHECK(QuoteArgument(L"two words") == L"\"two words\"");
    CHECK(QuoteArgument(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
    CHECK(QuoteArgument(L"C:\\Some Folder\\") == L"\"C:\\Some Folder\\\\\"");
    CHECK(QuoteArgument(L"back\\\\slashes") == L"back\\\\slashes"); // No quote follows: left alone.

    const std::vector<std::wstring> arguments = {
        L"plain", L"", L"two words", L"say \"hi\"", L"C:\\Some Folder\\", L"a\\\\\"b", L"tab\there"};
    const std::filesystem::path image = L"C:\\Games\\Contoso Game\\Game.exe";
    const auto parsed = Parse(BuildCommandLine(image, arguments));
    REQUIRE(parsed.size() == arguments.size() + 1);
    CHECK(parsed.front() == image.wstring());
    CHECK(std::vector<std::wstring>(parsed.begin() + 1, parsed.end()) == arguments);
}

TEST_CASE("Text is copied into buffers the way GetModuleFileName does", "[process]")
{
    std::array<wchar_t, 8> buffer{};
    const auto fits = CopyToBuffer(std::wstring_view(L"abc"), buffer);
    CHECK(fits.Length == 3);
    CHECK_FALSE(fits.Truncated);
    CHECK(std::wstring(buffer.data()) == L"abc");

    const auto cut = CopyToBuffer(std::wstring_view(L"abcdefghij"), buffer);
    CHECK(cut.Length == 7);
    CHECK(cut.Truncated);
    CHECK(std::wstring(buffer.data()) == L"abcdefg");

    CHECK(CopyToBuffer(std::wstring_view(L"abc"), std::span<wchar_t>()).Truncated);
}

TEST_CASE("The command line and image name are the title's while it runs", "[process]")
{
    ShimRegistry shims;
    REQUIRE(OpenEra::Loader::RegisterLoaderShims(shims));
    auto* const getCommandLine = Shim<GetCommandLineFunction>(shims, "GetCommandLineW");
    auto* const getCommandLineA = Shim<GetCommandLineAFunction>(shims, "GetCommandLineA");
    auto* const queryImageName = Shim<QueryImageNameFunction>(shims, "QueryFullProcessImageNameW");

    const std::filesystem::path image = L"C:\\Games\\Contoso\\Game.exe";
    {
        const IdentityScope identity(
            {.Image = image, .CommandLine = BuildCommandLine(image, std::vector<std::wstring>{L"-x"})});
        CHECK(std::wstring(getCommandLine()) == L"\"C:\\Games\\Contoso\\Game.exe\" -x");
        CHECK(std::string(getCommandLineA()) == "\"C:\\Games\\Contoso\\Game.exe\" -x");

        std::array<wchar_t, MAX_PATH> name{};
        DWORD size = MAX_PATH;
        REQUIRE(queryImageName(GetCurrentProcess(), 0, name.data(), &size));
        CHECK(std::wstring(name.data()) == image.wstring());
        CHECK(size == image.wstring().size());

        std::array<wchar_t, 4> tiny{};
        DWORD tinySize = 4;
        CHECK_FALSE(queryImageName(GetCurrentProcess(), 0, tiny.data(), &tinySize));
        CHECK(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    }
    // Without a title, the host's own answers.
    CHECK(std::wstring(getCommandLine()) == std::wstring(GetCommandLineW()));
    std::array<wchar_t, MAX_PATH> host{};
    DWORD hostSize = MAX_PATH;
    REQUIRE(queryImageName(GetCurrentProcess(), 0, host.data(), &hostSize));
    CHECK(std::filesystem::path(host.data()).filename() == "OpenEraTests.exe");
}

TEST_CASE("GetModuleFileName reports title modules from their folder", "[process]")
{
    Logger log;
    StubRegistry stubs(log);
    ShimRegistry shims;
    REQUIRE(OpenEra::Loader::RegisterLoaderShims(shims));
    const auto allocator = OpenEra::Loader::CreateVirtualImageAllocator();
    const auto host = OpenEra::Loader::HostModules::CreateDefault();
    ModuleManager manager(OpenEra::Loader::DirectoryModuleReader(OPENERA_FIXTURE_DIR), shims, stubs, *allocator, log,
                          host.get());
    manager.SetImageDirectory(OPENERA_FIXTURE_DIR);
    ModuleManager::SetActive(&manager);
    REQUIRE(manager.Load("SyncImage.dll") == OpenEra::Loader::ModuleError::None);
    auto* const getModuleFileName = Shim<GetModuleFileNameFunction>(shims, "GetModuleFileNameW");

    const auto expected = (std::filesystem::absolute(OPENERA_FIXTURE_DIR) / "SyncImage.dll").lexically_normal();
    std::array<wchar_t, MAX_PATH> name{};
    const DWORD length = getModuleFileName(nullptr, name.data(), MAX_PATH);
    CHECK(std::filesystem::path(name.data()) == expected);
    CHECK(length == expected.wstring().size());

    const auto* module = manager.Find("SyncImage.dll");
    REQUIRE(module != nullptr);
    CHECK(getModuleFileName(std::bit_cast<HMODULE>(module->Base()), name.data(), MAX_PATH) == length);

    // Too small: filled, terminated, and reported.
    std::array<wchar_t, 5> tiny{};
    CHECK(getModuleFileName(nullptr, tiny.data(), 5) == 5);
    CHECK(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    CHECK(std::wstring(tiny.data()).size() == 4);

    // Host modules are the host's.
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    REQUIRE(getModuleFileName(kernel32, name.data(), MAX_PATH) != 0);
    CHECK(CompareStringOrdinal(std::filesystem::path(name.data()).filename().c_str(), -1, L"kernel32.dll", -1, TRUE) ==
          CSTR_EQUAL);
    ModuleManager::SetActive(nullptr);
}
