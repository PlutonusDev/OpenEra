#include "Assert.h"

#include <cstdio>
#include <cstdlib>
#include <exception>

#include <Windows.h>

#include "Core/Logger.h"

namespace OpenEra::Core::Detail
{
void AssertionFailed(std::string_view expression, const std::source_location& location,
                     std::string_view message) noexcept
{
    try
    {
        const std::string line =
            std::format("Assertion failed: {}{}{} at {}:{} in {}", expression, message.empty() ? "" : ": ", message,
                        location.file_name(), location.line(), location.function_name());
        DefaultLogger().Log(LogLevel::Error, line);
        // Also straight to stderr, in case the logger has no sinks yet.
        std::fputs((line + "\n").c_str(), stderr);
        std::fflush(stderr);
    }
    catch (const std::exception&)
    {
        std::fputs("Assertion failed\n", stderr);
    }
    if (IsDebuggerPresent() != FALSE)
    {
        __debugbreak();
    }
    // Ends at once, without the C runtime's abort dialog or exit handlers.
    std::_Exit(kAssertionExitCode);
}
} // namespace OpenEra::Core::Detail
