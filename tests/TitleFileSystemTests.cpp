#include <filesystem>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "Kernel/TitleFileSystem.h"
#include "Support/TempFolder.h"

using OpenEra::Kernel::TitleFileSystem;
using OpenEra::Tests::TempFolder;

TEST_CASE("Paths are mapped relative to the title folder", "[files]")
{
    const TempFolder title("TitleRelative");
    const TempFolder overlay("TitleRelativeOverlay");
    const TitleFileSystem files(title.Path(), overlay.Path());

    CHECK(files.TitleRelative(title.Path() / "Media" / "a.txt") ==
          std::optional<std::filesystem::path>(std::filesystem::path("Media") / "a.txt"));
    CHECK(files.TitleRelative(title.Path()) == std::optional<std::filesystem::path>(std::filesystem::path(".")));
    CHECK(files.TitleRelative(title.Path() / "Media" / ".." / "b.txt") ==
          std::optional<std::filesystem::path>(std::filesystem::path("b.txt")));

    // Case-insensitive, and the \\?\ prefix is accepted.
    auto upper = title.Path().native();
    for (auto& value : upper)
    {
        value = value >= L'a' && value <= L'z' ? static_cast<wchar_t>(value - L'a' + L'A') : value;
    }
    CHECK(files.TitleRelative(std::filesystem::path(upper) / "X.BIN") ==
          std::optional<std::filesystem::path>(std::filesystem::path("X.BIN")));
    CHECK(files.TitleRelative(std::filesystem::path(L"\\\\?\\" + (title.Path() / "c.txt").native())) ==
          std::optional<std::filesystem::path>(std::filesystem::path("c.txt")));

    // Outside the folder: siblings, parents, escapes through "..".
    CHECK_FALSE(files.TitleRelative(title.Path().parent_path()).has_value());
    CHECK_FALSE(files.TitleRelative(title.Path() / ".." / "elsewhere.txt").has_value());
    CHECK_FALSE(files.TitleRelative(std::filesystem::path(title.Path().native() + L"-sibling") / "a.txt").has_value());
}

TEST_CASE("Writes copy to the overlay and leave the title folder unchanged", "[files]")
{
    const TempFolder title("CopyOnWrite");
    const TempFolder overlay("CopyOnWriteOverlay");
    title.Write("Data/settings.ini", "original");
    TitleFileSystem files(title.Path(), overlay.Path());

    CHECK(files.ReadPath("Data/settings.ini") ==
          std::optional<std::filesystem::path>(title.Path() / "Data/settings.ini"));

    const auto write = files.WritePath("Data/settings.ini", true);
    REQUIRE(write.has_value());
    CHECK(*write == (overlay.Path() / "Data/settings.ini").lexically_normal());
    CHECK(TempFolder::Read(*write) == "original");
    std::ofstream(*write, std::ios::binary | std::ios::trunc) << "changed";

    CHECK(files.ReadPath("Data/settings.ini") ==
          std::optional<std::filesystem::path>(overlay.Path() / "Data/settings.ini"));
    CHECK(TempFolder::Read(title.Path() / "Data/settings.ini") == "original");

    // A new file only exists in the overlay.
    const auto created = files.WritePath("Saves/slot1.bin", true);
    REQUIRE(created.has_value());
    CHECK(std::filesystem::is_directory(created->parent_path()));
    CHECK_FALSE(std::filesystem::exists(title.Path() / "Saves"));
}

TEST_CASE("Deleting hides the title's file and survives a restart", "[files]")
{
    const TempFolder title("Delete");
    const TempFolder overlay("DeleteOverlay");
    title.Write("cache.dat", "cached");
    {
        TitleFileSystem files(title.Path(), overlay.Path());
        CHECK(files.Exists("cache.dat"));
        CHECK_FALSE(files.Delete("cache.dat"));
        CHECK(files.IsDeleted("CACHE.DAT"));
        CHECK_FALSE(files.Exists("cache.dat"));
        CHECK_FALSE(files.ReadPath("cache.dat").has_value());
        CHECK(files.Delete("cache.dat") == std::errc::no_such_file_or_directory);
        CHECK(std::filesystem::exists(title.Path() / "cache.dat"));
    }
    {
        TitleFileSystem files(title.Path(), overlay.Path());
        CHECK(files.IsDeleted("cache.dat"));
        CHECK(files.IsInternal(".openera-deleted"));

        // Writing it again doesn't bring the old contents back.
        const auto write = files.WritePath("cache.dat", true);
        REQUIRE(write.has_value());
        CHECK_FALSE(std::filesystem::exists(*write));
        files.Undelete("cache.dat");
        CHECK_FALSE(files.IsDeleted("cache.dat"));
    }
    {
        const TitleFileSystem files(title.Path(), overlay.Path());
        CHECK_FALSE(files.IsDeleted("cache.dat"));
    }
}

TEST_CASE("Deleting an overlay-only file removes it", "[files]")
{
    const TempFolder title("DeleteOverlayOnly");
    const TempFolder overlay("DeleteOverlayOnlyOverlay");
    TitleFileSystem files(title.Path(), overlay.Path());
    const auto write = files.WritePath("new.txt", true);
    REQUIRE(write.has_value());
    std::ofstream(*write) << "x";
    CHECK(files.Exists("new.txt"));
    CHECK_FALSE(files.Delete("new.txt"));
    CHECK_FALSE(files.Exists("new.txt"));
    CHECK_FALSE(files.IsDeleted("new.txt")); // Nothing in the title folder to hide.
}
