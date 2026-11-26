#pragma once

#include <string>
#include <string_view>

namespace OpenEra::Kernel
{
class ShimModule;

// A title's debug output as one log line: "Title: <text>" without the trailing
// line break titles usually add. Empty if there's nothing to log.
[[nodiscard]] std::string DebugOutputLine(std::string_view text);

// Adds OutputDebugStringW / OutputDebugStringA to a kernel32 shim module. The
// text goes to OpenEra's log (Core::DefaultLogger) and on to the host, so an
// attached debugger still sees it.
bool AddDebugOutputExports(ShimModule& kernel32);
} // namespace OpenEra::Kernel
