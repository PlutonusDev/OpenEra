#include "DebugOutputShims.h"

#include <exception>
#include <string>
#include <string_view>

#include <Windows.h>

#include "Core/Logger.h"
#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"

namespace OpenEra::Kernel
{
namespace
{
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

void LogDebugOutput(std::string_view text) noexcept
{
    try
    {
        if (const auto line = DebugOutputLine(text); !line.empty())
        {
            Core::DefaultLogger().Log(Core::LogLevel::Info, line);
        }
    }
    catch (const std::exception& exception)
    {
        ReportShimException("OutputDebugString", exception);
    }
}

void WINAPI OutputDebugStringWShim(LPCWSTR text) noexcept
{
    if (text == nullptr)
    {
        return;
    }
    try
    {
        LogDebugOutput(Narrow(text));
    }
    catch (const std::exception& exception)
    {
        ReportShimException("OutputDebugStringW", exception);
    }
    OutputDebugStringW(text);
}

void WINAPI OutputDebugStringAShim(LPCSTR text) noexcept
{
    if (text == nullptr)
    {
        return;
    }
    LogDebugOutput(text);
    OutputDebugStringA(text);
}
} // namespace

std::string DebugOutputLine(std::string_view text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    if (text.empty())
    {
        return {};
    }
    return "Title: " + std::string(text);
}

bool AddDebugOutputExports(ShimModule& kernel32)
{
    return kernel32.Export("OutputDebugStringW", ToShimProc(&OutputDebugStringWShim)) &&
           kernel32.Export("OutputDebugStringA", ToShimProc(&OutputDebugStringAShim));
}
} // namespace OpenEra::Kernel
