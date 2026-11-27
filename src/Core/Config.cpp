#include "Config.h"

#include <algorithm>
#include <array>
#include <bit>
#include <exception>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include "Core/Assert.h"

namespace OpenEra::Core
{
namespace
{
constexpr std::string_view kSpace = " \t";

struct ParsedValue
{
    std::optional<SettingValue> Value;
    std::string Error;
};

[[nodiscard]] ParsedValue Fail(std::string message) noexcept
{
    return {.Value = std::nullopt, .Error = std::move(message)};
}

constexpr char AsciiLower(char value) noexcept
{
    constexpr std::string_view kUpper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    constexpr std::string_view kLower = "abcdefghijklmnopqrstuvwxyz";
    const auto index = kUpper.find(value);
    return index == std::string_view::npos ? value : kLower.at(index);
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right) noexcept
{
    return std::ranges::equal(left, right, [](char a, char b) noexcept { return AsciiLower(a) == AsciiLower(b); });
}

constexpr bool IsDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

constexpr bool IsBareKeyChar(char value) noexcept
{
    return IsDigit(value) || (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_' ||
           value == '-';
}

std::string_view TrimLeft(std::string_view text) noexcept
{
    const auto first = text.find_first_not_of(kSpace);
    return first == std::string_view::npos ? std::string_view{} : text.substr(first);
}

std::string_view Trim(std::string_view text) noexcept
{
    text = TrimLeft(text);
    const auto last = text.find_last_not_of(kSpace);
    return last == std::string_view::npos ? std::string_view{} : text.substr(0, last + 1);
}

// Only whitespace and an optional comment.
bool IsEmptyRest(std::string_view rest) noexcept
{
    rest = TrimLeft(rest);
    return rest.empty() || rest.front() == '#';
}

char ByteChar(uint32_t value) noexcept
{
    return std::bit_cast<char>(std::bit_cast<std::array<uint8_t, sizeof(uint32_t)>>(value).front());
}

bool AppendUtf8(std::string& out, uint32_t codePoint)
{
    if (codePoint > 0x10'FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
    {
        return false;
    }
    if (codePoint < 0x80)
    {
        out += ByteChar(codePoint);
    }
    else if (codePoint < 0x800)
    {
        out += ByteChar(0xC0 | (codePoint >> 6));
        out += ByteChar(0x80 | (codePoint & 0x3F));
    }
    else if (codePoint < 0x1'0000)
    {
        out += ByteChar(0xE0 | (codePoint >> 12));
        out += ByteChar(0x80 | ((codePoint >> 6) & 0x3F));
        out += ByteChar(0x80 | (codePoint & 0x3F));
    }
    else
    {
        out += ByteChar(0xF0 | (codePoint >> 18));
        out += ByteChar(0x80 | ((codePoint >> 12) & 0x3F));
        out += ByteChar(0x80 | ((codePoint >> 6) & 0x3F));
        out += ByteChar(0x80 | (codePoint & 0x3F));
    }
    return true;
}

std::optional<uint32_t> HexValue(std::string_view digits) noexcept
{
    uint32_t value = 0;
    for (const char digit : digits)
    {
        const uint32_t byte = std::bit_cast<unsigned char>(digit);
        uint32_t nibble = 16;
        if (IsDigit(digit))
        {
            nibble = byte - uint32_t{'0'};
        }
        else if (digit >= 'a' && digit <= 'f')
        {
            nibble = byte - uint32_t{'a'} + 10;
        }
        else if (digit >= 'A' && digit <= 'F')
        {
            nibble = byte - uint32_t{'A'} + 10;
        }
        if (nibble >= 16)
        {
            return std::nullopt;
        }
        value = (value << 4) | nibble;
    }
    return value;
}

// A basic string starting at text[0] == '"'. On success, `consumed` is its length.
std::optional<std::string> ParseBasicString(std::string_view text, size_t& consumed, std::string& error)
{
    std::string result;
    for (size_t index = 1; index < text.size(); ++index)
    {
        const char value = text.at(index);
        if (value == '"')
        {
            consumed = index + 1;
            return result;
        }
        if (std::bit_cast<unsigned char>(value) < 0x20 && value != '\t')
        {
            error = "control characters must be escaped in strings";
            return std::nullopt;
        }
        if (value != '\\')
        {
            result += value;
            continue;
        }
        if (++index >= text.size())
        {
            break;
        }
        const char escape = text.at(index);
        constexpr std::string_view kEscapes = "btnfr\"\\";
        constexpr std::string_view kMeanings = "\b\t\n\f\r\"\\";
        if (const auto simple = kEscapes.find(escape); simple != std::string_view::npos)
        {
            result += kMeanings.at(simple);
            continue;
        }
        size_t length = 0; // Hex digits after a "u" or "U" escape.
        if (escape == 'u')
        {
            length = 4;
        }
        else if (escape == 'U')
        {
            length = 8;
        }
        const auto codePoint = length == 0 ? std::nullopt : HexValue(text.substr(index + 1, length));
        if (!codePoint || index + length >= text.size() || !AppendUtf8(result, *codePoint))
        {
            error = "invalid escape sequence";
            return std::nullopt;
        }
        index += length;
    }
    error = "string is not closed";
    return std::nullopt;
}

// Removes "_" between digits; nullopt if one is misplaced.
std::optional<std::string> WithoutSeparators(std::string_view digits)
{
    std::string result;
    for (size_t index = 0; index < digits.size(); ++index)
    {
        const char value = digits.at(index);
        if (value == '_')
        {
            if (index == 0 || index + 1 == digits.size() || !IsDigit(digits.at(index - 1)) ||
                !IsDigit(digits.at(index + 1)))
            {
                return std::nullopt;
            }
            continue;
        }
        result += value;
    }
    return result;
}

ParsedValue ParseNumber(std::string_view token)
{
    const bool negative = token.starts_with('-');
    const std::string_view magnitudeText = token.starts_with('-') || token.starts_with('+') ? token.substr(1) : token;
    if (magnitudeText.starts_with("0x") || magnitudeText.starts_with("0o") || magnitudeText.starts_with("0b"))
    {
        return Fail("hexadecimal, octal and binary numbers are not supported");
    }
    if (magnitudeText == "inf" || magnitudeText == "nan")
    {
        return Fail("inf and nan are not supported");
    }
    const auto digits = WithoutSeparators(magnitudeText);
    if (!digits || digits->empty() || !IsDigit(digits->front()))
    {
        return Fail(std::format("'{}' is not a valid value", token));
    }

    const bool isFloat = digits->find_first_of(".eE") != std::string::npos;
    if (!isFloat)
    {
        if (!std::ranges::all_of(*digits, IsDigit) || (digits->size() > 1 && digits->front() == '0'))
        {
            return Fail(std::format("'{}' is not a valid integer", token));
        }
        constexpr uint64_t kMax = std::numeric_limits<int64_t>::max();
        uint64_t magnitude = 0;
        for (const char digit : *digits)
        {
            const uint64_t digitValue = std::bit_cast<unsigned char>(digit) - uint64_t{'0'};
            if (magnitude > (kMax + 1 - digitValue) / 10)
            {
                return Fail(std::format("{} is out of range", token));
            }
            magnitude = (magnitude * 10) + digitValue;
        }
        if (magnitude > kMax + (negative ? 1 : 0))
        {
            return Fail(std::format("{} is out of range", token));
        }
        int64_t value = std::numeric_limits<int64_t>::min(); // -2^63 has no positive counterpart.
        if (magnitude <= kMax)
        {
            value = negative ? -std::bit_cast<int64_t>(magnitude) : std::bit_cast<int64_t>(magnitude);
        }
        return {.Value = SettingValue(value), .Error = {}};
    }

    // digits [. digits] [e [+-] digits], as TOML writes floats.
    size_t index = 0;
    const auto readDigits = [&]() noexcept
    {
        const size_t start = index;
        while (index < digits->size() && IsDigit(digits->at(index)))
        {
            ++index;
        }
        return index > start;
    };
    bool valid = readDigits();
    if (valid && index < digits->size() && digits->at(index) == '.')
    {
        ++index;
        valid = readDigits();
    }
    if (valid && index < digits->size() && (digits->at(index) == 'e' || digits->at(index) == 'E'))
    {
        ++index;
        if (index < digits->size() && (digits->at(index) == '+' || digits->at(index) == '-'))
        {
            ++index;
        }
        valid = readDigits();
    }
    if (!valid || index != digits->size())
    {
        return Fail(std::format("'{}' is not a valid float", token));
    }
    try
    {
        const double magnitude = std::stod(*digits);
        return {.Value = SettingValue(negative ? -magnitude : magnitude), .Error = {}};
    }
    catch (const std::exception&)
    {
        return Fail(std::format("{} is out of range", token));
    }
}

// A value and what follows it on the line; anything but a comment after it is an error.
ParsedValue ParseValue(std::string_view text)
{
    text = TrimLeft(text);
    if (text.empty() || text.front() == '#')
    {
        return Fail("missing value");
    }
    const char first = text.front();
    if (text.starts_with(R"(""")") || text.starts_with("'''"))
    {
        return Fail("multi-line strings are not supported");
    }
    if (first == '[')
    {
        return Fail("arrays are not supported");
    }
    if (first == '{')
    {
        return Fail("inline tables are not supported");
    }

    std::string_view rest;
    ParsedValue parsed;
    if (first == '"')
    {
        size_t consumed = 0;
        std::string error;
        auto value = ParseBasicString(text, consumed, error);
        if (!value)
        {
            return Fail(error);
        }
        parsed.Value = SettingValue(std::move(*value));
        rest = text.substr(consumed);
    }
    else if (first == '\'')
    {
        const auto end = text.find('\'', 1);
        if (end == std::string_view::npos)
        {
            return Fail("string is not closed");
        }
        parsed.Value = SettingValue(std::string(text.substr(1, end - 1)));
        rest = text.substr(end + 1);
    }
    else
    {
        const auto end = text.find_first_of(" \t#");
        const auto token = text.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : text.substr(end);
        if (token == "true" || token == "false")
        {
            parsed.Value = SettingValue(token == "true");
        }
        else if (token.size() >= 5 && IsDigit(token.front()) &&
                 (token.at(4) == '-' || token.find(':') != std::string_view::npos))
        {
            return Fail("dates and times are not supported");
        }
        else
        {
            parsed = ParseNumber(token);
            if (!parsed.Value)
            {
                return parsed;
            }
        }
    }
    if (!IsEmptyRest(rest))
    {
        return Fail("unexpected text after the value");
    }
    return parsed;
}

// A bare, quoted or dotted key at the start of `text`, joined with "."; `consumed` is its length.
std::optional<std::string> ParseKey(std::string_view text, size_t& consumed, std::string& error)
{
    std::string key;
    size_t index = 0;
    for (;;)
    {
        while (index < text.size() && (text.at(index) == ' ' || text.at(index) == '\t'))
        {
            ++index;
        }
        std::string part;
        if (index < text.size() && text.at(index) == '"')
        {
            size_t length = 0;
            auto quoted = ParseBasicString(text.substr(index), length, error);
            if (!quoted)
            {
                return std::nullopt;
            }
            part = std::move(*quoted);
            index += length;
        }
        else if (index < text.size() && text.at(index) == '\'')
        {
            const auto end = text.find('\'', index + 1);
            if (end == std::string_view::npos)
            {
                error = "key is not closed";
                return std::nullopt;
            }
            part = text.substr(index + 1, end - index - 1);
            index = end + 1;
        }
        else
        {
            const size_t start = index;
            while (index < text.size() && IsBareKeyChar(text.at(index)))
            {
                ++index;
            }
            if (index == start)
            {
                error = "expected a key";
                return std::nullopt;
            }
            part = text.substr(start, index - start);
        }
        key += key.empty() ? part : "." + part;
        while (index < text.size() && (text.at(index) == ' ' || text.at(index) == '\t'))
        {
            ++index;
        }
        if (index < text.size() && text.at(index) == '.')
        {
            ++index;
            continue;
        }
        consumed = index;
        return key;
    }
}

std::string SourceName(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}
} // namespace

std::string_view ToString(SettingType type) noexcept
{
    switch (type)
    {
    case SettingType::Bool:
        return "boolean";
    case SettingType::Integer:
        return "integer";
    case SettingType::Float:
        return "float";
    case SettingType::String:
        return "string";
    }
    return "unknown";
}

std::string ToString(const ConfigIssue& issue)
{
    return issue.Line == 0 ? std::format("{}: {}", issue.Source, issue.Message)
                           : std::format("{}:{}: {}", issue.Source, issue.Line, issue.Message);
}

std::string FormatSettingValue(const SettingValue& value)
{
    if (const auto* flag = std::get_if<bool>(&value))
    {
        return *flag ? "true" : "false";
    }
    if (const auto* integer = std::get_if<int64_t>(&value))
    {
        return std::to_string(*integer);
    }
    if (const auto* number = std::get_if<double>(&value))
    {
        std::string text = std::format("{}", *number);
        if (text.find_first_of(".en") == std::string::npos)
        {
            text += ".0";
        }
        return text;
    }
    std::string quoted = "\"";
    for (const char character : std::get<std::string>(value))
    {
        if (character == '"' || character == '\\')
        {
            quoted += '\\';
        }
        quoted += character;
    }
    return quoted + "\"";
}

Config::Config(std::vector<SettingDefinition> schema)
    : m_schema(std::move(schema))
{
    for (const auto& setting : m_schema)
    {
        m_entries.insert_or_assign(setting.Key, Entry{.Value = setting.Default, .Source = "default"});
    }
}

const SettingDefinition* Config::Find(std::string_view key) const noexcept
{
    const auto it =
        std::ranges::find_if(m_schema, [key](const SettingDefinition& setting) noexcept { return setting.Key == key; });
    return it == m_schema.end() ? nullptr : &*it;
}

void Config::Set(std::string_view key, SettingValue value, std::string_view source, size_t line,
                 std::vector<ConfigIssue>& issues)
{
    const auto report = [&](std::string message)
    { issues.push_back({.Source = std::string(source), .Line = line, .Message = std::move(message)}); };
    const auto* setting = Find(key);
    if (setting == nullptr)
    {
        report(std::format("unknown setting '{}'", key));
        return;
    }
    if (setting->Type == SettingType::Float)
    {
        if (const auto* integer = std::get_if<int64_t>(&value))
        {
            const auto asFloat = static_cast<double>(*integer);
            value = asFloat;
        }
    }
    const bool matches = (setting->Type == SettingType::Bool && std::holds_alternative<bool>(value)) ||
                         (setting->Type == SettingType::Integer && std::holds_alternative<int64_t>(value)) ||
                         (setting->Type == SettingType::Float && std::holds_alternative<double>(value)) ||
                         (setting->Type == SettingType::String && std::holds_alternative<std::string>(value));
    if (!matches)
    {
        report(std::format("'{}' must be a {}", key, ToString(setting->Type)));
        return;
    }
    if (const auto* text = std::get_if<std::string>(&value); text != nullptr && !setting->Choices.empty())
    {
        const auto choice = std::ranges::find_if(setting->Choices, [text](const std::string& allowed) noexcept
                                                 { return EqualsIgnoreCase(allowed, *text); });
        if (choice == setting->Choices.end())
        {
            std::string allowed;
            for (const auto& option : setting->Choices)
            {
                allowed += (allowed.empty() ? "" : ", ") + option;
            }
            report(std::format("'{}' can't be \"{}\" (allowed: {})", key, *text, allowed));
            return;
        }
        value = *choice;
    }
    m_entries.insert_or_assign(std::string(key), Entry{.Value = std::move(value), .Source = std::string(source)});
}

std::vector<ConfigIssue> Config::LoadText(std::string_view text, std::string_view source)
{
    std::vector<ConfigIssue> issues;
    const auto report = [&](size_t line, std::string message)
    { issues.push_back({.Source = std::string(source), .Line = line, .Message = std::move(message)}); };
    if (text.starts_with("\xEF\xBB\xBF"))
    {
        text.remove_prefix(3);
    }

    std::string table;
    std::set<std::string, std::less<>> seen;
    size_t lineNumber = 0;
    while (!text.empty())
    {
        ++lineNumber;
        const auto newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (line.ends_with('\r'))
        {
            line.remove_suffix(1);
        }
        line = Trim(line);
        if (line.empty() || line.front() == '#')
        {
            continue;
        }

        std::string error;
        size_t consumed = 0;
        if (line.front() == '[')
        {
            if (line.starts_with("[["))
            {
                report(lineNumber, "arrays of tables are not supported");
                continue;
            }
            const auto name = ParseKey(line.substr(1), consumed, error);
            const auto rest = name ? line.substr(1 + consumed) : std::string_view{};
            if (!name || !rest.starts_with(']') || !IsEmptyRest(rest.substr(1)))
            {
                report(lineNumber, error.empty() ? "malformed table header" : error);
                continue;
            }
            table = *name;
            continue;
        }

        const auto key = ParseKey(line, consumed, error);
        if (!key || consumed >= line.size() || line.at(consumed) != '=')
        {
            report(lineNumber, error.empty() ? "expected key = value" : error);
            continue;
        }
        const std::string fullKey = table.empty() ? *key : table + "." + *key;
        if (!seen.insert(fullKey).second)
        {
            report(lineNumber, std::format("'{}' is set twice", fullKey));
            continue;
        }
        auto parsed = ParseValue(line.substr(consumed + 1));
        if (!parsed.Value)
        {
            report(lineNumber, std::format("'{}': {}", fullKey, parsed.Error));
            continue;
        }
        Set(fullKey, std::move(*parsed.Value), source, lineNumber, issues);
    }
    return issues;
}

std::vector<ConfigIssue> Config::LoadFile(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        return {};
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return {{.Source = SourceName(path), .Line = 0, .Message = "can't be read"}};
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return LoadText(text, SourceName(path));
}

std::vector<ConfigIssue> Config::ApplyOverride(std::string_view assignment, std::string_view source)
{
    std::vector<ConfigIssue> issues;
    const auto equals = assignment.find('=');
    if (equals == std::string_view::npos)
    {
        issues.push_back(
            {.Source = std::string(source), .Line = 0, .Message = std::format("'{}' isn't key=value", assignment)});
        return issues;
    }
    const auto key = std::string(Trim(assignment.substr(0, equals)));
    const auto text = Trim(assignment.substr(equals + 1));
    auto parsed = ParseValue(text);
    const auto* setting = Find(key);
    if (!parsed.Value && setting != nullptr && setting->Type == SettingType::String)
    {
        parsed.Value = SettingValue(std::string(text)); // A plain word: --set log.level=warn.
    }
    if (!parsed.Value)
    {
        issues.push_back(
            {.Source = std::string(source), .Line = 0, .Message = std::format("'{}': {}", key, parsed.Error)});
        return issues;
    }
    Set(key, std::move(*parsed.Value), source, 0, issues);
    return issues;
}

const Config::Entry& Config::EntryFor(std::string_view key) const
{
    const auto it = m_entries.find(key);
    OPENERA_ASSERT(it != m_entries.end(), "setting '{}' isn't in the schema", key);
    static const Entry kMissing{};
    return it == m_entries.end() ? kMissing : it->second;
}

bool Config::GetBool(std::string_view key) const
{
    const auto* value = std::get_if<bool>(&EntryFor(key).Value);
    OPENERA_ASSERT(value != nullptr, "setting '{}' isn't a boolean", key);
    return value != nullptr && *value;
}

int64_t Config::GetInteger(std::string_view key) const
{
    const auto* value = std::get_if<int64_t>(&EntryFor(key).Value);
    OPENERA_ASSERT(value != nullptr, "setting '{}' isn't an integer", key);
    return value == nullptr ? 0 : *value;
}

double Config::GetFloat(std::string_view key) const
{
    const auto* value = std::get_if<double>(&EntryFor(key).Value);
    OPENERA_ASSERT(value != nullptr, "setting '{}' isn't a float", key);
    return value == nullptr ? 0.0 : *value;
}

const std::string& Config::GetString(std::string_view key) const
{
    const auto* value = std::get_if<std::string>(&EntryFor(key).Value);
    OPENERA_ASSERT(value != nullptr, "setting '{}' isn't a string", key);
    static const std::string kEmpty;
    return value == nullptr ? kEmpty : *value;
}

std::string Config::FormatValue(std::string_view key) const
{
    return FormatSettingValue(EntryFor(key).Value);
}

const std::string& Config::SourceOf(std::string_view key) const
{
    return EntryFor(key).Source;
}
} // namespace OpenEra::Core
