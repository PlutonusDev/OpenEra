#include "TitleLauncher.h"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iterator>
#include <memory>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <Windows.h>
#include <timeapi.h>

#include "App/Settings.h"
#include "Core/Logger.h"
#include "Graphics/GraphicsShims.h"
#include "Kernel/ExitShims.h"
#include "Kernel/GameRuntimeShims.h"
#include "Kernel/ProcessShims.h"
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

constexpr UINT kTimerResolutionMs = 1;

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

// 1 ms timer resolution while a title runs: console titles expect Sleep and
// timed waits to be that precise, and desktop Windows defaults to 15.6 ms.
class TimerResolutionScope
{
public:
    TimerResolutionScope() noexcept
        : m_raised(timeBeginPeriod(kTimerResolutionMs) == TIMERR_NOERROR)
    {
    }
    ~TimerResolutionScope()
    {
        if (m_raised)
        {
            timeEndPeriod(kTimerResolutionMs);
        }
    }

    TimerResolutionScope(const TimerResolutionScope&) = delete;
    TimerResolutionScope& operator=(const TimerResolutionScope&) = delete;
    TimerResolutionScope(TimerResolutionScope&&) = delete;
    TimerResolutionScope& operator=(TimerResolutionScope&&) = delete;

    [[nodiscard]] bool Raised() const noexcept { return m_raised; }

private:
    bool m_raised;
};

// The title as the process image (its executable, command line and the title
// folder as the current directory) while it's alive; everything restored after.
class TitleProcessScope
{
public:
    TitleProcessScope(Kernel::TitleProcessIdentity identity, const std::filesystem::path& folder, Logger& logger)
    {
        std::error_code error;
        m_previousDirectory = std::filesystem::current_path(error);
        std::filesystem::current_path(folder, error);
        if (error)
        {
            logger.Warn("Launch: couldn't make {} the current directory ({})", folder.string(), error.message());
        }
        Kernel::SetTitleProcess(std::move(identity));
        Kernel::ArmTitleExit();
        // The title's C runtime installs its own; it mustn't outlive the title's code.
        m_previousFilter = SetUnhandledExceptionFilter(nullptr);
        SetUnhandledExceptionFilter(m_previousFilter);
    }

    ~TitleProcessScope()
    {
        if (m_keep)
        {
            return;
        }
        try
        {
            SetUnhandledExceptionFilter(m_previousFilter);
            Kernel::DisarmTitleExit();
            Kernel::SetTitleProcess(std::nullopt);
            std::error_code error;
            std::filesystem::current_path(m_previousDirectory, error);
        }
        catch (const std::exception&)
        {
            return; // Best effort: the title has finished.
        }
    }

    TitleProcessScope(const TitleProcessScope&) = delete;
    TitleProcessScope& operator=(const TitleProcessScope&) = delete;
    TitleProcessScope(TitleProcessScope&&) = delete;
    TitleProcessScope& operator=(TitleProcessScope&&) = delete;

    // Leaves everything as the title sees it, for its threads that are still running.
    void Keep() noexcept { m_keep = true; }

private:
    std::filesystem::path m_previousDirectory;
    LPTOP_LEVEL_EXCEPTION_FILTER m_previousFilter = nullptr;
    bool m_keep = false;
};

// Keeps the file shims pointed at `files` while it's alive.
class ActiveFileSystemScope
{
public:
    explicit ActiveFileSystemScope(Kernel::TitleFileSystem& files) noexcept
    {
        Kernel::TitleFileSystem::SetActive(&files);
    }
    ~ActiveFileSystemScope()
    {
        if (!m_keep)
        {
            Kernel::TitleFileSystem::SetActive(nullptr);
        }
    }

    ActiveFileSystemScope(const ActiveFileSystemScope&) = delete;
    ActiveFileSystemScope& operator=(const ActiveFileSystemScope&) = delete;
    ActiveFileSystemScope(ActiveFileSystemScope&&) = delete;
    ActiveFileSystemScope& operator=(ActiveFileSystemScope&&) = delete;

    void Keep() noexcept { m_keep = true; }

private:
    bool m_keep = false;
};

// Keeps the loader shims pointed at `manager` while it's alive.
class ActiveManagerScope
{
public:
    explicit ActiveManagerScope(ModuleManager& manager) noexcept { ModuleManager::SetActive(&manager); }
    ~ActiveManagerScope()
    {
        if (!m_keep)
        {
            ModuleManager::SetActive(nullptr);
        }
    }

    ActiveManagerScope(const ActiveManagerScope&) = delete;
    ActiveManagerScope& operator=(const ActiveManagerScope&) = delete;
    ActiveManagerScope(ActiveManagerScope&&) = delete;
    ActiveManagerScope& operator=(ActiveManagerScope&&) = delete;

