#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace OpenEra::Core
{
enum class SettingType : uint8_t
{
    Bool,
    Integer,
    Float,
    String,
};

[[nodiscard]] std::string_view ToString(SettingType type) noexcept;

using SettingValue = std::variant<bool, int64_t, double, std::string>;

// One setting, declared in code: its key ("section.name"), type, default,
// a description for `openera config --list`, and for strings an optional
// list of allowed values (compared case-insensitively).
struct SettingDefinition
{
    std::string Key;
    SettingType Type = SettingType::String;
    SettingValue Default;
    std::string Description;
    std::vector<std::string> Choices;
};

// A problem found while loading settings. Loading never throws: bad values
// are reported and the previous value is kept.
struct ConfigIssue
{
    std::string Source; // File path, or "command line".
    size_t Line = 0;    // 1-based; 0 when there's no line.
    std::string Message;
};

[[nodiscard]] std::string ToString(const ConfigIssue& issue);

// Typed settings in layers: defaults, then each file or override in the
// order it's applied, later layers winning.
//
// Files use a subset of TOML v1.0.0: [table] headers, key = value with bare,
// quoted or dotted keys, basic and literal strings, integers (with _
// separators), floats, booleans and # comments. Arrays, inline tables,
// multi-line strings and dates are reported as unsupported.
class Config
{
public:
    explicit Config(std::vector<SettingDefinition> schema);

    [[nodiscard]] const std::vector<SettingDefinition>& Schema() const noexcept { return m_schema; }

    std::vector<ConfigIssue> LoadText(std::string_view text, std::string_view source);
    // A missing file is fine and loads nothing.
    std::vector<ConfigIssue> LoadFile(const std::filesystem::path& path);
    // "key=value", the value written as in a file; a value that isn't valid
    // TOML is taken as a plain string, so --set log.level=warn works.
    std::vector<ConfigIssue> ApplyOverride(std::string_view assignment, std::string_view source = "command line");

    // Keys must be in the schema, with the matching type.
    [[nodiscard]] bool GetBool(std::string_view key) const;
    [[nodiscard]] int64_t GetInteger(std::string_view key) const;
    [[nodiscard]] double GetFloat(std::string_view key) const;
    [[nodiscard]] const std::string& GetString(std::string_view key) const;

    // The value as it would be written in a file, and where it came from.
    [[nodiscard]] std::string FormatValue(std::string_view key) const;
    [[nodiscard]] const std::string& SourceOf(std::string_view key) const;

private:
    struct Entry
    {
        SettingValue Value;
        std::string Source = "default";
    };

    [[nodiscard]] const SettingDefinition* Find(std::string_view key) const noexcept;
    [[nodiscard]] const Entry& EntryFor(std::string_view key) const;
    void Set(std::string_view key, SettingValue value, std::string_view source, size_t line,
             std::vector<ConfigIssue>& issues);

    std::vector<SettingDefinition> m_schema;
    std::map<std::string, Entry, std::less<>> m_entries;
};

[[nodiscard]] std::string FormatSettingValue(const SettingValue& value);
} // namespace OpenEra::Core
