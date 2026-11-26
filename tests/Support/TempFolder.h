#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace OpenEra::Tests
{
// A fresh folder under the temp directory, removed afterwards.
class TempFolder
{
public:
    explicit TempFolder(std::string_view name)
        : m_path(std::filesystem::temp_directory_path() / "OpenEraTests" /
                 (std::string(name) + "-" + std::to_string(NextId())))
    {
        std::filesystem::remove_all(m_path);
        std::filesystem::create_directories(m_path);
    }

    // Best-effort cleanup.
    ~TempFolder() // NOLINT(bugprone-exception-escape)
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    TempFolder(TempFolder&&) = delete;
    TempFolder& operator=(TempFolder&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

    void Write(const std::filesystem::path& relative, std::string_view text) const
    {
        std::filesystem::create_directories((m_path / relative).parent_path());
        std::ofstream(m_path / relative, std::ios::binary) << text;
    }

    [[nodiscard]] static std::string Read(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

private:
    static unsigned NextId() noexcept
    {
        static std::atomic<unsigned> next{0};
        return ++next;
    }

    std::filesystem::path m_path;
};
} // namespace OpenEra::Tests
