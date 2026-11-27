#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "App/Settings.h"
#include "Core/Config.h"
#include "Support/TempFolder.h"

using namespace OpenEra::Core;
using OpenEra::Tests::TempFolder;

namespace
{
std::vector<SettingDefinition> TestSchema()
{
    return {
        {.Key = "log.level",
         .Type = SettingType::String,
         .Default = std::string("info"),
         .Description = "level",
         .Choices = {"trace", "info", "warn", "error"}},
        {.Key = "log.file",
         .Type = SettingType::String,
         .Default = std::string(),
         .Description = "file",
         .Choices = {}},
        {.Key = "log.window", .Type = SettingType::Bool, .Default = false, .Description = "window", .Choices = {}},
        {.Key = "graphics.scale", .Type = SettingType::Float, .Default = 1.0, .Description = "scale", .Choices = {}},
        {.Key = "graphics.width",
         .Type = SettingType::Integer,
         .Default = int64_t{1920},
         .Description = "width",
         .Choices = {}},
        {.Key = "user.name",
         .Type = SettingType::String,
         .Default = std::string("Player"),
         .Description = "name",
         .Choices = {}},
    };
}

bool Mentions(const std::vector<ConfigIssue>& issues, std::string_view text)
{
    return std::ranges::any_of(issues, [text](const ConfigIssue& issue)
                               { return issue.Message.find(text) != std::string::npos; });
}
} // namespace

TEST_CASE("Settings start at their defaults", "[config]")
{
    const Config config(TestSchema());
    CHECK(config.GetString("log.level") == "info");
    CHECK(config.GetString("log.file").empty());
    CHECK_FALSE(config.GetBool("log.window"));
    CHECK(config.GetFloat("graphics.scale") == 1.0);
    CHECK(config.GetInteger("graphics.width") == 1920);
    CHECK(config.SourceOf("log.level") == "default");
    CHECK(config.FormatValue("user.name") == "\"Player\"");
}

TEST_CASE("Files set values with tables, dotted and quoted keys", "[config]")
{
    Config config(TestSchema());
    const auto issues = config.LoadText("\xEF\xBB\xBF# OpenEra settings\n"
                                        "[log]\n"
                                        "level = \"WARN\"   # choices ignore case\n"
                                        "window = true\n"
                                        "file = 'C:\\logs\\openera.log'\n"
                                        "\r\n"
                                        "[graphics]\n"
                                        "scale = 1.5e0\n"
                                        "width = 2_560\n"
                                        "[\"user\"]\n"
                                        "name = \"J\\u00f6rg \\\"J\\\"\"\n",
                                        "openera.toml");
    CHECK(issues.empty());
    CHECK(config.GetString("log.level") == "warn");
    CHECK(config.GetBool("log.window"));
    CHECK(config.GetString("log.file") == "C:\\logs\\openera.log");
    CHECK(config.GetFloat("graphics.scale") == 1.5);
    CHECK(config.GetInteger("graphics.width") == 2560);
    CHECK(config.GetString("user.name") == "J\xC3\xB6rg \"J\"");
    CHECK(config.SourceOf("graphics.width") == "openera.toml");

    // Dotted keys at the top level, and an integer for a float setting.
    Config dotted(TestSchema());
    CHECK(dotted.LoadText("graphics.scale = 2\nlog . level = 'trace'\n", "dotted.toml").empty());
    CHECK(dotted.GetFloat("graphics.scale") == 2.0);
    CHECK(dotted.GetString("log.level") == "trace");
}

TEST_CASE("Later layers win, and overrides take plain words", "[config]")
{
    Config config(TestSchema());
    CHECK(config.LoadText("[log]\nlevel = \"warn\"\n", "user.toml").empty());
    CHECK(config.LoadText("[log]\nlevel = \"error\"\nwindow = true\n", "title.toml").empty());
    CHECK(config.GetString("log.level") == "error");
    CHECK(config.ApplyOverride("log.level=trace").empty());
    CHECK(config.ApplyOverride("graphics.width = 1280").empty());
    CHECK(config.ApplyOverride("user.name=\"Two words\"").empty());
    CHECK(config.ApplyOverride("user.name=42").size() == 1);                // A number isn't a string...
    CHECK(config.ApplyOverride("log.file=C:\\some path\\log.txt").empty()); // ...but a plain word is.
    CHECK(config.GetString("log.level") == "trace");
    CHECK(config.SourceOf("log.level") == "command line");
    CHECK(config.GetBool("log.window"));
    CHECK(config.SourceOf("log.window") == "title.toml");
    CHECK(config.GetInteger("graphics.width") == 1280);
    CHECK(config.GetString("user.name") == "Two words");
    CHECK(config.GetString("log.file") == "C:\\some path\\log.txt");
}

