// Sample configs are written from the public MicrosoftGame.config schema.
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Loader/GameConfig.h"

using namespace OpenEra::Loader;
using OpenEra::Core::Logger;

TEST_CASE("A single-executable config is read", "[config]")
{
    Logger logger;
    const auto result = ParseGameConfig(R"(<?xml version="1.0" encoding="utf-8"?>
<Game configVersion="1">
  <Identity Name="Contoso.SampleGame" Publisher="CN=Contoso" Version="1.2.3.0"/>
  <ExecutableList>
    <Executable Name="SampleGame.exe" TargetDeviceFamily="Scarlett" Id="Game"/>
  </ExecutableList>
  <TitleId> 12345678 </TitleId>
  <ShellVisuals DefaultDisplayName="Sample Game" PublisherDisplayName="Contoso"/>
  <SomethingNew Value="ignored"/>
</Game>)",
                                        logger);
    REQUIRE(result.Error == GameConfigError::None);
    REQUIRE(result.Config.has_value());
    const auto& config = result.Config.value();
    CHECK(config.IdentityName == "Contoso.SampleGame");
    CHECK(config.Publisher == "CN=Contoso");
    CHECK(config.Version == "1.2.3.0");
    CHECK(config.TitleId == "12345678");
    CHECK(config.DisplayName == "Sample Game");
    REQUIRE(config.Executables.size() == 1);
    CHECK(config.Executables.front().Name == "SampleGame.exe");
    CHECK(config.Executables.front().Id == "Game");
    CHECK(config.Executables.front().TargetDeviceFamily == "Scarlett");
}

TEST_CASE("The Series X|S executable is selected by default", "[config]")
{
    Logger logger;
    const auto result = ParseGameConfig(R"(<Game configVersion="1">
  <Identity Name="Contoso.SampleGame" Publisher="CN=Contoso" Version="1.0.0.0"/>
  <ExecutableList>
    <Executable Name="Launcher.exe" Id="Launcher"/>
    <Executable Name="bin\Scarlett\Game.exe" TargetDeviceFamily="Scarlett" Id="Game"/>
    <Executable Name="Tool.exe" TargetDeviceFamily="PC" Id="Tool"/>
  </ExecutableList>
</Game>)",
                                        logger);
    REQUIRE(result.Config.has_value());
    const auto& config = result.Config.value();
    REQUIRE(config.Executables.size() == 3);

    const auto* selected = SelectExecutable(config);
    REQUIRE(selected != nullptr);
    CHECK(selected->Id == "Game");
    CHECK(selected->Name == "bin\\Scarlett\\Game.exe");

    const auto* tool = SelectExecutable(config, "Tool");
    REQUIRE(tool != nullptr);
    CHECK(tool->Name == "Tool.exe");
    CHECK(SelectExecutable(config, "Missing") == nullptr);

    GameConfig noSeries = config;
    noSeries.Executables.erase(noSeries.Executables.begin() + 1);
    const auto* first = SelectExecutable(noSeries);
    REQUIRE(first != nullptr);
    CHECK(first->Id == "Launcher");
}

TEST_CASE("Unusable configs are reported", "[config]")
{
    Logger logger;
    CHECK(ParseGameConfig("<Game", logger).Error == GameConfigError::NotXml);
    CHECK(ParseGameConfig("<Package/>", logger).Error == GameConfigError::NotGameConfig);
    CHECK(ParseGameConfig("<Game><Identity Name=\"A\"/></Game>", logger).Error == GameConfigError::NoExecutables);
    CHECK(ParseGameConfig("<Game><ExecutableList/></Game>", logger).Error == GameConfigError::NoExecutables);
    CHECK(ParseGameConfig("<Game><ExecutableList><Executable Id=\"Game\"/></ExecutableList></Game>", logger).Error ==
          GameConfigError::BadExecutable);

    const auto missingIdentity =
        ParseGameConfig("<Game><ExecutableList><Executable Name=\"Game.exe\"/></ExecutableList></Game>", logger);
    REQUIRE(missingIdentity.Config.has_value());
    CHECK(missingIdentity.Config->IdentityName.empty());
}
