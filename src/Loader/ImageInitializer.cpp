#include "ImageInitializer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Windows.h>
#include <tlhelp32.h>

#include "Core/Logger.h"
#include "Kernel/ExitShims.h"
#include "Kernel/ShimGuard.h"
#include "Loader/ImageMapper.h"

namespace OpenEra::Loader
{
namespace
{
using Core::Logger;
using TlsCallback = void NTAPI(PVOID, DWORD, PVOID) noexcept;
using DllEntry = BOOL WINAPI(HINSTANCE, DWORD, LPVOID) noexcept;
using ThreadNotify = void (*)(unsigned long reason) noexcept;
using SetNotifyFunction = void(ThreadNotify) noexcept;

constexpr uint16_t kFileDll = 0x2000; // IMAGE_FILE_DLL in the PE Format specification.
constexpr uint32_t kNotifierTemplateSize = 8;
constexpr DWORD kMaxPath = 32'768;
// How long the main thread gets to finish once another title thread asked to exit.
constexpr DWORD kExitGraceMs = 2'000;
constexpr std::string_view kSlotFileName = "OpenEraTlsSlot.dll";

struct LibraryDeleter
{
    void operator()(HMODULE module) const noexcept { FreeLibrary(module); }
};
using Library = std::unique_ptr<std::remove_pointer_t<HMODULE>, LibraryDeleter>;

struct HandleCloser
{
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

struct Slot
{
    Library Module;
    std::filesystem::path File;
    const LoadedModule* Owner = nullptr; // Null for the notifier.
};

// Ids of this process's threads that are still running.
std::unordered_set<DWORD> RunningProcessThreads()
{
    std::unordered_set<DWORD> threads;
    auto* const raw = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (raw == INVALID_HANDLE_VALUE)
    {
        return threads;
    }
    const Handle snapshot(raw);
    const DWORD process = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot.get(), &entry); more != FALSE; more = Thread32Next(snapshot.get(), &entry))
    {
        if (entry.th32OwnerProcessID != process)
        {
            continue;
        }
        const Handle thread(OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID));
        DWORD exitCode = 0;
        if (thread && GetExitCodeThread(thread.get(), &exitCode) != FALSE && exitCode == STILL_ACTIVE)
        {
            threads.insert(entry.th32ThreadID);
        }
    }
    return threads;
}

uint32_t LowDword(uint64_t value) noexcept
{
    return std::bit_cast<std::array<uint32_t, 2>>(value).front();
}

bool IsDll(const PeImage& image) noexcept
{
    return (image.FileHeader.Characteristics & kFileDll) != 0;
}

// TLS directory fields are VAs at the preferred base.
std::optional<uint32_t> VaToRva(const PeImage& image, uint64_t va) noexcept
{
    const uint64_t base = image.OptionalHeader.ImageBase;
    if (va < base || va - base >= image.OptionalHeader.SizeOfImage)
    {
        return std::nullopt;
    }
    return LowDword(va - base);
}

// Title code is called through pointers built from its RVAs, which are never null.
[[gsl::suppress("f.23")]] void RunTlsCallbacks(const LoadedModule& module, DWORD reason) noexcept
{
    if (!module.Pe.Tls)
    {
        return;
    }
    const uint64_t base = module.Base();
    for (const uint32_t rva : module.Pe.Tls->CallbackRvas)
    {
        auto* const callback = std::bit_cast<TlsCallback*>(base + rva);
        callback(std::bit_cast<PVOID>(base), reason, nullptr);
    }
}

[[gsl::suppress("f.23")]] std::optional<BOOL> CallDllMain(const LoadedModule& module, DWORD reason) noexcept
{
    const uint32_t rva = module.Pe.OptionalHeader.AddressOfEntryPoint;
    if (!IsDll(module.Pe) || rva == 0)
    {
        return std::nullopt;
    }
    auto* const entry = std::bit_cast<DllEntry*>(module.Base() + rva);
    return entry(std::bit_cast<HINSTANCE>(module.Base()), reason, nullptr);
}