TEST_CASE("Invalid settings are reported with lines and never thrown", "[config]")
{
    Config config(TestSchema());
    const auto issues = config.LoadText("[log]\n"
                                        "level = \"loud\"\n" // 2: not a choice
                                        "window = \"yes\"\n" // 3: wrong type
                                        "colour = true\n"    // 4: unknown
                                        "file = \"a\"\n"
                                        "file = \"b\"\n" // 6: set twice
                                        "[graphics]\n"
                                        "width = 99999999999999999999\n" // 8: out of range
                                        "scale = [1, 2]\n"               // 9: array
                                        "width2 = 1979-05-27\n"          // 10: date
                                        "this line is wrong\n"           // 11
                                        "[[tables]]\n"                   // 12
                                        "multi = \"\"\"multi\"\"\"\n"    // 13
                                        "hex = 0x10\n"                   // 14
                                        "dot = 1.\n"                     // 15
                                        "two = 1 2\n"                    // 16
                                        "open = 'open\n",                // 17
                                        "bad.toml");
    CHECK(Mentions(issues, "allowed: trace, info, warn, error"));
    CHECK(Mentions(issues, "must be a boolean"));
    CHECK(Mentions(issues, "unknown setting 'log.colour'"));
    CHECK(Mentions(issues, "set twice"));
    CHECK(Mentions(issues, "out of range"));
    CHECK(Mentions(issues, "arrays are not supported"));
    CHECK(Mentions(issues, "dates and times are not supported"));
    CHECK(Mentions(issues, "expected key = value"));
    CHECK(Mentions(issues, "arrays of tables are not supported"));
    CHECK(Mentions(issues, "multi-line strings are not supported"));
    CHECK(Mentions(issues, "hexadecimal"));
    CHECK(Mentions(issues, "not a valid float"));
    CHECK(Mentions(issues, "unexpected text after the value"));
    CHECK(Mentions(issues, "string is not closed"));

    std::set<size_t> lines;
    for (const auto& issue : issues)
    {
        CHECK(issue.Source == "bad.toml");
        lines.insert(issue.Line);
    }
    CHECK(lines == std::set<size_t>{2, 3, 4, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17});
    CHECK(ToString(issues.front()).starts_with("bad.toml:2: "));

    // Bad values leave the earlier ones in place.
    CHECK(config.GetString("log.level") == "info");
    CHECK_FALSE(config.GetBool("log.window"));
    CHECK(config.GetString("log.file") == "a");
    CHECK(config.GetInteger("graphics.width") == 1920);

    CHECK(config.ApplyOverride("no equals sign").size() == 1);
    CHECK(config.ApplyOverride("graphics.width=wide").size() == 1);
}

TEST_CASE("Settings files load from disk, and a missing one is fine", "[config]")
{
    const TempFolder folder("Config");
    folder.Write("openera.toml", "[log]\nlevel = \"error\"\n");
    Config config(TestSchema());
    CHECK(config.LoadFile(folder.Path() / "missing.toml").empty());
    CHECK(config.LoadFile(folder.Path() / "openera.toml").empty());
    CHECK(config.GetString("log.level") == "error");
    CHECK(config.SourceOf("log.level").ends_with("openera.toml"));
}

TEST_CASE("Values format back the way files write them", "[config]")
{
    CHECK(FormatSettingValue(true) == "true");
    CHECK(FormatSettingValue(int64_t{-42}) == "-42");
    CHECK(FormatSettingValue(2.0) == "2.0");
    CHECK(FormatSettingValue(0.25) == "0.25");
    CHECK(FormatSettingValue(std::string("a \"b\" \\c")) == "\"a \\\"b\\\" \\\\c\"");

    // And read back.
    Config config(TestSchema());
    CHECK(config.ApplyOverride("user.name=" + FormatSettingValue(std::string("a \"b\" \\c"))).empty());
    CHECK(config.GetString("user.name") == "a \"b\" \\c");
}

TEST_CASE("The launcher's settings are a valid schema", "[config]")
{
    const auto schema = OpenEra::App::LauncherSettings();
    std::set<std::string> keys;
    for (const auto& setting : schema)
    {
        INFO(setting.Key);
        CHECK(keys.insert(setting.Key).second);
        CHECK_FALSE(setting.Description.empty());
        if (!setting.Choices.empty())
        {
            const auto* text = std::get_if<std::string>(&setting.Default);
            REQUIRE(text != nullptr);
            CHECK(std::ranges::find(setting.Choices, *text) != setting.Choices.end());
        }
    }
    // Every default survives a round trip through its own formatting.
    Config config(schema);
    for (const auto& setting : schema)
    {
        INFO(setting.Key);
        CHECK(config.ApplyOverride(setting.Key + "=" + FormatSettingValue(setting.Default)).empty());
    }
    CHECK(OpenEra::App::UserConfigPath().filename() == "openera.toml");
    CHECK(OpenEra::App::TitleDataDirectory("Contoso.Game: 2").filename() == "Contoso.Game__2");
    CHECK(OpenEra::App::TitleDataDirectory("..").filename() == "Title");
}
