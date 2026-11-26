#include "LogWindow.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <Windows.h>

namespace OpenEra::App
{
namespace
{
using Core::LogLevel;

constexpr const wchar_t* kClassName = L"OpenEraLogWindow";
constexpr UINT_PTR kDrainTimer = 1;
constexpr UINT kDrainIntervalMs = 50;
constexpr UINT kDrainNow = WM_APP + 1;
constexpr intptr_t kListId = 1;
constexpr int kFontPoints = 10;
constexpr int kTextMargin = 6;
constexpr int kInitialWidth = 1000;
constexpr int kInitialHeight = 560;

constexpr COLORREF Rgb(uint32_t red, uint32_t green, uint32_t blue) noexcept
{
    return red | (green << 8) | (blue << 16);
}

// A dark console palette.
constexpr COLORREF kBackground = Rgb(12, 12, 12);
constexpr COLORREF kSelection = Rgb(38, 79, 120);

constexpr COLORREF ColourFor(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Trace:
        return Rgb(118, 118, 118);
    case LogLevel::Info:
        return Rgb(204, 204, 204);
    case LogLevel::Warn:
        return Rgb(249, 241, 165);
    case LogLevel::Error:
        return Rgb(231, 72, 86);
    }
    return Rgb(204, 204, 204);
}

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

uint16_t LowWord(WPARAM value) noexcept
{
    return std::bit_cast<std::array<uint16_t, sizeof(WPARAM) / sizeof(uint16_t)>>(value).front();
}

struct Line
{
    LogLevel Level = LogLevel::Info;
    std::wstring Text;
};
} // namespace

struct LogWindowSink::State
{
    // Shared with writers.
    std::mutex PendingMutex;
    std::deque<Line> Pending;
    std::atomic<HWND> Window{nullptr};
    std::atomic<bool> Open{false};

    mutable std::mutex ClosedMutex;
    mutable std::condition_variable ClosedChanged;
    bool Closed = false; // Guarded by ClosedMutex.

    std::wstring Title;

    // UI thread only.
    std::deque<Line> Lines;
    HWND List = nullptr;
    HFONT Font = nullptr;
    HBRUSH Background = nullptr;
    HBRUSH Highlight = nullptr;
    int LineHeight = 16;

    // Last, so it joins (on destruction) before anything the window thread uses is destroyed.
    std::jthread Thread;

    // The window thread: creates the window, reports whether that worked, then
    // pumps messages until it's closed.
    void Run(std::promise<bool>& ready) noexcept
    {
        bool reported = false;
        try
        {
            auto* const window = Create();
            Window = window;
            Open = window != nullptr;
            reported = true;
            ready.set_value(window != nullptr);
            if (window != nullptr)
            {
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0)
                {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            Release();
            Window = nullptr;
            Open = false;
            {
                const std::scoped_lock lock(ClosedMutex);
                Closed = true;
            }
            ClosedChanged.notify_all();
        }
        catch (...)
        {
            Window = nullptr;
            Open = false;
            if (!reported)
            {
                ReportFailure(ready);
            }
        }
    }

    static void ReportFailure(std::promise<bool>& ready) noexcept
    {
        try
        {
            ready.set_value(false);
        }
        catch (const std::future_error&)
        {
            return; // Already reported.
        }
    }

    [[gsl::suppress("type.4")]] HWND Create() noexcept
    {
        auto* const instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(WNDCLASSEXW);
        windowClass.lpfnWndProc = &State::WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return nullptr;
        }
        return CreateWindowExW(0, kClassName, Title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                               CW_USEDEFAULT, kInitialWidth, kInitialHeight, nullptr, nullptr, instance, this);
    }

    void Release() noexcept
    {
        const auto remove = [](HGDIOBJ object) noexcept
        {
            if (object != nullptr)
            {
                DeleteObject(object);
            }
        };
        remove(Font);
        remove(Background);
        remove(Highlight);
        Font = nullptr;
        Background = nullptr;
        Highlight = nullptr;
    }