std::optional<std::vector<std::byte>> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    const std::vector<char> raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char value) { return std::bit_cast<std::byte>(value); });
    return bytes;
}

bool WriteFileBytes(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::vector<char> raw(bytes.size());
    std::ranges::transform(bytes, raw.begin(), [](std::byte value) { return std::bit_cast<char>(value); });
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(raw.data(), std::ssize(raw));
    return static_cast<bool>(stream);
}

// Writes the module's TLS index variable, which may sit on a read-only page.
bool WriteTlsIndex(const LoadedModule& module, uint32_t rva, uint32_t index)
{
    auto& memory = *module.Image.Memory;
    const auto section = std::ranges::find_if(
        module.Pe.Sections, [rva](const SectionHeader& header) noexcept
        { return rva >= header.VirtualAddress && rva - header.VirtualAddress < header.VirtualSize; });
    const bool inSection = section != module.Pe.Sections.end();
    const bool writable = inSection && (section->Characteristics & kSectionMemoryWrite) != 0;
    const size_t page = rva - (rva % kPageSize);
    if (!writable && !memory.Protect(page, kPageSize, PageProtection::ReadWrite))
    {
        return false;
    }
    std::ranges::copy(std::bit_cast<std::array<std::byte, sizeof(uint32_t)>>(index),
                      memory.Bytes().subspan(rva, sizeof(uint32_t)).begin());
    if (writable)
    {
        return true;
    }
    const auto restore = inSection ? SectionProtection(section->Characteristics) : PageProtection::ReadOnly;
    return memory.Protect(page, kPageSize, restore);
}
} // namespace

struct ImageInitializer::State
{
    std::filesystem::path SlotSource;
    std::filesystem::path WorkDirectory;
    std::reference_wrapper<Logger> Log;

    // Slots are only touched while attaching (serialised by the manager) and on destruction.
    std::vector<Slot> Slots;
    uint32_t NextCopy = 0;
    bool NotifierLoaded = false;

    // Guards Attached only. Never held while calling title code or the OS loader,
    // since thread notifications arrive under the OS loader lock.
    std::mutex Mutex;
    std::vector<std::reference_wrapper<const LoadedModule>> Attached; // Thread notifications, in attach order.
    // Held shared while notifications run, so an unloaded module waits them out.
    std::shared_mutex Notifying;
    std::unordered_set<DWORD> LiveThreads;     // Threads started since the first attach, still running.
    std::unordered_set<DWORD> BaselineThreads; // Threads running before the first attach.
    bool KeepSlotsLoaded = false;

    State(std::filesystem::path slotSource, std::filesystem::path workDirectory, Logger& log)
        : SlotSource(std::move(slotSource))
        , WorkDirectory(std::move(workDirectory))
        , Log(log)
    {
    }

    static std::atomic<State*>& Active() noexcept
    {
        static std::atomic<State*> active{nullptr};
        return active;
    }

    static void OnThread(unsigned long reason) noexcept
    {
        try
        {
            State* const state = Active().load();
            if (state == nullptr)
            {
                return;
            }
            const std::shared_lock notifying(state->Notifying);
            std::vector<std::reference_wrapper<const LoadedModule>> modules;
            {
                const std::scoped_lock lock(state->Mutex);
                modules = state->Attached;
                if (reason == DLL_THREAD_ATTACH)
                {
                    state->LiveThreads.insert(GetCurrentThreadId());
                }
                else
                {
                    state->LiveThreads.erase(GetCurrentThreadId());
                }
            }
            for (const LoadedModule& module : modules)
            {
                RunTlsCallbacks(module, reason);
                std::ignore = CallDllMain(module, reason);
            }
        }
        catch (const std::exception& exception)
        {
            Kernel::ReportShimException("ImageInitializer thread notification", exception);
        }
        catch (...)
        {
            Kernel::ReportShimException("ImageInitializer thread notification", "unknown exception");
        }
    }

