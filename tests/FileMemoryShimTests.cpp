// The file and memory shims, called through the kernel32 shim module the way a
// title's imports would bind to them.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include "Kernel/MemoryShims.h"
#include "Kernel/ShimModule.h"
#include "Kernel/TitleFileSystem.h"
#include "Loader/ModuleManager.h"
#include "Support/TempFolder.h"

using OpenEra::Kernel::FromShimProc;
using OpenEra::Kernel::SanitizeVirtualAllocFlags;
using OpenEra::Kernel::ShimRegistry;
using OpenEra::Kernel::TitleFileSystem;
using OpenEra::Tests::TempFolder;

namespace
{
using CreateFileFunction = HANDLE WINAPI(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetAttributesFunction = DWORD WINAPI(LPCWSTR);
using FindFirstFunction = HANDLE WINAPI(LPCWSTR, LPWIN32_FIND_DATAW);
using FindNextFunction = BOOL WINAPI(HANDLE, LPWIN32_FIND_DATAW);
using FindCloseFunction = BOOL WINAPI(HANDLE);
using CreateDirectoryFunction = BOOL WINAPI(LPCWSTR, LPSECURITY_ATTRIBUTES);
using DeleteFileFunction = BOOL WINAPI(LPCWSTR);
using VirtualAllocFunction = LPVOID WINAPI(LPVOID, SIZE_T, DWORD, DWORD);

class FileFixture
{
public:
    FileFixture()
        : m_title("FileShims")
        , m_overlay("FileShimsOverlay")
    {
        REQUIRE(OpenEra::Loader::RegisterLoaderShims(m_shims));
        m_title.Write("Media/asset.txt", "asset");
        m_title.Write("Media/old.txt", "old");
        m_files.emplace(m_title.Path(), m_overlay.Path());
        TitleFileSystem::SetActive(&*m_files);
    }

    ~FileFixture() { TitleFileSystem::SetActive(nullptr); }

    FileFixture(const FileFixture&) = delete;
    FileFixture& operator=(const FileFixture&) = delete;
    FileFixture(FileFixture&&) = delete;
    FileFixture& operator=(FileFixture&&) = delete;

    template <typename Function>
    [[nodiscard]] Function* Shim(std::string_view symbol) const
    {
        const auto resolved = m_shims.Resolve("kernel32.dll", symbol);
        REQUIRE(resolved.has_value());
        return FromShimProc<Function>(resolved->Export->Proc);
    }

    [[nodiscard]] std::wstring Title(std::wstring_view relative) const { return (m_title.Path() / relative).wstring(); }
    [[nodiscard]] const std::filesystem::path& TitleRoot() const noexcept { return m_title.Path(); }
    [[nodiscard]] const std::filesystem::path& OverlayRoot() const noexcept { return m_overlay.Path(); }

private:
    ShimRegistry m_shims;
    TempFolder m_title;
    TempFolder m_overlay;
    std::optional<TitleFileSystem> m_files;
};

std::wstring NameOf(const WIN32_FIND_DATAW& data)
{
    const auto& name = data.cFileName;
    return {std::begin(name), std::ranges::find(name, L'\0')};
}

std::string ReadAll(HANDLE file)
{
    std::string text(256, '\0');
    DWORD read = 0;
    REQUIRE(ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr));
    text.resize(read);
    return text;
}

void WriteAll(HANDLE file, std::string_view text)
{
    DWORD written = 0;
    REQUIRE(WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr));
    CHECK(written == text.size());
}

std::set<std::wstring> List(const FileFixture& fixture, std::wstring_view pattern)
{
    auto* const findFirst = fixture.Shim<FindFirstFunction>("FindFirstFileW");
    auto* const findNext = fixture.Shim<FindNextFunction>("FindNextFileW");
    auto* const findClose = fixture.Shim<FindCloseFunction>("FindClose");
    std::set<std::wstring> names;
    WIN32_FIND_DATAW data{};
    auto* const find = findFirst(fixture.Title(pattern).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return names;
    }
    do
    {
        names.insert(NameOf(data));
    } while (findNext(find, &data) != FALSE);
    CHECK(GetLastError() == ERROR_NO_MORE_FILES);
    CHECK(findClose(find));
    return names;
}
} // namespace