    bool OnCreate(HWND window) noexcept
    {
        const int dpi = std::bit_cast<int>(GetDpiForWindow(window));
        Font = CreateFontW(-MulDiv(kFontPoints, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                           L"Consolas");
        Background = CreateSolidBrush(kBackground);
        Highlight = CreateSolidBrush(kSelection);
        if (auto* const dc = GetDC(window))
        {
            auto* const previous = SelectObject(dc, Font);
            TEXTMETRICW metrics{};
            if (GetTextMetricsW(dc, &metrics) != FALSE)
            {
                LineHeight = metrics.tmHeight + 2;
            }
            SelectObject(dc, previous);
            ReleaseDC(window, dc);
        }
        constexpr DWORD kListStyle = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_OWNERDRAWFIXED | LBS_NODATA |
                                     LBS_NOINTEGRALHEIGHT | LBS_EXTENDEDSEL | LBS_WANTKEYBOARDINPUT;
        List = CreateWindowExW(0, L"LISTBOX", nullptr, kListStyle, 0, 0, 0, 0, window, std::bit_cast<HMENU>(kListId),
                               GetModuleHandleW(nullptr), nullptr);
        SetTimer(window, kDrainTimer, kDrainIntervalMs, nullptr);
        return List != nullptr && Font != nullptr && Background != nullptr && Highlight != nullptr;
    }

    // Moves queued lines into the list, following the end if the view was at it.
    size_t Drain()
    {
        std::deque<Line> incoming;
        {
            const std::scoped_lock lock(PendingMutex);
            incoming.swap(Pending);
        }
        if (incoming.empty() || List == nullptr)
        {
            return Lines.size();
        }

        const size_t count = Lines.size();
        const auto top = std::bit_cast<size_t>(SendMessageW(List, LB_GETTOPINDEX, 0, 0));
        RECT client{};
        GetClientRect(List, &client);
        const LONG lineHeight = std::max(1, LineHeight);
        const LONG visibleRows = std::max<LONG>(1, (client.bottom - client.top) / lineHeight);
        const size_t visible = std::bit_cast<unsigned long>(visibleRows);
        const bool follow = count == 0 || top + visible >= count;

        for (auto& line : incoming)
        {
            Lines.push_back(std::move(line));
        }
        size_t dropped = 0;
        while (Lines.size() > kMaxLines)
        {
            Lines.pop_front();
            ++dropped;
        }

        SendMessageW(List, WM_SETREDRAW, FALSE, 0);
        SendMessageW(List, LB_SETCOUNT, Lines.size(), 0);
        const size_t newTop = follow ? Lines.size() - std::min(Lines.size(), visible) : top - std::min(top, dropped);
        SendMessageW(List, LB_SETTOPINDEX, newTop, 0);
        SendMessageW(List, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(List, nullptr, FALSE);
        return Lines.size();
    }

    void DrawLine(const DRAWITEMSTRUCT& item) const
    {
        auto* const dc = item.hDC;
        RECT bounds = item.rcItem;
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        FillRect(dc, &bounds, selected ? Highlight : Background);
        if (item.itemID >= Lines.size())
        {
            return;
        }
        const Line& line = Lines.at(item.itemID);
        auto* const previous = SelectObject(dc, Font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ColourFor(line.Level));
        bounds.left += kTextMargin;
        DrawTextW(dc, line.Text.c_str(), -1, &bounds, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_EXPANDTABS);
        SelectObject(dc, previous);
    }

    void SelectAll() const noexcept
    {
        if (!Lines.empty())
        {
            SendMessageW(List, LB_SELITEMRANGEEX, 0, std::bit_cast<LPARAM>(Lines.size() - 1));
        }
    }

    void CopySelection(HWND window) const
    {
        const LRESULT selectedCount = SendMessageW(List, LB_GETSELCOUNT, 0, 0);
        if (selectedCount <= 0)
        {
            return;
        }
        std::vector<int> indices(std::bit_cast<size_t>(selectedCount));
        SendMessageW(List, LB_GETSELITEMS, indices.size(), std::bit_cast<LPARAM>(indices.data()));
        std::wstring text;
        for (const int index : indices)
        {
            const size_t position = std::bit_cast<unsigned>(index);
            if (index >= 0 && position < Lines.size())
            {
                text += Lines.at(position).Text;
                text += L"\r\n";
            }
        }
        if (text.empty() || OpenClipboard(window) == FALSE)
        {
            return;
        }
        EmptyClipboard();
        HGLOBAL const memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        void* const locked = memory != nullptr ? GlobalLock(memory) : nullptr;
        if (locked != nullptr)
        {
            const std::span<wchar_t> buffer(static_cast<wchar_t*>(locked), text.size() + 1);
            std::ranges::copy(text, buffer.begin());
            buffer.back() = L'\0';
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr)
            {
                GlobalFree(memory);
            }
        }
        else if (memory != nullptr)
        {
            GlobalFree(memory);
        }
        CloseClipboard();
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (message == WM_NCCREATE)
        {
            const auto* create = std::bit_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, std::bit_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* const state = std::bit_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state == nullptr)
        {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        try
        {
            return state->Handle(window, message, wParam, lParam);
        }
        catch (...)
        {
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }

    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            return OnCreate(window) ? 0 : -1;
        case WM_SIZE:
        {
            RECT client{};
            GetClientRect(window, &client);
            MoveWindow(List, 0, 0, client.right - client.left, client.bottom - client.top, TRUE);
            return 0;
        }
        case WM_SETFOCUS:
            SetFocus(List);
            return 0;
        case WM_MEASUREITEM:
            std::bit_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = std::bit_cast<unsigned>(LineHeight);
            return TRUE;
        case WM_DRAWITEM:
            DrawLine(*std::bit_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_CTLCOLORLISTBOX:
            return std::bit_cast<LRESULT>(Background);
        case WM_ERASEBKGND:
            return TRUE;
        case WM_VKEYTOITEM:
            if (GetKeyState(VK_CONTROL) < 0)
            {
                const uint16_t key = LowWord(wParam);
                if (key == 'C')
                {
                    CopySelection(window);
                    return -2;
                }
                if (key == 'A')
                {
                    SelectAll();
                    return -2;
                }
            }
            return -1;
        case WM_TIMER:
            if (wParam == kDrainTimer)
            {
                std::ignore = Drain();
            }
            return 0;
        case kDrainNow:
            return std::bit_cast<LRESULT>(Drain());
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kDrainTimer);
            Open = false;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }
};

LogWindowSink::LogWindowSink(std::wstring_view title)
    : m_state(std::make_unique<State>())
{
    m_state->Title = title;
    std::promise<bool> ready;
    auto created = ready.get_future();
    m_state->Thread = std::jthread([state = m_state.get(), &ready]() noexcept { state->Run(ready); });
    if (!created.get())
    {
        m_state->Thread.join();
        throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
                                "cannot create the log window");
    }
}

// Copy and move are deleted by LogSink (redeclaring them here is C26456).
[[gsl::suppress("c.21")]] LogWindowSink::~LogWindowSink()
{
    Close(); // Destroying the state then joins the window thread.
}

void LogWindowSink::Write(const Core::LogRecord& record)
{
    if (!IsOpen())
    {
        return;
    }
    Line line{.Level = record.Level, .Text = Widen(Core::FormatLogLine(record))};
    const std::scoped_lock lock(m_state->PendingMutex);
    m_state->Pending.push_back(std::move(line));
    while (m_state->Pending.size() > kMaxLines)
    {
        m_state->Pending.pop_front();
    }
}

bool LogWindowSink::IsOpen() const noexcept
{
    return m_state->Open.load();
}

void LogWindowSink::WaitUntilClosed() const
{
    std::unique_lock lock(m_state->ClosedMutex);
    m_state->ClosedChanged.wait(lock, [this]() noexcept { return m_state->Closed; });
}

void LogWindowSink::Close() noexcept
{
    if (auto* const window = m_state->Window.load())
    {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
}

size_t LogWindowSink::LineCount() const noexcept
{
    auto* const window = m_state->Window.load();
    return window == nullptr ? 0 : std::bit_cast<size_t>(SendMessageW(window, kDrainNow, 0, 0));
}
} // namespace OpenEra::App
