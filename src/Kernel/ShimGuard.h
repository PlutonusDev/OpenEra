#pragma once

#include <bit>
#include <cstdint>
#include <exception>
#include <new>
#include <string_view>
#include <utility>

#include "Kernel/StubRegistry.h"

namespace OpenEra::Kernel
{
// Status codes returned across shim boundaries. Values are the documented
// HRESULT / NTSTATUS constants (Microsoft Learn: "Common HRESULT Values",
// "NTSTATUS Values"); our own names keep <Windows.h> out of this header.
using HResult = int32_t;
using NtStatus = int32_t;

constexpr HResult kHResultOk = 0;
constexpr HResult kHResultNotImplemented = std::bit_cast<HResult>(0x8000'4001U);
constexpr HResult kHResultFail = std::bit_cast<HResult>(0x8000'4005U);
constexpr HResult kHResultOutOfMemory = std::bit_cast<HResult>(0x8007'000EU);
constexpr HResult kHResultInvalidArgument = std::bit_cast<HResult>(0x8007'0057U);

constexpr NtStatus kStatusSuccess = 0;
constexpr NtStatus kStatusUnsuccessful = std::bit_cast<NtStatus>(0xC000'0001U);
constexpr NtStatus kStatusNotImplemented = std::bit_cast<NtStatus>(0xC000'0002U);
constexpr NtStatus kStatusInvalidParameter = std::bit_cast<NtStatus>(0xC000'000DU);
constexpr NtStatus kStatusNoMemory = std::bit_cast<NtStatus>(0xC000'0017U);

// Logs an exception that reached a shim boundary.
void ReportShimException(std::string_view shimName, std::string_view what) noexcept;
void ReportShimException(std::string_view shimName, const std::exception& exception) noexcept;

// Records a call into a shim that only partly implements its API.
void ReportPartialShim(StubKind kind, std::string_view module, std::string_view symbol, std::string_view note) noexcept;

// Runs a shim body so no exception escapes to title code: returns `onOutOfMemory`
// for std::bad_alloc, `onError` for anything else thrown, and logs either.
template <typename Result, typename Body>
[[nodiscard]] Result GuardShim(std::string_view shimName, Result onError, Result onOutOfMemory, Body&& body) noexcept
{
    try
    {
        return std::forward<Body>(body)();
    }
    catch (const std::bad_alloc&)
    {
        ReportShimException(shimName, "out of memory");
        return onOutOfMemory;
    }
    catch (const std::exception& exception)
    {
        ReportShimException(shimName, exception);
        return onError;
    }
    catch (...)
    {
        ReportShimException(shimName, "unknown exception");
        return onError;
    }
}

template <typename Body>
[[nodiscard]] HResult GuardHResult(std::string_view shimName, Body&& body) noexcept
{
    return GuardShim<HResult>(shimName, kHResultFail, kHResultOutOfMemory, std::forward<Body>(body));
}

template <typename Body>
[[nodiscard]] NtStatus GuardNtStatus(std::string_view shimName, Body&& body) noexcept
{
    return GuardShim<NtStatus>(shimName, kStatusUnsuccessful, kStatusNoMemory, std::forward<Body>(body));
}
} // namespace OpenEra::Kernel
