#include "Xml.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace OpenEra::Core
{
namespace
{
constexpr size_t kMaxDepth = 256;
constexpr uint32_t kMaxCodePoint = 0x10'FFFF;

constexpr std::string_view LocalPart(std::string_view name) noexcept
{
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

constexpr char ByteChar(uint32_t value) noexcept
{
    return std::bit_cast<char>(std::bit_cast<std::array<uint8_t, sizeof(uint32_t)>>(value).front());
}

constexpr bool IsSpace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

constexpr bool IsNameChar(char value) noexcept
{
    const auto byte = std::bit_cast<unsigned char>(value);
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
           value == '_' || value == ':' || value == '-' || value == '.' || byte >= 0x80;
}

constexpr std::optional<uint32_t> DigitValue(char value, uint32_t radix) noexcept
{
    const uint32_t byte = std::bit_cast<unsigned char>(value);
    uint32_t digit = radix;
    if (value >= '0' && value <= '9')
    {
        digit = byte - uint32_t{'0'};
    }
    else if (value >= 'a' && value <= 'f')
    {
        digit = byte - uint32_t{'a'} + 10;
    }
    else if (value >= 'A' && value <= 'F')
    {
        digit = byte - uint32_t{'A'} + 10;
    }
    return digit < radix ? std::optional(digit) : std::nullopt;
}

bool AppendUtf8(std::string& out, uint32_t codePoint)
{
    if (codePoint == 0 || codePoint > kMaxCodePoint || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
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

class XmlParser
{
public:
    explicit XmlParser(std::string_view text) noexcept
        : m_text(text)
    {
    }

    XmlParseResult Parse()
    {
        XmlParseResult result;
        XmlElement root;
        if (ParseDocument(root))
        {
            result.Root = std::move(root);
            return result;
        }
        result.Error = m_error;
        result.Line = 1;
        for (const char value : m_text.substr(0, m_errorAt))
        {
            if (value == '\n')
            {
                ++result.Line;
            }
        }
        return result;
    }

private:
    [[nodiscard]] bool AtEnd() const noexcept { return m_pos >= m_text.size(); }
    [[nodiscard]] char Peek() const { return AtEnd() ? '\0' : m_text.at(m_pos); }
    [[nodiscard]] bool LooksAt(std::string_view token) const noexcept
    {
        return m_text.substr(std::min(m_pos, m_text.size())).starts_with(token);
    }

    bool Consume(std::string_view token) noexcept
    {
        if (!LooksAt(token))
        {
            return false;
        }
        m_pos += token.size();
        return true;
    }

    void SkipSpace()
    {
        while (!AtEnd() && IsSpace(Peek()))
        {
            ++m_pos;
        }
    }

    bool Fail(std::string_view message)
    {
        if (m_error.empty())
        {
            m_error = message;
            m_errorAt = std::min(m_pos, m_text.size());
        }
        return false;
    }

    // Skips to just past `terminator`.
    bool SkipPast(std::string_view terminator, std::string_view what)
    {
        const auto end = m_text.find(terminator, m_pos);
        if (end == std::string_view::npos)
        {
            return Fail(std::string(what) + " is not closed");
        }
        m_pos = end + terminator.size();
        return true;
    }

    // Comments and processing instructions; false if neither is next.
    bool SkipMisc(bool& failed)
    {
        if (Consume("<!--"))
        {
            failed = !SkipPast("-->", "comment");
            return true;
        }
        if (Consume("<?"))
        {
            failed = !SkipPast("?>", "processing instruction");
            return true;
        }
        return false;
    }

    bool ParseDocument(XmlElement& root)
    {
        if (LooksAt("\xFF\xFE") || LooksAt("\xFE\xFF"))
        {
            return Fail("UTF-16 documents are not supported");
        }
        std::ignore = Consume("\xEF\xBB\xBF");
        for (;;)
        {
            SkipSpace();
            bool failed = false;
            if (SkipMisc(failed))
            {
                if (failed)
                {
                    return false;
                }
                continue;
            }
            break;
        }
        if (LooksAt("<!"))
        {
            return Fail("document type declarations are not supported");
        }
        if (Peek() != '<')
        {
            return Fail("expected the root element");
        }
        if (!ParseElement(root, 0))
        {
            return false;
        }
        for (;;)
        {
            SkipSpace();
            if (AtEnd())
            {
                return true;
            }
            bool failed = false;
            if (!SkipMisc(failed))
            {
                return Fail("content after the root element");
            }
            if (failed)
            {
                return false;
            }
        }
    }

    bool ParseName(std::string& name)
    {
        const size_t start = m_pos;
        while (!AtEnd() && IsNameChar(Peek()))
        {
            ++m_pos;
        }
        if (m_pos == start)
        {
            return Fail("expected a name");
        }
        name = m_text.substr(start, m_pos - start);
        return true;
    }

    bool DecodeReference(std::string& out)
    {
        const auto end = m_text.find(';', m_pos);
        if (end == std::string_view::npos || end - m_pos > 12)
        {
            return Fail("unterminated character reference");
        }
        const auto name = m_text.substr(m_pos, end - m_pos);
        m_pos = end + 1;
        static constexpr std::array<std::pair<std::string_view, char>, 5> kEntities = {
            {{"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}}};
        if (const auto it = std::ranges::find(kEntities, name, &std::pair<std::string_view, char>::first);
            it != kEntities.end())
        {
            out += it->second;
            return true;
        }
        if (!name.starts_with('#') || name.size() < 2)
        {
            return Fail("unknown entity reference");
        }
        const bool hex = name.size() > 2 && (name.at(1) == 'x');
        const uint32_t radix = hex ? 16 : 10;
        const auto digits = name.substr(hex ? 2 : 1);
        uint32_t codePoint = 0;
        for (const char digit : digits)
        {
            const auto value = DigitValue(digit, radix);
            if (!value || codePoint > kMaxCodePoint)
            {
                return Fail("invalid character reference");
            }
            codePoint = (codePoint * radix) + *value;
        }
        return AppendUtf8(out, codePoint) || Fail("invalid character reference");
    }

    // Character data up to `stop` (not consumed), with references decoded.
    bool ParseCharacters(std::string& out, char stop)
    {
        while (!AtEnd() && Peek() != stop)
        {
            const char value = Peek();
            if (value == '<')
            {
                return Fail("'<' in an attribute value");
            }
            ++m_pos;
            if (value == '&')
            {
                if (!DecodeReference(out))
                {
                    return false;
                }
                continue;
            }
            out += value;
        }
        return true;
    }

    bool ParseAttribute(XmlElement& element)
    {
        XmlAttribute attribute;
        if (!ParseName(attribute.Name))
        {
            return false;
        }
        SkipSpace();
        if (!Consume("="))
        {
            return Fail("expected '=' after an attribute name");
        }
        SkipSpace();
        const char quote = Peek();
        if (quote != '"' && quote != '\'')
        {
            return Fail("expected a quoted attribute value");
        }
        ++m_pos;
        if (!ParseCharacters(attribute.Value, quote))
        {
            return false;
        }
        if (AtEnd())
        {
            return Fail("attribute value is not closed");
        }
        ++m_pos;
        if (std::ranges::any_of(element.Attributes,
                                [&attribute](const XmlAttribute& existing) { return existing.Name == attribute.Name; }))
        {
            return Fail("duplicate attribute");
        }
        element.Attributes.push_back(std::move(attribute));
        return true;
    }

    bool ParseElement(XmlElement& element, size_t depth)
    {
        if (depth > kMaxDepth)
        {
            return Fail("elements are nested too deeply");
        }
        ++m_pos; // '<'
        if (!ParseName(element.Name))
        {
            return false;
        }
        for (;;)
        {
            const size_t before = m_pos;
            SkipSpace();
            if (Consume("/>"))
            {
                return true;
            }
            if (Consume(">"))
            {
                break;
            }
            if (m_pos == before)
            {
                return Fail("expected whitespace, '>' or '/>'");
            }
            if (!ParseAttribute(element))
            {
                return false;
            }
        }
        return ParseContent(element, depth);
    }

    bool ParseContent(XmlElement& element, size_t depth)
    {
        for (;;)
        {
            if (AtEnd())
            {
                return Fail("element <" + element.Name + "> is not closed");
            }
            if (Consume("</"))
            {
                std::string name;
                if (!ParseName(name))
                {
                    return false;
                }
                SkipSpace();
                if (name != element.Name || !Consume(">"))
                {
                    return Fail("mismatched end tag for <" + element.Name + ">");
                }
                return true;
            }
            if (Consume("<![CDATA["))
            {
                const auto end = m_text.find("]]>", m_pos);
                if (end == std::string_view::npos)
                {
                    return Fail("CDATA section is not closed");
                }
                element.Text += m_text.substr(m_pos, end - m_pos);
                m_pos = end + 3;
                continue;
            }
            bool failed = false;
            if (SkipMisc(failed))
            {
                if (failed)
                {
                    return false;
                }
                continue;
            }
            if (LooksAt("<!"))
            {
                return Fail("unexpected markup declaration");
            }
            if (Peek() == '<')
            {
                XmlElement child;
                if (!ParseElement(child, depth + 1))
                {
                    return false;
                }
                element.Children.push_back(std::move(child));
                continue;
            }
            if (!ParseText(element.Text))
            {
                return false;
            }
        }
    }

    bool ParseText(std::string& out)
    {
        while (!AtEnd() && Peek() != '<')
        {
            const char value = Peek();
            ++m_pos;
            if (value == '&')
            {
                if (!DecodeReference(out))
                {
                    return false;
                }
                continue;
            }
            out += value;
        }
        return true;
    }

    std::string_view m_text;
    size_t m_pos = 0;
    std::string m_error;
    size_t m_errorAt = 0;
};
} // namespace

std::string_view XmlElement::LocalName() const noexcept
{
    return LocalPart(Name);
}

const std::string* XmlElement::Attribute(std::string_view localName) const noexcept
{
    const auto it = std::ranges::find_if(Attributes, [localName](const XmlAttribute& attribute) noexcept
                                         { return LocalPart(attribute.Name) == localName; });
    return it == Attributes.end() ? nullptr : &it->Value;
}

const XmlElement* XmlElement::Child(std::string_view localName) const noexcept
{
    const auto it = std::ranges::find_if(Children, [localName](const XmlElement& child) noexcept
                                         { return child.LocalName() == localName; });
    return it == Children.end() ? nullptr : &*it;
}

XmlParseResult ParseXml(std::string_view text)
{
    try
    {
        return XmlParser(text).Parse();
    }
    catch (const std::exception& exception)
    {
        XmlParseResult result;
        result.Error = exception.what();
        return result;
    }
}
} // namespace OpenEra::Core
