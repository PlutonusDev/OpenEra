#include <array>
#include <string>
#include <string_view>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include "Core/Assert.h"

using OpenEra::Core::kAssertionExitCode;
using OpenEra::Core::kAssertionsEnabled;

#ifdef NDEBUG
static_assert(!kAssertionsEnabled, "Release builds compile assertions out");
#else
static_assert(kAssertionsEnabled, "Debug builds check assertions");
#endif

namespace
{
struct HelperResult
{
    DWORD ExitCode = 0;
    std::string Output; // stderr and stdout.
};

// Runs AssertHelper with `mode`, capturing what it prints.
HelperResult RunHelper(std::wstring_view mode)
{
    SECURITY_ATTRIBUTES inherit{
        .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    REQUIRE(CreatePipe(&readPipe, &writePipe, &inherit, 0));
    REQUIRE(SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + std::wstring(OPENERA_ASSERT_HELPER) + L"\" " + std::wstring(mode);
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &process));
    CloseHandle(writePipe);

    HelperResult result;
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != FALSE && read != 0)
    {
        result.Output.append(buffer.data(), read);
    }
    CloseHandle(readPipe);
    REQUIRE(WaitForSingleObject(process.hProcess, 30'000) == WAIT_OBJECT_0);
    GetExitCodeProcess(process.hProcess, &result.ExitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return result;
}

int Touch(int& counter) noexcept
{
    return ++counter;
}
} // namespace

TEST_CASE("Assertions are only evaluated when enabled; VERIFY always is", "[assert]")
{
    int asserted = 0;
    OPENERA_ASSERT(Touch(asserted) > 0);
    CHECK(asserted == (kAssertionsEnabled ? 1 : 0));

    int verified = 0;
    OPENERA_VERIFY(Touch(verified) > 0, "touched {}", verified);
    CHECK(verified == 1);
}

TEST_CASE("Passing assertions let the process carry on", "[assert]")
{
    const auto result = RunHelper(L"pass");
    CHECK(result.ExitCode == 0);
    CHECK(result.Output.empty());
}

TEST_CASE("A failed assertion logs where it failed and ends the process", "[assert]")
{
    const auto result = RunHelper(L"assert");
    if (!kAssertionsEnabled)
    {
        CHECK(result.ExitCode == 0);
        return;
    }
    INFO(result.Output);
    CHECK(result.ExitCode == static_cast<DWORD>(kAssertionExitCode));
    CHECK(result.Output.find("Assertion failed: Twice(value) == 0: Twice(42) is 84") != std::string::npos);
    CHECK(result.Output.find("AssertHelper.cpp:") != std::string::npos);
}

TEST_CASE("A failed VERIFY ends the process in Debug builds", "[assert]")
{
    const auto result = RunHelper(L"verify");
    if (!kAssertionsEnabled)
    {
        CHECK(result.ExitCode == 0);
        return;
    }
    INFO(result.Output);
    CHECK(result.ExitCode == static_cast<DWORD>(kAssertionExitCode));
    CHECK(result.Output.find("Assertion failed: Twice(1) == 3 at ") != std::string::npos);
}
