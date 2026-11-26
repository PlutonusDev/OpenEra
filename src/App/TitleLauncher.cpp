#include "TitleLauncher.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

#include <Windows.h>

#include "Core/Logger.h"
#include "Graphics/GraphicsShims.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"
#include "Kernel/TitleFileSystem.h"
#include "Loader/HostModules.h"
#include "Loader/ImageInitializer.h"
#include "Loader/ImageMemory.h"
#include "Loader/ModuleManager.h"

namespace OpenEra::App
{
namespace
{
using Core::Logger;
using Loader::ModuleManager;

constexpr DWORD kMaxPath = 32'768;

constexpr std::array<std::string_view, 4> kPackageExtensions = {".xvc", ".msixvc", ".xvd", ".msixvcbundle"};

constexpr char AsciiLower(char value) noexcept
{
    constexpr std::string_view kUpper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    constexpr std::string_view kLower = "abcdefghijklmnopqrstuvwxyz";
    const auto index = kUpper.find(value);
    return index == std::string_view::npos ? value : kLower.at(index);
}

bool IsPackage(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), AsciiLower);
    return std::ranges::find(kPackageExtensions, extension) != kPackageExtensions.end();
}

struct ExecutableLocation
{
    std::filesystem::path Directory; // Relative to the title folder.
    std::string FileName;
};

// Config names are relative paths inside the folder ("Game.exe", "bin\Game.exe").
std::optional<ExecutableLocation> LocateExecutable(std::string_view name)
{
    if (name.empty() || name.find(':') != std::string_view::npos || name.starts_with('\\') || name.starts_with('/'))
    {
        return std::nullopt;
    }
    const std::filesystem::path path(std::u8string(name.begin(), name.end()));
    for (const auto& part : path)
    {
        if (part == "..")
        {
            return std::nullopt;
        }
    }
    auto fileName = path.filename().u8string();
    if (fileName.empty())
    {
        return std::nullopt;
    }
    return ExecutableLocation{.Directory = path.parent_path(),
                              .FileName = std::string(fileName.begin(), fileName.end())};
}

// Default overlay for a title: under %LOCALAPPDATA%, named after its identity.
std::filesystem::path DefaultOverlay(std::string_view identity)
{
    std::string name;
    for (const char value : identity)
    {
        const bool safe = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                          (value >= '0' && value <= '9') || value == '.' || value == '-' || value == '_';
        name += safe ? value : '_';
    }
    if (name.empty() || name.find_first_not_of('.') == std::string::npos)
    {
        name = "Title";
    }
    std::wstring localAppData(kMaxPath, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), kMaxPath);
    localAppData.resize(length < kMaxPath ? length : 0);
    const std::filesystem::path base =
        localAppData.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(localAppData);
    return base / "OpenEra" / "Titles" / name / "Overlay";
}

// Keeps the file shims pointed at `files` while it's alive.
class ActiveFileSystemScope
{
public:
    explicit ActiveFileSystemScope(Kernel::TitleFileSystem& files) noexcept
    {
        Kernel::TitleFileSystem::SetActive(&files);
    }
    ~ActiveFileSystemScope() { Kernel::TitleFileSystem::SetActive(nullptr); }

    ActiveFileSystemScope(const ActiveFileSystemScope&) = delete;
    ActiveFileSystemScope& operator=(const ActiveFileSystemScope&) = delete;
    ActiveFileSystemScope(ActiveFileSystemScope&&) = delete;
    ActiveFileSystemScope& operator=(ActiveFileSystemScope&&) = delete;
};

// Keeps the loader shims pointed at `manager` while it's alive.
class ActiveManagerScope
{
public:
    explicit ActiveManagerScope(ModuleManager& manager) noexcept { ModuleManager::SetActive(&manager); }
    ~ActiveManagerScope() { ModuleManager::SetActive(nullptr); }

    ActiveManagerScope(const ActiveManagerScope&) = delete;
    ActiveManagerScope& operator=(const ActiveManagerScope&) = delete;
    ActiveManagerScope(ActiveManagerScope&&) = delete;
    ActiveManagerScope& operator=(ActiveManagerScope&&) = delete;
};
} // namespace

std::string_view ToString(LaunchError error) noexcept
{
    switch (error)
    {
    case LaunchError::None:
        return "None";
    case LaunchError::NotAFolder:
        return "NotAFolder";
    case LaunchError::PackagedInput:
        return "PackagedInput";
    case LaunchError::NoConfig:
        return "NoConfig";
    case LaunchError::BadConfig:
        return "BadConfig";
    case LaunchError::NoExecutable:
        return "NoExecutable";
    case LaunchError::BadExecutablePath:
        return "BadExecutablePath";
    case LaunchError::LoadFailed:
        return "LoadFailed";
    case LaunchError::StartFailed:
        return "StartFailed";
    }
    return "Unknown";
}

