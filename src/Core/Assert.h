#pragma once

#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace OpenEra::Core
{
#ifdef NDEBUG
inline constexpr bool kAssertionsEnabled = false;
#else
inline constexpr bool kAssertionsEnabled = true;
#endif

// Exit code of a process stopped by a failed assertion.
inline constexpr int kAssertionExitCode = 70;

namespace Detail
{
// Logs the failure (DefaultLogger and stderr), breaks into an attached
// debugger, then ends the process with kAssertionExitCode.
[[noreturn]] void AssertionFailed(std::string_view expression, const std::source_location& location,
                                  std::string_view message = {}) noexcept;

template <typename... Args>
[[nodiscard]] std::string AssertionMessage(std::format_string<Args...> format, Args&&... args)
{
    try
    {
        return std::format(format, std::forward<Args>(args)...);
    }
    catch (const std::format_error&)
    {
        return "(message could not be formatted)";
    }
}
} // namespace Detail
} // namespace OpenEra::Core

// Checks an internal invariant in Debug builds; compiled out (never evaluated)
// in Release. Optional std::format message: OPENERA_ASSERT(size > 0, "size {}", size).
#define OPENERA_ASSERT(expression, ...)                                                                     \
    do                                                                                                      \
    {                                                                                                       \
        if constexpr (::OpenEra::Core::kAssertionsEnabled)                                                  \
        {                                                                                                   \
            if (!(expression))                                                                              \
            {                                                                                               \
                ::OpenEra::Core::Detail::AssertionFailed(                                                   \
                    #expression, std::source_location::current()                                            \
                                     __VA_OPT__(, ::OpenEra::Core::Detail::AssertionMessage(__VA_ARGS__))); \
            }                                                                                               \
        }                                                                                                   \
    } while (false)

// Like OPENERA_ASSERT, but `expression` is always evaluated, for calls whose
// side effects matter. Only the check is compiled out in Release.
#define OPENERA_VERIFY(expression, ...)                                                                     \
    do                                                                                                      \
    {                                                                                                       \
        const bool openEraVerified = static_cast<bool>(expression);                                         \
        if constexpr (::OpenEra::Core::kAssertionsEnabled)                                                  \
        {                                                                                                   \
            if (!openEraVerified)                                                                           \
            {                                                                                               \
                ::OpenEra::Core::Detail::AssertionFailed(                                                   \
                    #expression, std::source_location::current()                                            \
                                     __VA_OPT__(, ::OpenEra::Core::Detail::AssertionMessage(__VA_ARGS__))); \
            }                                                                                               \
        }                                                                                                   \
        else                                                                                                \
        {                                                                                                   \
            static_cast<void>(openEraVerified);                                                             \
        }                                                                                                   \
    } while (false)
