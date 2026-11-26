#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenEra::Core
{
struct XmlAttribute
{
    std::string Name;
    std::string Value; // References decoded.
};

struct XmlElement
{
    std::string Name; // As written, including any namespace prefix.
    std::vector<XmlAttribute> Attributes;
    std::string Text; // Character data directly inside this element, references decoded.
    std::vector<XmlElement> Children;

    [[nodiscard]] std::string_view LocalName() const noexcept;
    // Lookups by local name (any prefix ignored); null if absent.
    [[nodiscard]] const std::string* Attribute(std::string_view localName) const noexcept;
    [[nodiscard]] const XmlElement* Child(std::string_view localName) const noexcept;
};

struct XmlParseResult
{
    std::optional<XmlElement> Root; // Set exactly when Error is empty.
    std::string Error;
    size_t Line = 0; // 1-based line of the error.
};

// Minimal non-validating XML 1.0 reader for small configuration files:
// elements, attributes, character data, CDATA sections, comments, processing
// instructions, and the predefined and numeric character references.
// UTF-8 only; document type declarations are rejected.
[[nodiscard]] XmlParseResult ParseXml(std::string_view text);
} // namespace OpenEra::Core
