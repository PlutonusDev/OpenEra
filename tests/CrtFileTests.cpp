// C runtime and C++ library file I/O from an image OpenEra maps itself
// (tests/Fixtures/CrtFileImage.cpp, built with /MD), against a title folder
// that must never change.
#include <array>
#include <bit>
#include <cerrno>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "Core/Logger.h"
#include "Kernel/CrtFileShims.h"
#include "Kernel/ShimModule.h"
#include "Kernel/StubRegistry.h"
#include "Kernel/TitleFileSystem.h"
#include "Loader/HostModules.h"
#include "Loader/ImageMemory.h"
#include "Loader/ModuleManager.h"
#include "Support/TempFolder.h"

using OpenEra::Core::Logger;
using OpenEra::Kernel::ShimRegistry;
using OpenEra::Kernel::StubRegistry;
using OpenEra::Kernel::TitleFileSystem;
using OpenEra::Loader::ModuleManager;
using OpenEra::Tests::TempFolder;

namespace
{
constexpr std::string_view kImage = "CrtFileImage.dll";
constexpr int kBuffer = 512;

// Every file under `root` with its contents, to prove a folder didn't change.
std::map<std::wstring, std::string> Snapshot(const std::filesystem::path& root)
{
    std::map<std::wstring, std::string> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        files.emplace(entry.path().lexically_relative(root).wstring(),
                      entry.is_regular_file() ? TempFolder::Read(entry.path()) : std::string("<dir>"));
    }
    return files;
}

std::set<std::wstring> Names(std::wstring_view joined)
{
    std::set<std::wstring> names;
    size_t start = 0;
    while (start < joined.size())
    {
        const auto end = joined.find(L'|', start);
        names.emplace(joined.substr(start, end - start));
        start = end == std::wstring_view::npos ? joined.size() : end + 1;
    }
    return names;
}

class CrtFixture
{
public:
    CrtFixture()
        : m_title("CrtTitle")
        , m_overlay("CrtOverlay")
    {
        m_title.Write("Data/old.txt", "old");
        m_title.Write("Data/keep.txt", "keep");
        m_title.Write("Data/gone.txt", "gone");
        m_title.Write("Tree/a/one.txt", "1");
        m_title.Write("Tree/two.txt", "2");
        m_before = Snapshot(m_title.Path());

        REQUIRE(OpenEra::Loader::RegisterLoaderShims(m_shims));
        m_manager = std::make_unique<ModuleManager>(OpenEra::Loader::DirectoryModuleReader(OPENERA_FIXTURE_DIR),
                                                    m_shims, m_stubs, *m_allocator, m_log, m_host.get());
        ModuleManager::SetActive(m_manager.get());
        REQUIRE(m_manager->Load(kImage) == OpenEra::Loader::ModuleError::None);
        m_files.emplace(m_title.Path(), m_overlay.Path());
        TitleFileSystem::SetActive(&*m_files);
    }

    ~CrtFixture()
    {
        TitleFileSystem::SetActive(nullptr);
        ModuleManager::SetActive(nullptr);
    }

    CrtFixture(const CrtFixture&) = delete;
    CrtFixture& operator=(const CrtFixture&) = delete;
    CrtFixture(CrtFixture&&) = delete;
    CrtFixture& operator=(CrtFixture&&) = delete;

    template <typename Function>
    [[nodiscard]] Function* Export(std::string_view symbol) const
    {
        const auto address = m_manager->FindExport(kImage, symbol);
        REQUIRE(address.has_value());
        return std::bit_cast<Function*>(address.value_or(0));
    }

    [[nodiscard]] std::wstring Title(std::wstring_view relative) const { return (m_title.Path() / relative).wstring(); }
    [[nodiscard]] std::string TitleAnsi(std::string_view relative) const
    {
        return (m_title.Path() / relative).string();
    }
    [[nodiscard]] const std::filesystem::path& OverlayRoot() const noexcept { return m_overlay.Path(); }
    [[nodiscard]] bool TitleUnchanged() const { return Snapshot(m_title.Path()) == m_before; }

    [[nodiscard]] std::string Read(std::wstring_view relative) const
    {
        std::array<char, kBuffer> text{};
        const int read =
            Export<int(const wchar_t*, char*, int)>("FopenRead")(Title(relative).c_str(), text.data(), kBuffer);
        return read < 0 ? std::string("<missing>") : std::string(text.data());
    }