    void Keep() noexcept { m_keep = true; }

private:
    bool m_keep = false;
};

// Everything a running title uses. When title threads outlive the run they may
// still be executing its code, stubs and files, so it's then left alive until
// openera exits, as the process would have ended them.
struct TitleSession
{
    explicit TitleSession(Logger& logger)
        : Stubs(logger)
    {
    }

    Kernel::ShimRegistry Shims;
    Kernel::StubRegistry Stubs;
    std::optional<Kernel::TitleFileSystem> Files;
    std::unique_ptr<Loader::ImageAllocator> Allocator = Loader::CreateVirtualImageAllocator();
    std::unique_ptr<Loader::HostModules> Host = Loader::HostModules::CreateDefault();
    // Declared before the manager, so it's destroyed after it.
    std::optional<Loader::ImageInitializer> Initializer;
    std::optional<ModuleManager> Manager;
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
    // Absolute up front: the title folder becomes the current directory before anything is loaded.
    std::error_code folderError;
    const auto folder = std::filesystem::absolute(options.Folder, folderError).lexically_normal();
    auto config = ReadTitleConfig(folder, logger);
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

    auto session = std::make_unique<TitleSession>(logger);
    if (!Loader::RegisterLoaderShims(session->Shims) ||
        !Graphics::RegisterGraphicsShims(session->Shims, &session->Stubs) ||
        !Kernel::RegisterGameRuntimeShims(session->Shims))
    {
        result.Error = LaunchError::LoadFailed;
        return result;
    }
    Kernel::SetGameRuntimeTitle({.TitleId = Kernel::ParseTitleId(config.Config->TitleId),
                                 .Device = Kernel::DeviceForFamily(executable->TargetDeviceFamily)});
    auto& files = session->Files.emplace(folder, options.OverlayDirectory.empty()
                                                     ? TitleDataDirectory(config.Config->IdentityName) / "Overlay"
                                                     : options.OverlayDirectory);
    ActiveFileSystemScope activeFiles(files);
    logger.Info("Launch: title writes go to {}", files.OverlayRoot().string());

    auto& initializer = session->Initializer;
    if (options.Start)
    {
        initializer.emplace(options.TlsSlot.empty() ? Loader::ImageInitializer::DefaultTlsSlotPath() : options.TlsSlot,
                            options.WorkDirectory.empty() ? std::filesystem::temp_directory_path() / "OpenEra"
                                                          : options.WorkDirectory,
                            logger);
    }
    auto& manager =
        session->Manager.emplace(Loader::DirectoryModuleReader(folder / location->Directory), session->Shims,
                                 session->Stubs, *session->Allocator, logger, session->Host.get());
    if (initializer)
    {
        manager.SetInitializer(&*initializer);
    }
    const auto imageDirectory = (folder / location->Directory).lexically_normal();
    manager.SetImageDirectory(imageDirectory);
    const auto image =
        imageDirectory / std::filesystem::path(std::u8string(location->FileName.begin(), location->FileName.end()));
    TitleProcessScope process({.Image = image, .CommandLine = Kernel::BuildCommandLine(image, options.Arguments)},
                              folder, logger);
    ActiveManagerScope active(manager);
    const TimerResolutionScope timer;
    if (!timer.Raised())
    {
        logger.Warn("Launch: couldn't raise the timer resolution to {} ms", kTimerResolutionMs);
    }

    if (const auto error = manager.Load(location->FileName); error != Loader::ModuleError::None)
    {
        logger.Error("Launch: loading {} failed ({})", executable->Name, Loader::ToString(error));
        result.Error = LaunchError::LoadFailed;
    }
    else if (initializer)
    {
        const auto* module = manager.Find(location->FileName);
        result.ExitCode = module == nullptr ? std::nullopt : initializer->RunEntryPoint(*module);
        result.ExitRequested = Kernel::TitleExitCode().has_value();
        if (!result.ExitCode)
        {
            result.Error = LaunchError::StartFailed;
        }
    }
    else
    {
        logger.Info("Launch: loaded and linked {} module(s); not started", manager.ModuleCount());
    }

    if (initializer && initializer->RunningThreadCount() != 0)
    {
        const size_t running = initializer->RunningThreadCount();
        // Shutting down under them would pull their code, TLS and files away.
        logger.Info("Launch: {} title thread(s) still running; leaving the title loaded until openera exits", running);
        initializer->KeepLoaded();
        activeFiles.Keep();
        active.Keep();
        process.Keep();
        session->Stubs.LogSummary();
        std::ignore = session.release(); // Intentionally kept for those threads.
        result.LeftRunning = true;
        return result;
    }

    manager.Shutdown();
    session->Stubs.LogSummary();
    return result;
}
} // namespace OpenEra::App