    // Loads a copy of the slot DLL carrying `tlsTemplate`, returning its TLS index.
    std::optional<uint32_t> CreateSlot(std::span<const std::byte> tlsTemplate, uint32_t zeroFill,
                                       uint32_t characteristics)
    {
        auto& log = Log.get();
        auto bytes = ReadFileBytes(SlotSource);
        if (!bytes)
        {
            log.Warn("Modules: TLS slot {} not found", SlotSource.string());
            return std::nullopt;
        }
        const auto parsed = ParsePe(*bytes, log);
        if (!parsed.Image || !parsed.Image->Tls)
        {
            log.Warn("Modules: {} is not a TLS slot image", SlotSource.string());
            return std::nullopt;
        }
        const auto& slotImage = *parsed.Image;
        const auto& directory = parsed.Image->Tls->Directory;
        const auto directoryEntry = slotImage.Directory(DataDirectoryIndex::Tls);
        const auto startRva = VaToRva(slotImage, directory.RawDataStartVa);
        const auto indexRva = VaToRva(slotImage, directory.IndexAddressVa);
        const auto startOffset = startRva ? RvaToFileOffset(slotImage, *startRva) : std::nullopt;
        const auto directoryOffset =
            directoryEntry ? RvaToFileOffset(slotImage, directoryEntry->VirtualAddress) : std::nullopt;
        if (!startOffset || !directoryOffset || !indexRva || directory.RawDataEndVa < directory.RawDataStartVa)
        {
            log.Warn("Modules: {} has an unusable TLS directory", SlotSource.string());
            return std::nullopt;
        }
        const uint64_t capacity = directory.RawDataEndVa - directory.RawDataStartVa;
        if (tlsTemplate.size() > capacity)
        {
            log.Warn("Modules: TLS template of {} bytes exceeds the slot's {}", tlsTemplate.size(), capacity);
            return std::nullopt;
        }
        if (*startOffset + capacity > bytes->size() || *directoryOffset + sizeof(TlsDirectory64) > bytes->size())
        {
            log.Warn("Modules: {} is truncated", SlotSource.string());
            return std::nullopt;
        }

        const std::span<std::byte> file(*bytes);
        std::ranges::copy(tlsTemplate, file.subspan(*startOffset).begin());
        TlsDirectory64 patched = directory;
        patched.RawDataEndVa = directory.RawDataStartVa + tlsTemplate.size();
        patched.ZeroFillSize = zeroFill;
        patched.Characteristics = characteristics;
        std::ranges::copy(std::bit_cast<std::array<std::byte, sizeof(TlsDirectory64)>>(patched),
                          file.subspan(*directoryOffset).begin());

        std::error_code error;
        std::filesystem::create_directories(WorkDirectory, error);
        auto path = WorkDirectory / std::format("OpenEraTlsSlot-{}-{}.dll", GetCurrentProcessId(), NextCopy++);
        if (!WriteFileBytes(path, file))
        {
            log.Warn("Modules: writing TLS slot {} failed", path.string());
            return std::nullopt;
        }
        Library module(LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
        if (!module)
        {
            log.Warn("Modules: loading TLS slot {} failed ({})", path.string(), GetLastError());
            std::filesystem::remove(path, error);
            return std::nullopt;
        }
        // The OS loader stores the index it assigned in the slot's own index variable.
        const auto* const indexAddress =
            std::bit_cast<const uint32_t*>(std::bit_cast<uint64_t>(module.get()) + *indexRva);
        if (indexAddress == nullptr)
        {
            return std::nullopt;
        }
        const uint32_t index = *indexAddress;
        Slots.push_back({.Module = std::move(module), .File = std::move(path)});
        return index;
    }

    // A slot with a tiny template that forwards thread notifications, so they
    // reach title modules whether or not any of them use TLS.
    bool EnsureNotifier()
    {
        if (NotifierLoaded)
        {
            return true;
        }
        constexpr std::array<std::byte, kNotifierTemplateSize> kEmpty{};
        if (!CreateSlot(kEmpty, 0, 0))
        {
            return false;
        }
        auto* const proc = GetProcAddress(Slots.back().Module.get(), "OpenEraTlsSlotSetNotify");
        if (proc == nullptr)
        {
            Log.get().Warn("Modules: {} has no thread notification export", SlotSource.string());
            return false;
        }
        BaselineThreads = RunningProcessThreads();
        Active().store(this);
        std::bit_cast<SetNotifyFunction*>(std::bit_cast<uintptr_t>(proc))(&OnThread);
        NotifierLoaded = true;
        return true;
    }

    bool SetUpTls(const LoadedModule& module, const PeTls& tls)
    {
        auto& log = Log.get();
        const auto& directory = tls.Directory;
        const auto bytes = module.Image.Memory->Bytes();
        const auto indexRva = VaToRva(module.Pe, directory.IndexAddressVa);
        const uint64_t size =
            directory.RawDataEndVa > directory.RawDataStartVa ? directory.RawDataEndVa - directory.RawDataStartVa : 0;
        const auto startRva = size == 0 ? std::optional<uint32_t>(0) : VaToRva(module.Pe, directory.RawDataStartVa);
        if (!indexRva || !startRva || *startRva + size > bytes.size() || *indexRva + sizeof(uint32_t) > bytes.size())
        {
            log.Warn("Modules: {} has an unusable TLS directory", module.Name);
            return false;
        }
        // Copied from the mapped image, so pointers in the template are already relocated.
        const auto index =
            CreateSlot(bytes.subspan(*startRva, size), directory.ZeroFillSize, directory.Characteristics);
        if (!index || !WriteTlsIndex(module, *indexRva, *index))
        {
            log.Warn("Modules: setting up TLS for {} failed", module.Name);
            return false;
        }
        Slots.back().Owner = &module;
        log.Info("Modules: {} uses TLS index {}", module.Name, *index);
        return true;
    }
};

ImageInitializer::ImageInitializer(std::filesystem::path tlsSlot, std::filesystem::path workDirectory,
                                   Core::Logger& logger)
    : m_state(std::make_unique<State>(std::move(tlsSlot), std::move(workDirectory), logger))
{
}

// Copy and move are deleted by ModuleInitializer, and redeclaring them here is C26456.
[[gsl::suppress("c.21")]] ImageInitializer::~ImageInitializer()
{
    State* expected = m_state.get();
    State::Active().compare_exchange_strong(expected, nullptr);
    // Freeing a slot waits for the OS loader lock, so in-flight notifications finish first.
    // With title threads still running, freeing it would pull their TLS away; the
    // process is about to end anyway, so the slots stay loaded (their files stay too).
    while (!m_state->KeepSlotsLoaded && !m_state->Slots.empty())
    {
        Slot& slot = m_state->Slots.back();
        slot.Module.reset();
        std::error_code error;
        std::filesystem::remove(slot.File, error);
        m_state->Slots.pop_back();
    }
}

bool ImageInitializer::Attach(const LoadedModule& module)
{
    auto& state = *m_state;
    if (!state.EnsureNotifier() || (module.Pe.Tls && !state.SetUpTls(module, *module.Pe.Tls)))
    {
        return false;
    }
    RunTlsCallbacks(module, DLL_PROCESS_ATTACH);
    if (const auto result = CallDllMain(module, DLL_PROCESS_ATTACH))
    {
        state.Log.get().Info("Modules: {} DllMain(DLL_PROCESS_ATTACH) returned {}", module.Name, *result);
        if (*result == FALSE)
        {
            return false;
        }
    }
    const std::scoped_lock lock(state.Mutex);
    state.Attached.emplace_back(module);
    return true;
}

void ImageInitializer::Detach(const LoadedModule& module)
{
    auto& state = *m_state;
    {
        const std::scoped_lock lock(state.Mutex);
        std::erase_if(state.Attached, [&module](const LoadedModule& entry) noexcept { return &entry == &module; });
    }
    if (const auto result = CallDllMain(module, DLL_PROCESS_DETACH))
    {
        state.Log.get().Info("Modules: {} DllMain(DLL_PROCESS_DETACH) returned {}", module.Name, *result);
    }
    RunTlsCallbacks(module, DLL_PROCESS_DETACH);
}

void ImageInitializer::Unloading(const LoadedModule& module) noexcept
{
    auto& state = *m_state;
    // Detach already removed it, so only notifications already running can still reach it.
    state.Notifying.lock();
    state.Notifying.unlock();
    const auto slot =
        std::ranges::find_if(state.Slots, [&module](const Slot& entry) noexcept { return entry.Owner == &module; });
    if (slot != state.Slots.end())
    {
        slot->Module.reset();
        std::error_code error;
        std::filesystem::remove(slot->File, error);
        state.Slots.erase(slot);
    }
}

std::optional<uint32_t> ImageInitializer::RunEntryPoint(const LoadedModule& module)
{
    auto& log = m_state->Log.get();
    const uint32_t rva = module.Pe.OptionalHeader.AddressOfEntryPoint;
    if (IsDll(module.Pe) || rva == 0)
    {
        log.Warn("Modules: {} has no executable entry point", module.Name);
        return std::nullopt;
    }
    auto* const start = std::bit_cast<LPTHREAD_START_ROUTINE>(module.Base() + rva);
    const SIZE_T stackReserve = module.Pe.OptionalHeader.SizeOfStackReserve;
    const Handle thread(
        CreateThread(nullptr, stackReserve, start, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    if (!thread)
    {
        log.Warn("Modules: starting {} failed ({})", module.Name, GetLastError());
        return std::nullopt;
    }
    // The entry point returning, or any title thread ending "the process" (Kernel::RequestTitleExit).
    const std::array<HANDLE, 2> waits = {thread.get(), Kernel::TitleExitEvent()};
    const DWORD signalled = WaitForMultipleObjects(2, waits.data(), FALSE, INFINITE);
    if (signalled == WAIT_OBJECT_0 + 1 && WaitForSingleObject(thread.get(), kExitGraceMs) == WAIT_TIMEOUT)
    {
        // Another thread asked to exit; the main thread would have been ended with the process.
        const uint32_t code = Kernel::TitleExitCode().value_or(0);
        log.Info("Modules: {} asked to exit; stopping its main thread", module.Name);
        TerminateThread(thread.get(), code);
        WaitForSingleObject(thread.get(), INFINITE);
        const std::scoped_lock lock(m_state->Mutex);
        m_state->LiveThreads.erase(GetThreadId(thread.get()));
    }
    DWORD exitCode = 0;
    if (GetExitCodeThread(thread.get(), &exitCode) == FALSE)
    {
        return std::nullopt;
    }
    if (const auto requested = Kernel::TitleExitCode())
    {
        log.Info("Modules: {} exited the process with {}", module.Name, *requested);
        return *requested;
    }
    log.Info("Modules: {} entry point returned {}", module.Name, exitCode);
    return exitCode;
}

size_t ImageInitializer::RunningThreadCount() const
{
    // A snapshot too: a thread just created may not have sent its attach notification yet.
    // Without a baseline nothing was attached, so no title code ever ran.
    auto running = m_state->NotifierLoaded ? RunningProcessThreads() : std::unordered_set<DWORD>{};
    std::erase_if(running, [this](DWORD id) { return m_state->BaselineThreads.contains(id); });
    {
        const std::scoped_lock lock(m_state->Mutex);
        running.insert(m_state->LiveThreads.begin(), m_state->LiveThreads.end());
    }
    running.erase(GetCurrentThreadId());
    return running.size();
}

void ImageInitializer::KeepLoaded() noexcept
{
    m_state->KeepSlotsLoaded = true;
}

std::filesystem::path ImageInitializer::DefaultTlsSlotPath()
{
    std::wstring path(kMaxPath, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), kMaxPath);
    path.resize(length);
    return std::filesystem::path(path).parent_path() / kSlotFileName;
}
} // namespace OpenEra::Loader