    [[nodiscard]] std::set<std::wstring> Find(std::wstring_view pattern) const
    {
        std::array<wchar_t, kBuffer> names{};
        const int count =
            Export<int(const wchar_t*, wchar_t*, int)>("CrtFind")(Title(pattern).c_str(), names.data(), kBuffer);
        return count < 0 ? std::set<std::wstring>{} : Names(names.data());
    }

    [[nodiscard]] std::set<std::wstring> Iterate(std::wstring_view directory) const
    {
        std::array<wchar_t, kBuffer> names{};
        const int count = Export<int(const wchar_t*, wchar_t*, int)>("DirectoryEntries")(Title(directory).c_str(),
                                                                                         names.data(), kBuffer);
        return count < 0 ? std::set<std::wstring>{L"<error>"} : Names(names.data());
    }

private:
    TempFolder m_title;
    TempFolder m_overlay;
    std::map<std::wstring, std::string> m_before;
    Logger m_log;
    StubRegistry m_stubs{m_log};
    ShimRegistry m_shims;
    std::unique_ptr<OpenEra::Loader::ImageAllocator> m_allocator = OpenEra::Loader::CreateVirtualImageAllocator();
    std::unique_ptr<OpenEra::Loader::HostModules> m_host = OpenEra::Loader::HostModules::CreateDefault();
    std::unique_ptr<ModuleManager> m_manager;
    std::optional<TitleFileSystem> m_files;
};
} // namespace

TEST_CASE("fopen modes say whether they write and keep contents", "[crtfiles]")
{
    using OpenEra::Kernel::KeepsContents;
    using OpenEra::Kernel::OpensForWrite;
    CHECK_FALSE(OpensForWrite(L"r"));
    CHECK_FALSE(OpensForWrite(L"rb"));
    CHECK(OpensForWrite(L"w"));
    CHECK(OpensForWrite(L"ab"));
    CHECK(OpensForWrite(L"r+"));
    CHECK_FALSE(OpensForWrite(L"r, ccs=UTF-8"));
    CHECK(KeepsContents(L"a"));
    CHECK(KeepsContents(L"r+b"));
    CHECK_FALSE(KeepsContents(L"w+"));
}

TEST_CASE("fopen writes land in the overlay and read back", "[crtfiles]")
{
    const CrtFixture fixture;
    using Write = int(const wchar_t*, const char*, const wchar_t*);
    auto* const write = fixture.Export<Write>("FopenWrite");

    CHECK(write(fixture.Title(L"Data\\new.txt").c_str(), "hello", L"w") == 0);
    CHECK(fixture.Read(L"Data\\new.txt") == "hello");
    CHECK(std::filesystem::exists(fixture.OverlayRoot() / "Data" / "new.txt"));

    CHECK(write(fixture.Title(L"Data\\old.txt").c_str(), "+1", L"a") == 0); // Appends to a copy.
    CHECK(fixture.Read(L"Data\\old.txt") == "old+1");
    CHECK(write(fixture.Title(L"Data\\keep.txt").c_str(), "new", L"w") == 0); // Replaces a copy.
    CHECK(fixture.Read(L"Data\\keep.txt") == "new");

    // Narrow paths too.
    using NarrowWrite = int(const char*, const char*);
    using NarrowRead = int(const char*, char*, int);
    CHECK(fixture.Export<NarrowWrite>("NarrowFopenWrite")(fixture.TitleAnsi("Data/narrow.txt").c_str(), "n") == 0);
    std::array<char, kBuffer> text{};
    CHECK(fixture.Export<NarrowRead>("NarrowFopenRead")(fixture.TitleAnsi("Data/narrow.txt").c_str(), text.data(),
                                                        kBuffer) == 1);
    CHECK(std::string(text.data()) == "n");

    CHECK(fixture.TitleUnchanged());
}

