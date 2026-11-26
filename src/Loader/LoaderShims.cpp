// Runtime loader shims (LoadLibrary, GetModuleHandle, GetProcAddress,
// FreeLibrary) and the image-base lookups C++ exceptions need, resolved
// through the active ModuleManager.
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include <Windows.h>

#include "Kernel/FileShims.h"
#include "Kernel/MemoryShims.h"
#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"
#include "Loader/ModuleManager.h"

namespace OpenEra::Loader
{
namespace
{
// Low 16 bits (x64 is little-endian).
constexpr uint16_t LowWord(uintptr_t value) noexcept
{
    return std::bit_cast<std::array<uint16_t, sizeof(uintptr_t) / sizeof(uint16_t)>>(value).front();
}

std::string Narrow(LPCWSTR text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
    {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    result.pop_back(); // Terminator.
    return result;
}

HMODULE LoadByName(std::string_view name)
{
    auto* manager = ModuleManager::Active();
    if (manager == nullptr || name.empty())
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const uint64_t handle = manager->LoadForRuntime(name);
    if (handle == 0)
    {
        SetLastError(ERROR_MOD_NOT_FOUND);
    }
    return std::bit_cast<HMODULE>(handle);
}

HMODULE HandleByName(const char* name)
{
    auto* manager = ModuleManager::Active();
    if (manager == nullptr)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const uint64_t handle = name == nullptr ? manager->MainModuleHandle() : manager->HandleOf(name);
    if (handle == 0)
    {
        SetLastError(ERROR_MOD_NOT_FOUND);
    }
    return std::bit_cast<HMODULE>(handle);
}

HMODULE WINAPI LoadLibraryExWShim(LPCWSTR name, HANDLE /*file*/, DWORD /*flags*/) noexcept
{
    return Kernel::GuardShim<HMODULE>("LoadLibraryExW", nullptr, nullptr,
                                      [name] { return name == nullptr ? LoadByName({}) : LoadByName(Narrow(name)); });
}

HMODULE WINAPI LoadLibraryWShim(LPCWSTR name) noexcept
{
    return LoadLibraryExWShim(name, nullptr, 0);
}

HMODULE WINAPI LoadLibraryExAShim(LPCSTR name, HANDLE /*file*/, DWORD /*flags*/) noexcept
{
    return Kernel::GuardShim<HMODULE>("LoadLibraryExA", nullptr, nullptr,
                                      [name] { return LoadByName(name == nullptr ? std::string_view{} : name); });
}

HMODULE WINAPI LoadLibraryAShim(LPCSTR name) noexcept
{
    return LoadLibraryExAShim(name, nullptr, 0);
}

HMODULE WINAPI GetModuleHandleWShim(LPCWSTR name) noexcept
{
    return Kernel::GuardShim<HMODULE>("GetModuleHandleW", nullptr, nullptr,
                                      [name]
                                      {
                                          if (name == nullptr)
                                          {
                                              return HandleByName(nullptr);
                                          }
                                          const std::string narrow = Narrow(name);
                                          return HandleByName(narrow.c_str());
                                      });
}

HMODULE WINAPI GetModuleHandleAShim(LPCSTR name) noexcept
{
    return Kernel::GuardShim<HMODULE>("GetModuleHandleA", nullptr, nullptr, [name] { return HandleByName(name); });
}

FARPROC WINAPI GetProcAddressShim(HMODULE module, LPCSTR name) noexcept
{
    return Kernel::GuardShim<FARPROC>("GetProcAddress", nullptr, nullptr,
                                      [module, name]() -> FARPROC
                                      {
                                          auto* manager = ModuleManager::Active();
                                          if (manager == nullptr || name == nullptr)
                                          {
                                              SetLastError(ERROR_INVALID_PARAMETER);
                                              return nullptr;
                                          }
                                          const auto handle = std::bit_cast<uint64_t>(module);
                                          const auto nameValue = std::bit_cast<uintptr_t>(name);
                                          // Values below 0x10000 are ordinals (MAKEINTRESOURCEA).
                                          const uint64_t address =
                                              nameValue <= 0xFFFF
                                                  ? manager->ProcAddress(handle, LowWord(nameValue))
                                                  : manager->ProcAddress(handle, std::string_view(name));
                                          if (address == 0)
                                          {
                                              SetLastError(ERROR_PROC_NOT_FOUND);
                                          }
                                          return std::bit_cast<FARPROC>(address);
                                      });
}

BOOL WINAPI FreeLibraryShim(HMODULE /*module*/) noexcept
{
    return TRUE; // Title modules stay loaded for the life of the process.
}

// The OS only knows images its own loader mapped, so title modules are looked up first.
uint64_t ImageBaseOf(void* address)
{
    const auto* manager = ModuleManager::Active();
    if (manager != nullptr)
    {
        if (const uint64_t base = manager->ImageBaseFor(std::bit_cast<uint64_t>(address)); base != 0)
        {
            return base;
        }
    }
    PVOID base = nullptr;
    RtlPcToFileHeader(address, &base);
    return std::bit_cast<uint64_t>(base);
}

PVOID NTAPI RtlPcToFileHeaderShim(PVOID pc, PVOID* baseOfImage) noexcept
{
    return Kernel::GuardShim<PVOID>("RtlPcToFileHeader", nullptr, nullptr,
                                    [pc, baseOfImage]() -> PVOID
                                    {
                                        if (baseOfImage == nullptr)
                                        {
                                            return nullptr;
                                        }
                                        *baseOfImage = std::bit_cast<PVOID>(ImageBaseOf(pc));
                                        return *baseOfImage;
                                    });
}

// MSVC C++ exception record: code 0xE06D7363 ("msc"), then the magic number
// 0x19930520, the thrown object, its throw info, and the image base the throw
// info's RVAs are relative to. It only raises, so it's neither noexcept nor guarded.
constexpr DWORD kCxxExceptionCode = 0xE06D'7363;
constexpr ULONG_PTR kCxxMagic = 0x1993'0520;
constexpr DWORD kCxxParameterCount = 4;

[[gsl::suppress("f.6")]] [[noreturn]] void CxxThrowExceptionShim(void* object, void* throwInfo)
{
    // A rethrow (throw;) passes no throw info, so there's no image base either.
    const uint64_t imageBase = throwInfo == nullptr ? 0 : ImageBaseOf(throwInfo);
    const std::array<ULONG_PTR, kCxxParameterCount> parameters = {kCxxMagic, std::bit_cast<ULONG_PTR>(object),
                                                                  std::bit_cast<ULONG_PTR>(throwInfo), imageBase};
    RaiseException(kCxxExceptionCode, EXCEPTION_NONCONTINUABLE, kCxxParameterCount, parameters.data());
    std::abort(); // Non-continuable: never reached.
}
} // namespace

bool RegisterLoaderShims(Kernel::ShimRegistry& shims)
{
    Kernel::ShimModule kernel("kernel32.dll", Kernel::StubKind::Win32Import);
    kernel.AddAlias("kernelbase.dll");
    kernel.AddAlias("api-ms-win-core-libraryloader-l1-2-0.dll");
    kernel.AddAlias("api-ms-win-core-libraryloader-l1-2-1.dll");
    for (const char* alias :
         {"api-ms-win-core-file-l1-1-0.dll", "api-ms-win-core-file-l1-2-0.dll", "api-ms-win-core-file-l1-2-1.dll",
          "api-ms-win-core-file-l1-2-2.dll", "api-ms-win-core-memory-l1-1-0.dll", "api-ms-win-core-memory-l1-1-1.dll",
          "api-ms-win-core-memory-l1-1-2.dll"})
    {
        kernel.AddAlias(alias);
    }
    const bool exported = kernel.Export("LoadLibraryExW", Kernel::ToShimProc(&LoadLibraryExWShim)) &&
                          kernel.Export("LoadLibraryW", Kernel::ToShimProc(&LoadLibraryWShim)) &&
                          kernel.Export("LoadLibraryExA", Kernel::ToShimProc(&LoadLibraryExAShim)) &&
                          kernel.Export("LoadLibraryA", Kernel::ToShimProc(&LoadLibraryAShim)) &&
                          kernel.Export("GetModuleHandleW", Kernel::ToShimProc(&GetModuleHandleWShim)) &&
                          kernel.Export("GetModuleHandleA", Kernel::ToShimProc(&GetModuleHandleAShim)) &&
                          kernel.Export("GetProcAddress", Kernel::ToShimProc(&GetProcAddressShim)) &&
                          kernel.Export("FreeLibrary", Kernel::ToShimProc(&FreeLibraryShim)) &&
                          kernel.Export("RtlPcToFileHeader", Kernel::ToShimProc(&RtlPcToFileHeaderShim)) &&
                          Kernel::AddFileExports(kernel) && Kernel::AddMemoryExports(kernel);

    Kernel::ShimModule ntdll("ntdll.dll", Kernel::StubKind::Win32Import);
    ntdll.AddAlias("api-ms-win-core-rtlsupport-l1-1-0.dll");
    ntdll.AddAlias("api-ms-win-core-rtlsupport-l1-2-0.dll");
    const bool ntdllExported = ntdll.Export("RtlPcToFileHeader", Kernel::ToShimProc(&RtlPcToFileHeaderShim));

    Kernel::ShimModule vcruntime("vcruntime140.dll", Kernel::StubKind::Win32Import);
    const bool vcruntimeExported = vcruntime.Export("_CxxThrowException", Kernel::ToShimProc(&CxxThrowExceptionShim));

    return exported && ntdllExported && vcruntimeExported && shims.Register(std::move(kernel)) &&
           shims.Register(std::move(ntdll)) && shims.Register(std::move(vcruntime));
}
} // namespace OpenEra::Loader
