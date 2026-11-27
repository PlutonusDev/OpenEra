#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace OpenEra::Kernel
{
class ShimModule;

// What the process-image shims report while a title runs: its executable
// instead of openera.exe, and its own command line.
struct TitleProcessIdentity
{
    std::filesystem::path Image; // Absolute host path of the title's executable.
    std::wstring CommandLine;
};

// Set before the title starts, cleared (nullopt) after it ends.
void SetTitleProcess(std::optional<TitleProcessIdentity> identity);

// One argument quoted so CommandLineToArgvW and the C runtime read it back
// unchanged (Microsoft Learn: "Parsing C command-line arguments").
[[nodiscard]] std::wstring QuoteArgument(std::wstring_view argument);

// "<image>" followed by the quoted arguments.
[[nodiscard]] std::wstring BuildCommandLine(const std::filesystem::path& image,
                                            std::span<const std::wstring> arguments);

// `text` copied into a caller's buffer the way GetModuleFileName does: at most
// buffer.size() - 1 characters and a terminator. Truncated means it didn't fit.
struct CopiedText
{
    uint32_t Length = 0; // Characters written, without the terminator.
    bool Truncated = false;
};

[[nodiscard]] CopiedText CopyToBuffer(std::wstring_view text, std::span<wchar_t> buffer) noexcept;
[[nodiscard]] CopiedText CopyToBuffer(std::string_view text, std::span<char> buffer) noexcept;

// Adds GetCommandLineW / GetCommandLineA and QueryFullProcessImageNameW /
// QueryFullProcessImageNameA (for the current process) to a kernel32 shim
// module. With no title identity set, they pass through to the host.
bool AddProcessExports(ShimModule& kernel32);
} // namespace OpenEra::Kernel
