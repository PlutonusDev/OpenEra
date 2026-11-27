#include "ProcessShims.h"

#include <algorithm>
#include <array>
#include <bit>
#include <mutex>
#include <type_traits>
#include <utility>

#include <Windows.h>

#include "Kernel/ShimGuard.h"
#include "Kernel/ShimModule.h"

namespace OpenEra::Kernel
{
namespace
{
// The identity and the strings handed out for it. Pointers returned by
// GetCommandLine stay valid until the identity changes.
struct IdentityState
{
    std::mutex Mutex;
    bool Active = false;
    std::wstring Image;
    std::string ImageAnsi;
    std::wstring CommandLine;
    std::string CommandLineAnsi;
};

IdentityState& Identity() noexcept
{
    static IdentityState state;
    return state;
}

std::string ToAnsi(const std::wstring& text)
{
    const int length = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
    {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, result.data(), length, nullptr, nullptr);
    result.pop_back(); // Terminator.
    return result;
}

template <typename Char>
CopiedText CopyText(std::basic_string_view<Char> text, std::span<Char> buffer) noexcept
{
    if (buffer.empty())
    {
        return {.Length = 0, .Truncated = true};
    }
    const size_t length = std::min(text.size(), buffer.size() - 1);
    std::ranges::copy(text.substr(0, length), buffer.begin());
    buffer.subspan(length).front() = Char{};
    const DWORD copied = std::bit_cast<std::array<DWORD, 2>>(length).front();
    return {.Length = copied, .Truncated = length < text.size()};
}

bool IsCurrentProcess(HANDLE process) noexcept
{
    return process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId();
}

LPWSTR WINAPI GetCommandLineWShim() noexcept
{
    return GuardShim<LPWSTR>("GetCommandLineW", GetCommandLineW(), GetCommandLineW(),
                             []() -> LPWSTR
                             {
                                 auto& state = Identity();
                                 const std::scoped_lock lock(state.Mutex);
                                 return state.Active ? state.CommandLine.data() : GetCommandLineW();
                             });
}

LPSTR WINAPI GetCommandLineAShim() noexcept
{
    return GuardShim<LPSTR>("GetCommandLineA", GetCommandLineA(), GetCommandLineA(),
                            []() -> LPSTR
                            {
                                auto& state = Identity();
                                const std::scoped_lock lock(state.Mutex);
                                return state.Active ? state.CommandLineAnsi.data() : GetCommandLineA();
                            });
}

template <typename Char, typename Real>
BOOL QueryImageName(HANDLE process, DWORD flags, Char* name, PDWORD size, Real real)
{
    if (flags != 0 || name == nullptr || size == nullptr || !IsCurrentProcess(process))
    {
        return real(process, flags, name, size); // The native (\Device\...) form isn't redirected.
    }
    auto& state = Identity();
    const std::scoped_lock lock(state.Mutex);
    if (!state.Active)
    {
        return real(process, flags, name, size);
    }
    std::basic_string_view<Char> image;
    if constexpr (std::is_same_v<Char, wchar_t>)
    {
        image = state.Image;
    }
    else
    {
        image = state.ImageAnsi;
    }
    if (image.size() >= *size)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    const auto copied = CopyText(image, std::span<Char>(name, *size));
    *size = copied.Length;
    return TRUE;
}

BOOL WINAPI QueryFullProcessImageNameWShim(HANDLE process, DWORD flags, LPWSTR name, PDWORD size) noexcept
{
    return GuardShim<BOOL>("QueryFullProcessImageNameW", FALSE, FALSE,
                           [=] { return QueryImageName(process, flags, name, size, &QueryFullProcessImageNameW); });
}

BOOL WINAPI QueryFullProcessImageNameAShim(HANDLE process, DWORD flags, LPSTR name, PDWORD size) noexcept
{
    return GuardShim<BOOL>("QueryFullProcessImageNameA", FALSE, FALSE,
                           [=] { return QueryImageName(process, flags, name, size, &QueryFullProcessImageNameA); });
}
} // namespace

void SetTitleProcess(std::optional<TitleProcessIdentity> identity)
{
    auto& state = Identity();
    const std::scoped_lock lock(state.Mutex);
    state.Active = identity.has_value();
    if (!identity)
    {
        return;
    }
    state.Image = identity->Image.wstring();
    state.ImageAnsi = ToAnsi(state.Image);
    state.CommandLine = std::move(identity->CommandLine);
    state.CommandLineAnsi = ToAnsi(state.CommandLine);
}

std::wstring QuoteArgument(std::wstring_view argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
    {
        return std::wstring(argument);
    }
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        // Backslashes only escape when a quote follows them.
        quoted.append(character == L'"' ? (backslashes * 2) + 1 : backslashes, L'\\');
        backslashes = 0;
        quoted += character;
    }
    quoted.append(backslashes * 2, L'\\'); // Before the closing quote.
    return quoted + L"\"";
}

std::wstring BuildCommandLine(const std::filesystem::path& image, std::span<const std::wstring> arguments)
{
    // The program name ends at the next quote; it can't contain one, so it's never escaped.
    std::wstring commandLine = L"\"" + image.wstring() + L"\"";
    for (const auto& argument : arguments)
    {
        commandLine += L" " + QuoteArgument(argument);
    }
    return commandLine;
}

CopiedText CopyToBuffer(std::wstring_view text, std::span<wchar_t> buffer) noexcept
{
    return CopyText(text, buffer);
}

CopiedText CopyToBuffer(std::string_view text, std::span<char> buffer) noexcept
{
    return CopyText(text, buffer);
}

bool AddProcessExports(ShimModule& kernel32)
{
    return kernel32.Export("GetCommandLineW", ToShimProc(&GetCommandLineWShim)) &&
           kernel32.Export("GetCommandLineA", ToShimProc(&GetCommandLineAShim)) &&
           kernel32.Export("QueryFullProcessImageNameW", ToShimProc(&QueryFullProcessImageNameWShim)) &&
           kernel32.Export("QueryFullProcessImageNameA", ToShimProc(&QueryFullProcessImageNameAShim));
}
} // namespace OpenEra::Kernel