TEST_CASE("Title files read from the folder and write to the overlay", "[files]")
{
    const FileFixture fixture;
    auto* const createFile = fixture.Shim<CreateFileFunction>("CreateFileW");

    auto* const reader = createFile(fixture.Title(L"Media\\asset.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(reader != INVALID_HANDLE_VALUE);
    CHECK(ReadAll(reader) == "asset");
    CloseHandle(reader);

    auto* const writer = createFile(fixture.Title(L"Media\\asset.txt").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(writer != INVALID_HANDLE_VALUE);
    CHECK(ReadAll(writer) == "asset"); // Copied on first write.
    WriteAll(writer, "+more");
    CloseHandle(writer);

    CHECK(TempFolder::Read(fixture.TitleRoot() / "Media" / "asset.txt") == "asset");
    CHECK(TempFolder::Read(fixture.OverlayRoot() / "Media" / "asset.txt") == "asset+more");

    auto* const again = createFile(fixture.Title(L"MEDIA\\ASSET.TXT").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(again != INVALID_HANDLE_VALUE);
    CHECK(ReadAll(again) == "asset+more");
    CloseHandle(again);
}

TEST_CASE("New files, directories and deletes stay in the overlay", "[files]")
{
    const FileFixture fixture;
    auto* const createFile = fixture.Shim<CreateFileFunction>("CreateFileW");
    auto* const createDirectory = fixture.Shim<CreateDirectoryFunction>("CreateDirectoryW");
    auto* const deleteFile = fixture.Shim<DeleteFileFunction>("DeleteFileW");
    auto* const attributes = fixture.Shim<GetAttributesFunction>("GetFileAttributesW");

    CHECK(createDirectory(fixture.Title(L"Saves").c_str(), nullptr));
    CHECK_FALSE(createDirectory(fixture.Title(L"Saves").c_str(), nullptr));
    CHECK(GetLastError() == ERROR_ALREADY_EXISTS);
    CHECK_FALSE(createDirectory(fixture.Title(L"Missing\\Deeper").c_str(), nullptr));
    CHECK(GetLastError() == ERROR_PATH_NOT_FOUND);

    auto* const save = createFile(fixture.Title(L"Saves\\slot1.sav").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(save != INVALID_HANDLE_VALUE);
    WriteAll(save, "save");
    CloseHandle(save);
    CHECK((attributes(fixture.Title(L"Saves").c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0);
    CHECK(attributes(fixture.Title(L"Saves\\slot1.sav").c_str()) != INVALID_FILE_ATTRIBUTES);

    CHECK(deleteFile(fixture.Title(L"Media\\old.txt").c_str()));
    CHECK(attributes(fixture.Title(L"Media\\old.txt").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetLastError() == ERROR_FILE_NOT_FOUND);
    CHECK_FALSE(deleteFile(fixture.Title(L"Media\\old.txt").c_str()));
    CHECK(GetLastError() == ERROR_FILE_NOT_FOUND);
    auto* const gone = createFile(fixture.Title(L"Media\\old.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(gone == INVALID_HANDLE_VALUE);

    // The title folder itself is untouched.
    CHECK(std::filesystem::exists(fixture.TitleRoot() / "Media" / "old.txt"));
    CHECK_FALSE(std::filesystem::exists(fixture.TitleRoot() / "Saves"));
}

TEST_CASE("Directory listings merge the folder and the overlay", "[files]")
{
    const FileFixture fixture;
    auto* const createFile = fixture.Shim<CreateFileFunction>("CreateFileW");
    auto* const deleteFile = fixture.Shim<DeleteFileFunction>("DeleteFileW");

    auto* const added = createFile(fixture.Title(L"Media\\added.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(added != INVALID_HANDLE_VALUE);
    CloseHandle(added);
    CHECK(deleteFile(fixture.Title(L"Media\\old.txt").c_str()));

    const auto names = List(fixture, L"Media\\*");
    CHECK(names == std::set<std::wstring>{L".", L"..", L"asset.txt", L"added.txt"});
    CHECK(List(fixture, L"Media\\*.txt") == std::set<std::wstring>{L"asset.txt", L"added.txt"});
    CHECK(List(fixture, L"Media\\nothing*").empty());

    // OpenEra's own bookkeeping never shows up.
    const auto root = List(fixture, L"*");
    CHECK_FALSE(root.contains(L".openera-deleted"));
    CHECK(root.contains(L"Media"));
}

TEST_CASE("Paths outside the title folder pass through", "[files]")
{
    const FileFixture fixture;
    auto* const createFile = fixture.Shim<CreateFileFunction>("CreateFileW");
    const TempFolder elsewhere("FileShimsElsewhere");
    const auto path = (elsewhere.Path() / "outside.txt").wstring();
    auto* const file =
        createFile(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    CHECK(std::filesystem::exists(elsewhere.Path() / "outside.txt"));
    CHECK_FALSE(std::filesystem::exists(fixture.OverlayRoot() / "outside.txt"));
}

TEST_CASE("Undocumented VirtualAlloc bits are split off", "[memory]")
{
    const auto plain = SanitizeVirtualAllocFlags(MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(plain.AllocationType == (MEM_COMMIT | MEM_RESERVE));
    CHECK(plain.Protection == PAGE_READWRITE);
    CHECK(plain.UnknownAllocationType == 0);
    CHECK(plain.UnknownProtection == 0);

    const auto odd =
        SanitizeVirtualAllocFlags(MEM_COMMIT | 0x0000'4000U, PAGE_READWRITE | PAGE_WRITECOMBINE | 0x0001'0000U);
    CHECK(odd.AllocationType == MEM_COMMIT);
    CHECK(odd.UnknownAllocationType == 0x4000U);
    CHECK(odd.Protection == (PAGE_READWRITE | PAGE_WRITECOMBINE));
    CHECK(odd.UnknownProtection == 0x1'0000U);
}

TEST_CASE("VirtualAlloc succeeds for large pages and undocumented flags", "[memory]")
{
    ShimRegistry shims;
    REQUIRE(OpenEra::Loader::RegisterLoaderShims(shims));
    const auto resolved = shims.Resolve("api-ms-win-core-memory-l1-1-0.dll", "VirtualAlloc");
    REQUIRE(resolved.has_value());
    auto* const virtualAlloc = FromShimProc<VirtualAllocFunction>(resolved->Export->Proc);

    // Large pages need SeLockMemoryPrivilege on desktop; without it the shim uses normal pages.
    const SIZE_T largePage = GetLargePageMinimum();
    if (largePage != 0)
    {
        void* const memory =
            virtualAlloc(nullptr, largePage, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
        REQUIRE(memory != nullptr);
        CHECK(std::bit_cast<uintptr_t>(memory) % largePage == 0); // Still large-page aligned.
        CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    }

    void* const odd = virtualAlloc(nullptr, 0x1'0000, MEM_COMMIT | MEM_RESERVE | 0x4000U, PAGE_READWRITE);
    REQUIRE(odd != nullptr);
    MEMORY_BASIC_INFORMATION info{};
    REQUIRE(VirtualQuery(odd, &info, sizeof(info)) == sizeof(info));
    CHECK(info.Protect == PAGE_READWRITE);
    CHECK(VirtualFree(odd, 0, MEM_RELEASE));
}
