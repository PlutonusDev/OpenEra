// Runs synthetic title folders end to end: a MicrosoftGame.config plus the
// EntryImage.exe test image, loaded, linked and started by OpenEra.
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include "App/TitleLauncher.h"
#include "Core/Logger.h"
#include "Kernel/ExitShims.h"

using namespace OpenEra::App;
using OpenEra::Core::Logger;

namespace
{
// A fresh folder under the temp directory, removed afterwards.
class TitleFolder
{
public:
    explicit TitleFolder(std::string_view name)
        : m_path(std::filesystem::temp_directory_path() / "OpenEraTests" /
                 std::format("{}-{}", name, GetCurrentProcessId()))
    {
        std::filesystem::remove_all(m_path);
        std::filesystem::create_directories(m_path);
    }

    // Best-effort cleanup.
    ~TitleFolder() // NOLINT(bugprone-exception-escape)
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
        std::filesystem::remove_all(Overlay(), error);
    }

    TitleFolder(const TitleFolder&) = delete;
    TitleFolder& operator=(const TitleFolder&) = delete;
    TitleFolder(TitleFolder&&) = delete;
    TitleFolder& operator=(TitleFolder&&) = delete;

    void WriteConfig(std::string_view executableName) const
    {
        std::ofstream(m_path / std::string(kConfigFileName)) << std::format(R"(<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="OpenEra.TestTitle" Publisher="CN=OpenEra" Version="1.0.0.0"/>
  <ExecutableList>
    <Executable Name="{}" TargetDeviceFamily="Scarlett" Id="Game"/>
  </ExecutableList>
</Game>)",
                                                                            executableName);
    }

    void AddEntryImage(const std::filesystem::path& relativeDirectory) const
    {
        AddImage("EntryImage.exe", relativeDirectory);
    }

    void AddImage(std::string_view name, const std::filesystem::path& relativeDirectory) const
    {
        std::filesystem::create_directories(m_path / relativeDirectory);
        std::filesystem::copy_file(std::filesystem::path(OPENERA_FIXTURE_DIR) / name,
                                   m_path / relativeDirectory / name);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }
    [[nodiscard]] std::filesystem::path Overlay() const { return m_path.native() + L"-overlay"; }

private:
    std::filesystem::path m_path;
};

LaunchOptions OptionsFor(const TitleFolder& folder)
{
    return {.Folder = folder.Path(),
            .ExecutableId = {},
            .Start = true,
            .TlsSlot = OPENERA_TLS_SLOT,
            .WorkDirectory = std::filesystem::temp_directory_path() / "OpenEraTests",
            .OverlayDirectory = folder.Overlay(),
            .Arguments = {}};
}
} // namespace

TEST_CASE("A title folder runs and returns its exit code", "[launcher]")
{
    const TitleFolder folder("RunsTitle");
    folder.WriteConfig("EntryImage.exe");
    folder.AddEntryImage({});
    Logger logger;
    const auto result = RunTitleFolder(OptionsFor(folder), logger);
    CHECK(result.Error == LaunchError::None);
    CHECK(result.Executable == "EntryImage.exe");
    CHECK(result.ExitCode == std::optional<uint32_t>(42));
}

TEST_CASE("A running title sees itself as the process image", "[launcher][process]")
{
    const TitleFolder folder("Identity");
    folder.WriteConfig("IdentityImage.exe");
    folder.AddImage("IdentityImage.exe", {});
    Logger logger;
    auto options = OptionsFor(folder);
    options.Arguments = {L"--level", L"two words"};
    const auto before = std::filesystem::current_path();
    const auto result = RunTitleFolder(options, logger);
    CHECK(result.Error == LaunchError::None);
    // Each bit is a check inside the title that failed: module path, command line, image name, small buffer.
    CHECK(result.ExitCode == std::optional<uint32_t>(0));
    CHECK(std::filesystem::current_path() == before); // Restored afterwards.
}

TEST_CASE("The executable can live in a subfolder", "[launcher]")
{
    const TitleFolder folder("Subfolder");
    folder.WriteConfig("bin\\EntryImage.exe");
    folder.AddEntryImage("bin");
    Logger logger;
    const auto result = RunTitleFolder(OptionsFor(folder), logger);
    CHECK(result.Error == LaunchError::None);
    CHECK(result.ExitCode == std::optional<uint32_t>(42));
}

TEST_CASE("Without Start a title is only loaded and linked", "[launcher]")
{
    const TitleFolder folder("NoStart");
    folder.WriteConfig("EntryImage.exe");
    folder.AddEntryImage({});
    Logger logger;
    auto options = OptionsFor(folder);
    options.Start = false;
    const auto result = RunTitleFolder(options, logger);
    CHECK(result.Error == LaunchError::None);
    CHECK_FALSE(result.ExitCode.has_value());
}

TEST_CASE("Unusable title folders are refused", "[launcher]")
{
    Logger logger;
    const TitleFolder folder("Refused");
    auto options = OptionsFor(folder);

    CHECK(RunTitleFolder(options, logger).Error == LaunchError::NoConfig);

    options.Folder = folder.Path() / "missing";
    CHECK(RunTitleFolder(options, logger).Error == LaunchError::NotAFolder);

    options.Folder = folder.Path() / "Title.msixvc";
    std::ofstream(options.Folder) << "not a real package";
    CHECK(RunTitleFolder(options, logger).Error == LaunchError::PackagedInput);

    options.Folder = folder.Path();
    std::ofstream(folder.Path() / std::string(kConfigFileName)) << "<Game>";
    CHECK(RunTitleFolder(options, logger).Error == LaunchError::BadConfig);

    folder.WriteConfig("..\\Outside.exe");
    CHECK(RunTitleFolder(options, logger).Error == LaunchError::BadExecutablePath);

    folder.WriteConfig("Missing.exe");
    CHECK(RunTitleFolder(options, logger).Error == LaunchError::LoadFailed);

    options.ExecutableId = "NoSuchId";
    CHECK(RunTitleFolder(options, logger).Error == LaunchError::NoExecutable);
}