TEST_CASE("std::fstream writes land in the overlay and read back", "[crtfiles]")
{
    const CrtFixture fixture;
    auto* const write = fixture.Export<int(const wchar_t*, const char*, int)>("StreamWrite");
    auto* const read = fixture.Export<int(const wchar_t*, char*, int)>("StreamRead");
    std::array<char, kBuffer> text{};

    CHECK(write(fixture.Title(L"Data\\stream.txt").c_str(), "streamed", 0) == 0);
    CHECK(read(fixture.Title(L"Data\\stream.txt").c_str(), text.data(), kBuffer) == 8);
    CHECK(std::string(text.data()) == "streamed");

    CHECK(write(fixture.Title(L"Data\\old.txt").c_str(), "+app", 1) == 0);
    CHECK(read(fixture.Title(L"Data\\old.txt").c_str(), text.data(), kBuffer) == 7);
    CHECK(std::string(text.data()) == "old+app");

    CHECK(fixture.Export<int(const char*, const char*)>("NarrowStreamWrite")(
              fixture.TitleAnsi("Data/narrow-stream.txt").c_str(), "ns") == 0);
    CHECK(fixture.Read(L"Data\\narrow-stream.txt") == "ns");
    CHECK(fixture.TitleUnchanged());
}

TEST_CASE("C runtime removes, renames, directories, access and stat use the layer", "[crtfiles]")
{
    const CrtFixture fixture;
    auto* const remove = fixture.Export<int(const wchar_t*)>("CrtRemove");
    auto* const rename = fixture.Export<int(const wchar_t*, const wchar_t*)>("CrtRename");
    auto* const makeDirectory = fixture.Export<int(const wchar_t*)>("CrtMakeDirectory");
    auto* const removeDirectory = fixture.Export<int(const wchar_t*)>("CrtRemoveDirectory");
    auto* const access = fixture.Export<int(const wchar_t*)>("CrtAccess");
    auto* const statSize = fixture.Export<long long(const wchar_t*)>("CrtStatSize");
    auto* const lastErrno = fixture.Export<int()>("LastErrno");

    CHECK(statSize(fixture.Title(L"Data\\old.txt").c_str()) == 3);
    CHECK(fixture.Export<long long(const char*)>("NarrowStatSize")(fixture.TitleAnsi("Data/keep.txt").c_str()) == 4);
    CHECK(access(fixture.Title(L"Data\\keep.txt").c_str()) == 0);

    CHECK(remove(fixture.Title(L"Data\\gone.txt").c_str()) == 0);
    CHECK(access(fixture.Title(L"Data\\gone.txt").c_str()) == -1);
    CHECK(lastErrno() == ENOENT);
    CHECK(statSize(fixture.Title(L"Data\\gone.txt").c_str()) == -1);
    CHECK(remove(fixture.Title(L"Data\\gone.txt").c_str()) == -1);

    CHECK(rename(fixture.Title(L"Data\\old.txt").c_str(), fixture.Title(L"Data\\renamed.txt").c_str()) == 0);
    CHECK(fixture.Read(L"Data\\renamed.txt") == "old");
    CHECK(fixture.Read(L"Data\\old.txt") == "<missing>");
    CHECK(rename(fixture.Title(L"Data\\renamed.txt").c_str(), fixture.Title(L"Data\\keep.txt").c_str()) == -1);
    CHECK(lastErrno() == EACCES); // rename never replaces.

    CHECK(makeDirectory(fixture.Title(L"Saves").c_str()) == 0);
    CHECK(makeDirectory(fixture.Title(L"Saves").c_str()) == -1);
    CHECK(lastErrno() == EEXIST);
    CHECK(access(fixture.Title(L"Saves").c_str()) == 0);
    CHECK(removeDirectory(fixture.Title(L"Saves").c_str()) == 0);
    CHECK(access(fixture.Title(L"Saves").c_str()) == -1);
    CHECK(removeDirectory(fixture.Title(L"Data").c_str()) == -1); // Not empty.
    CHECK(lastErrno() == ENOTEMPTY);
    CHECK(fixture.TitleUnchanged());
}

TEST_CASE("Listings show overlay-only files and hide deleted ones", "[crtfiles]")
{
    const CrtFixture fixture;
    CHECK(fixture.Export<int(const wchar_t*, const char*, const wchar_t*)>("FopenWrite")(
              fixture.Title(L"Data\\added.txt").c_str(), "a", L"w") == 0);
    CHECK(fixture.Export<int(const wchar_t*)>("CrtRemove")(fixture.Title(L"Data\\gone.txt").c_str()) == 0);

    const std::set<std::wstring> expected = {L"added.txt", L"keep.txt", L"old.txt"};
    CHECK(fixture.Find(L"Data\\*.txt") == expected);
    CHECK(fixture.Export<int(const char*)>("NarrowCrtFindCount")(fixture.TitleAnsi("Data/*.txt").c_str()) == 3);
    CHECK(fixture.Iterate(L"Data") == expected); // std::filesystem::directory_iterator.
    CHECK(fixture.TitleUnchanged());
}

