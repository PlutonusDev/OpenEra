#include "GameConfig.h"

#include <algorithm>
#include <string>
#include <utility>

#include "Core/Logger.h"
#include "Core/Xml.h"

namespace OpenEra::Loader
{
namespace
{
using Core::XmlElement;

constexpr std::string_view kSeriesFamily = "Scarlett";

std::string Trimmed(std::string_view text)
{
    constexpr std::string_view kSpace = " \t\r\n";
    const auto first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(kSpace);
    return std::string(text.substr(first, last - first + 1));
}

std::string AttributeOr(const XmlElement* element, std::string_view name)
{
    if (element == nullptr)
    {
        return {};
    }
    const auto* value = element->Attribute(name);
    return value == nullptr ? std::string{} : Trimmed(*value);
}

std::string TextOf(const XmlElement* element)
{
    return element == nullptr ? std::string{} : Trimmed(element->Text);
}
} // namespace

std::string_view ToString(GameConfigError error) noexcept
{
    switch (error)
    {
    case GameConfigError::None:
        return "None";
    case GameConfigError::NotXml:
        return "NotXml";
    case GameConfigError::NotGameConfig:
        return "NotGameConfig";
    case GameConfigError::NoExecutables:
        return "NoExecutables";
    case GameConfigError::BadExecutable:
        return "BadExecutable";
    }
    return "Unknown";
}

GameConfigResult ParseGameConfig(std::string_view text, Core::Logger& logger)
{
    const auto parsed = Core::ParseXml(text);
    if (!parsed.Root)
    {
        logger.Warn("Config: not valid XML: {} (line {})", parsed.Error, parsed.Line);
        return {.Error = GameConfigError::NotXml, .Config = std::nullopt};
    }
    const XmlElement& root = *parsed.Root;
    if (root.LocalName() != "Game")
    {
        logger.Warn("Config: root element is <{}>, not <Game>", root.Name);
        return {.Error = GameConfigError::NotGameConfig, .Config = std::nullopt};
    }

    GameConfig config;
    const auto* identity = root.Child("Identity");
    if (identity == nullptr)
    {
        logger.Warn("Config: no <Identity> element");
    }
    config.IdentityName = AttributeOr(identity, "Name");
    config.Publisher = AttributeOr(identity, "Publisher");
    config.Version = AttributeOr(identity, "Version");
    config.TitleId = TextOf(root.Child("TitleId"));
    config.DisplayName = AttributeOr(root.Child("ShellVisuals"), "DefaultDisplayName");

    const auto* list = root.Child("ExecutableList");
    if (list != nullptr)
    {
        for (const auto& child : list->Children)
        {
            if (child.LocalName() != "Executable")
            {
                logger.Trace("Config: ignoring <{}> in <ExecutableList>", child.Name);
                continue;
            }
            GameExecutable executable{.Name = AttributeOr(&child, "Name"),
                                      .Id = AttributeOr(&child, "Id"),
                                      .TargetDeviceFamily = AttributeOr(&child, "TargetDeviceFamily")};
            if (executable.Name.empty())
            {
                logger.Warn("Config: an <Executable> has no Name");
                return {.Error = GameConfigError::BadExecutable, .Config = std::nullopt};
            }
            config.Executables.push_back(std::move(executable));
        }
    }
    if (config.Executables.empty())
    {
        logger.Warn("Config: no executables listed");
        return {.Error = GameConfigError::NoExecutables, .Config = std::nullopt};
    }
    return {.Error = GameConfigError::None, .Config = std::move(config)};
}

const GameExecutable* SelectExecutable(const GameConfig& config, std::string_view id) noexcept
{
    const auto& executables = config.Executables;
    if (!id.empty())
    {
        const auto it =
            std::ranges::find_if(executables, [id](const GameExecutable& entry) noexcept { return entry.Id == id; });
        return it == executables.end() ? nullptr : &*it;
    }
    const auto series = std::ranges::find_if(executables, [](const GameExecutable& entry) noexcept
                                             { return entry.TargetDeviceFamily == kSeriesFamily; });
    if (series != executables.end())
    {
        return &*series;
    }
    return executables.empty() ? nullptr : &executables.front();
}
} // namespace OpenEra::Loader
