// Runs synthetic title folders end to end: a MicrosoftGame.config plus the
// EntryImage.exe test image, loaded, linked and started by OpenEra.
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include "App/TitleLauncher.h"
#include "Core/Logger.h"

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
        std::filesystem::create_directories(m_path / relativeDirectory);
        std::filesystem::copy_file(std::filesystem::path(OPENERA_FIXTURE_DIR) / "EntryImage.exe",
                                   m_path / relativeDirectory / "EntryImage.exe");
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

LaunchOptions OptionsFor(const TitleFolder& folder)
{
    return {.Folder = folder.Path(),
            .ExecutableId = {},
            .Start = true,
            .TlsSlot = OPENERA_TLS_SLOT,
            .WorkDirectory = std::filesystem::temp_directory_path() / "OpenEraTests"};
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