TEST_CASE("std::filesystem remove_all and rename use the layer", "[crtfiles]")
{
    const CrtFixture fixture;
    auto* const removeAll = fixture.Export<long long(const wchar_t*)>("FilesystemRemoveAll");
    CHECK(removeAll(fixture.Title(L"Tree").c_str()) == 4); // Two files, two directories.
    CHECK(fixture.Iterate(L"Tree") == std::set<std::wstring>{L"<error>"});
    CHECK_FALSE(fixture.Iterate(L".").contains(L"Tree"));

    auto* const rename = fixture.Export<int(const wchar_t*, const wchar_t*)>("FilesystemRename");
    CHECK(rename(fixture.Title(L"Data\\keep.txt").c_str(), fixture.Title(L"Data\\moved.txt").c_str()) == 0);
    CHECK(fixture.Read(L"Data\\moved.txt") == "keep");
    CHECK(fixture.Read(L"Data\\keep.txt") == "<missing>");

    CHECK(fixture.Export<int(const wchar_t*, const char*)>("CreateFile2Write")(
              fixture.Title(L"Data\\created2.txt").c_str(), "c2") == 0);
    CHECK(fixture.Read(L"Data\\created2.txt") == "c2");
    CHECK(fixture.TitleUnchanged());
}

TEST_CASE("C runtime file I/O outside the title folder is unchanged", "[crtfiles]")
{
    const CrtFixture fixture;
    const TempFolder elsewhere("CrtElsewhere");
    const auto path = (elsewhere.Path() / "outside.txt").wstring();
    CHECK(fixture.Export<int(const wchar_t*, const char*, const wchar_t*)>("FopenWrite")(path.c_str(), "out", L"w") ==
          0);
    CHECK(TempFolder::Read(elsewhere.Path() / "outside.txt") == "out");
    CHECK_FALSE(std::filesystem::exists(fixture.OverlayRoot() / "outside.txt"));
}

TEST_CASE("Copies read through the layer and write to the overlay", "[crtfiles]")
{
    const CrtFixture fixture;
    OpenEra::Kernel::ShimRegistry shims;
    REQUIRE(OpenEra::Loader::RegisterLoaderShims(shims));
    const auto copyFile2 = shims.Resolve("kernel32.dll", "CopyFile2");
    const auto copyFile = shims.Resolve("kernel32.dll", "CopyFileW");
    REQUIRE(copyFile2.has_value());
    REQUIRE(copyFile.has_value());
    using CopyFile2Function = long __stdcall(const wchar_t*, const wchar_t*, void*);
    using CopyFileFunction = int __stdcall(const wchar_t*, const wchar_t*, int);
    auto* const copy2 = OpenEra::Kernel::FromShimProc<CopyFile2Function>(copyFile2->Export->Proc);
    auto* const copy = OpenEra::Kernel::FromShimProc<CopyFileFunction>(copyFile->Export->Proc);

    CHECK(copy2(fixture.Title(L"Data\\old.txt").c_str(), fixture.Title(L"Data\\copy2.txt").c_str(), nullptr) >= 0);
    CHECK(fixture.Read(L"Data\\copy2.txt") == "old");
    CHECK(copy(fixture.Title(L"Data\\keep.txt").c_str(), fixture.Title(L"Data\\copy.txt").c_str(), 1) != 0);
    CHECK(fixture.Read(L"Data\\copy.txt") == "keep");
    CHECK(copy(fixture.Title(L"Data\\keep.txt").c_str(), fixture.Title(L"Data\\old.txt").c_str(), 1) == 0); // Exists.
    CHECK(copy(fixture.Title(L"Data\\keep.txt").c_str(), fixture.Title(L"Data\\old.txt").c_str(), 0) != 0); // Replaces.
    CHECK(fixture.Read(L"Data\\old.txt") == "keep");
    CHECK(fixture.TitleUnchanged());
}
