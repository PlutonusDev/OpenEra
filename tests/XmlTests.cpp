#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "Core/Xml.h"

using OpenEra::Core::ParseXml;

TEST_CASE("XML elements, attributes and text are read", "[xml]")
{
    const auto result = ParseXml(R"(<?xml version="1.0" encoding="utf-8"?>
<!-- comment -->
<Root a="1" b='two'>
  <Child x="y"/>
  <Child>text &amp; more</Child>
  <ns:Other ns:attr="v"><![CDATA[<raw>]]></ns:Other>
</Root>
)");
    REQUIRE(result.Error.empty());
    REQUIRE(result.Root.has_value());
    const auto& root = result.Root.value();
    CHECK(root.Name == "Root");
    REQUIRE(root.Attribute("a") != nullptr);
    CHECK(*root.Attribute("a") == "1");
    CHECK(*root.Attribute("b") == "two");
    CHECK(root.Attribute("missing") == nullptr);
    REQUIRE(root.Children.size() == 3);
    CHECK(root.Children.at(1).Text == "text & more");

    const auto* other = root.Child("Other");
    REQUIRE(other != nullptr);
    CHECK(other->Name == "ns:Other");
    CHECK(other->Text == "<raw>");
    REQUIRE(other->Attribute("attr") != nullptr);
    CHECK(*other->Attribute("attr") == "v");
}

TEST_CASE("XML character references decode to UTF-8", "[xml]")
{
    const auto result = ParseXml("\xEF\xBB\xBF<a v=\"&lt;&gt;&quot;&apos;&#65;&#x263A;\">&#x1F600;</a>");
    REQUIRE(result.Root.has_value());
    REQUIRE(result.Root->Attribute("v") != nullptr);
    CHECK(*result.Root->Attribute("v") == "<>\"'A\xE2\x98\xBA");
    CHECK(result.Root->Text == "\xF0\x9F\x98\x80");
}

TEST_CASE("Malformed XML is rejected with a line number", "[xml]")
{
    struct Case
    {
        std::string_view Text;
        size_t Line;
    };
    const auto check = [](const Case& sample)
    {
        const auto result = ParseXml(sample.Text);
        CHECK_FALSE(result.Root.has_value());
        CHECK_FALSE(result.Error.empty());
        CHECK(result.Line == sample.Line);
    };
    check({.Text = "", .Line = 1});
    check({.Text = "<a>\n<b></a>", .Line = 2});
    check({.Text = R"(<a x="1" x="2"/>)", .Line = 1});
    check({.Text = "<a>\n\n&bogus;</a>", .Line = 3});
    check({.Text = "<a>&#0;</a>", .Line = 1});
    check({.Text = "<a/><b/>", .Line = 1});
    check({.Text = "<!DOCTYPE a><a/>", .Line = 1});
    check({.Text = "<a x=1/>", .Line = 1});
    check({.Text = "\xFF\xFE<a/>", .Line = 1}); // UTF-16 byte order mark.
}

TEST_CASE("Deeply nested XML is rejected", "[xml]")
{
    std::string text;
    for (int depth = 0; depth < 300; ++depth)
    {
        text += "<a>";
    }
    const auto result = ParseXml(text);
    CHECK_FALSE(result.Root.has_value());
}
