#include <cstddef>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "App/LogWindow.h"
#include "Core/Logger.h"

using OpenEra::App::LogWindowSink;
using OpenEra::Core::Logger;

TEST_CASE("The log window shows records, trims old lines and closes", "[logwindow]")
{
    auto window = std::make_shared<LogWindowSink>(L"OpenEra test log");
    REQUIRE(window->IsOpen());
    Logger logger;
    logger.SetMinimumLevel(OpenEra::Core::LogLevel::Trace);
    logger.AddSink(window);

    logger.Trace("trace {}", 1);
    logger.Info("info");
    logger.Warn("warn");
    logger.Error("error");
    CHECK(window->LineCount() == 4);

    for (size_t index = 0; index < LogWindowSink::kMaxLines + 50; ++index)
    {
        logger.Info("line {}", index);
    }
    CHECK(window->LineCount() == LogWindowSink::kMaxLines);

    window->Close();
    window->WaitUntilClosed();
    CHECK_FALSE(window->IsOpen());
    CHECK(window->LineCount() == 0);
    logger.Info("after close"); // Dropped, not queued.
}