namespace
{
struct LauncherRun
{
    DWORD ExitCode = 0;
    std::string Output; // stderr and stdout.
};

// Runs the real openera.exe with `arguments`, capturing what it prints.
LauncherRun RunLauncher(const std::wstring& arguments)
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
    std::wstring command = L"\"" + std::wstring(OPENERA_LAUNCHER) + L"\" " + arguments;
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &process));
    CloseHandle(writePipe);
    LauncherRun run;
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != FALSE && read != 0)
    {
        run.Output.append(buffer.data(), read);
    }
    CloseHandle(readPipe);
    REQUIRE(WaitForSingleObject(process.hProcess, 60'000) == WAIT_OBJECT_0);
    GetExitCodeProcess(process.hProcess, &run.ExitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return run;
}
} // namespace

TEST_CASE("Exit requests are recorded once while armed", "[exit]")
{
    using namespace OpenEra::Kernel;
    DisarmTitleExit();
    CHECK_FALSE(RequestTitleExit(1));
    ArmTitleExit();
    CHECK_FALSE(TitleExitCode().has_value());
    CHECK(WaitForSingleObject(TitleExitEvent(), 0) == WAIT_TIMEOUT);
    CHECK(RequestTitleExit(4));
    CHECK(RequestTitleExit(5)); // Later requests still end their thread, but the first code stands.
    CHECK(TitleExitCode() == std::optional<uint32_t>(4));
    CHECK(WaitForSingleObject(TitleExitEvent(), 0) == WAIT_OBJECT_0);
    ArmTitleExit(); // A new run starts clean.
    CHECK_FALSE(TitleExitCode().has_value());
    DisarmTitleExit();
}

TEST_CASE("A title that exits the process only ends itself", "[launcher][exit]")
{
    struct Case
    {
        std::vector<std::wstring> Arguments;
        uint32_t Code;
    };
    for (const auto& sample : {Case{.Arguments = {}, .Code = 7}, Case{.Arguments = {L"--from-thread"}, .Code = 9},
                               Case{.Arguments = {L"--terminate"}, .Code = 11}})
    {
        const TitleFolder folder("Exit");
        folder.WriteConfig("ExitImage.exe");
        folder.AddImage("ExitImage.exe", {});
        Logger logger;
        auto options = OptionsFor(folder);
        options.Arguments = sample.Arguments;
        const auto result = RunTitleFolder(options, logger);
        INFO(sample.Code);
        CHECK(result.Error == LaunchError::None);
        CHECK(result.ExitRequested);
        CHECK(result.ExitCode == std::optional<uint32_t>(sample.Code));
    }
}

TEST_CASE("The C runtime's exit after main returns only ends the title", "[launcher][exit]")
{
    const TitleFolder folder("CrtExit");
    folder.WriteConfig("CrtExitImage.exe");
    folder.AddImage("CrtExitImage.exe", {});
    Logger logger;
    const auto result = RunTitleFolder(OptionsFor(folder), logger);
    CHECK(result.Error == LaunchError::None);
    CHECK(result.ExitRequested);
    CHECK(result.ExitCode == std::optional<uint32_t>(3));
}

TEST_CASE("openera logs its summary and exits with the title's code", "[launcher][exit]")
{
    const TitleFolder folder("LauncherExit");
    folder.WriteConfig("ExitImage.exe");
    folder.AddImage("ExitImage.exe", {});
    const std::wstring common = L"run \"" + folder.Path().wstring() +
                                L"\" --set log.debugger=false --set \"files.overlay='" + folder.Overlay().wstring() +
                                L"'\"";

    const auto exited = RunLauncher(common);
    INFO(exited.Output);
    CHECK(exited.ExitCode == 7);
    CHECK(exited.Output.find("exited the process with 7") != std::string::npos);
    CHECK(exited.Output.find("Stub summary") != std::string::npos);

    // A thread that outlives the title: it's left running, and openera still finishes.
    const auto lingering = RunLauncher(common + L" -- --linger");
    INFO(lingering.Output);
    CHECK(lingering.ExitCode == 5);
    CHECK(lingering.Output.find("still running") != std::string::npos);
    CHECK(lingering.Output.find("Stub summary") != std::string::npos);
}

TEST_CASE("A C runtime title's exit runs its atexit handlers and openera exits cleanly", "[launcher][exit]")
{
    const TitleFolder folder("LauncherCrtExit");
    folder.WriteConfig("CrtExitImage.exe");
    folder.AddImage("CrtExitImage.exe", {});
    const auto run =
        RunLauncher(L"run \"" + folder.Path().wstring() + L"\" --set log.debugger=false --set \"files.overlay='" +
                    folder.Overlay().wstring() + L"'\"");
    INFO(run.Output);
    CHECK(run.ExitCode == 3); // Not a crash once the title's image is gone.
    CHECK(run.Output.find("atexit handler ran") != std::string::npos);
    CHECK(run.Output.find("Stub summary") != std::string::npos);
}