TitleConfigResult ReadTitleConfig(const std::filesystem::path& folder, Logger& logger)
{
    std::error_code error;
    if (std::filesystem::is_regular_file(folder, error) && IsPackage(folder))
    {
        logger.Error("Launch: {} is a game package. OpenEra runs unpacked title folders and never decrypts or "
                     "unpacks packages.",
                     folder.string());
        return {.Error = LaunchError::PackagedInput, .Config = std::nullopt};
    }
    if (!std::filesystem::is_directory(folder, error))
    {
        logger.Error("Launch: {} is not a folder", folder.string());
        return {.Error = LaunchError::NotAFolder, .Config = std::nullopt};
    }

    std::ifstream stream(folder / kConfigFileName, std::ios::binary);
    if (!stream)
    {
        logger.Error("Launch: {} has no {}", folder.string(), kConfigFileName);
        return {.Error = LaunchError::NoConfig, .Config = std::nullopt};
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    auto parsed = Loader::ParseGameConfig(text, logger);
    if (!parsed.Config)
    {
        logger.Error("Launch: {} is not usable ({})", kConfigFileName, Loader::ToString(parsed.Error));
        return {.Error = LaunchError::BadConfig, .Config = std::nullopt};
    }
    return {.Error = LaunchError::None, .Config = std::move(parsed.Config)};
}

LaunchResult RunTitleFolder(const LaunchOptions& options, Logger& logger)
{
    LaunchResult result;
    auto config = ReadTitleConfig(options.Folder, logger);
    if (!config.Config)
    {
        result.Error = config.Error;
        return result;
    }
    const auto* executable = Loader::SelectExecutable(*config.Config, options.ExecutableId);
    if (executable == nullptr)
    {
        logger.Error("Launch: no executable {}in {}",
                     options.ExecutableId.empty() ? std::string{} : "with Id " + options.ExecutableId + " ",
                     kConfigFileName);
        result.Error = LaunchError::NoExecutable;
        return result;
    }
    result.Executable = executable->Name;
    const auto location = LocateExecutable(executable->Name);
    if (!location)
    {
        logger.Error("Launch: executable name {} is not a path inside the title folder", executable->Name);
        result.Error = LaunchError::BadExecutablePath;
        return result;
    }
    logger.Info("Launch: {} {} ({}), executable {}", config.Config->IdentityName, config.Config->Version,
                config.Config->TitleId, executable->Name);

    Kernel::ShimRegistry shims;
    if (!Loader::RegisterLoaderShims(shims) || !Graphics::RegisterGraphicsShims(shims))
    {
        result.Error = LaunchError::LoadFailed;
        return result;
    }
    Kernel::StubRegistry stubs(logger);
    Kernel::TitleFileSystem files(options.Folder, options.OverlayDirectory.empty()
                                                      ? DefaultOverlay(config.Config->IdentityName)
                                                      : options.OverlayDirectory);
    const ActiveFileSystemScope activeFiles(files);
    logger.Info("Launch: title writes go to {}", files.OverlayRoot().string());
    const auto allocator = Loader::CreateVirtualImageAllocator();
    const auto host = Loader::HostModules::CreateDefault();

    // Declared before the manager, so it's destroyed after it.
    std::optional<Loader::ImageInitializer> initializer;
    if (options.Start)
    {
        initializer.emplace(options.TlsSlot.empty() ? Loader::ImageInitializer::DefaultTlsSlotPath() : options.TlsSlot,
                            options.WorkDirectory.empty() ? std::filesystem::temp_directory_path() / "OpenEra"
                                                          : options.WorkDirectory,
                            logger);
    }
    ModuleManager manager(Loader::DirectoryModuleReader(options.Folder / location->Directory), shims, stubs, *allocator,
                          logger, host.get());
    if (initializer)
    {
        manager.SetInitializer(&*initializer);
    }
    const ActiveManagerScope active(manager);

    if (const auto error = manager.Load(location->FileName); error != Loader::ModuleError::None)
    {
        logger.Error("Launch: loading {} failed ({})", executable->Name, Loader::ToString(error));
        result.Error = LaunchError::LoadFailed;
    }
    else if (initializer)
    {
        const auto* module = manager.Find(location->FileName);
        result.ExitCode = module == nullptr ? std::nullopt : initializer->RunEntryPoint(*module);
        if (!result.ExitCode)
        {
            result.Error = LaunchError::StartFailed;
        }
    }
    else
    {
        logger.Info("Launch: loaded and linked {} module(s); not started", manager.ModuleCount());
    }

    manager.Shutdown();
    stubs.LogSummary();
    return result;
}
} // namespace OpenEra::App
