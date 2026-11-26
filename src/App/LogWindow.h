#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "Core/Logger.h"

namespace OpenEra::App
{
// A console-style window that shows log records as they arrive: dark
// background, monospace text coloured by level, scrolling to follow new
// lines unless you've scrolled up. Ctrl+C copies the selected lines, Ctrl+A
// selects everything. The window runs on its own thread; Write only queues.
class LogWindowSink final : public Core::LogSink // NOLINT(cppcoreguidelines-special-member-functions)
{
public:
    // Lines kept; older ones are dropped.
    static constexpr size_t kMaxLines = 20'000;

    // Opens a window with this title. Throws std::system_error if it can't be created.
    explicit LogWindowSink(std::wstring_view title);
    // Closes the window if it's still open.
    ~LogWindowSink() override;

    void Write(const Core::LogRecord& record) override;

    [[nodiscard]] bool IsOpen() const noexcept;
    // Blocks until the user closes the window.
    void WaitUntilClosed() const;
    void Close() noexcept;

    // Lines the window holds, once everything queued has been shown (for tests).
    [[nodiscard]] size_t LineCount() const noexcept;

private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace OpenEra::App
