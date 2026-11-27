// DebuggerSink lives apart from Logger.cpp so <Windows.h> stays out of the rest of Core.
#include <string>

#include <Windows.h>

#include "Core/Logger.h"

namespace OpenEra::Core
{
namespace
{
std::wstring Widen(const std::string& text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (length <= 1)
    {
        return {};
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), length);
    result.pop_back(); // Terminator.
    return result;
}
} // namespace

void DebuggerSink::Write(const LogRecord& record)
{
    if (IsDebuggerPresent() == FALSE)
    {
        return;
    }
    const std::wstring line = Widen(FormatLogLine(record)) + L"\n";
    OutputDebugStringW(line.c_str());
}
} // namespace OpenEra::Core
