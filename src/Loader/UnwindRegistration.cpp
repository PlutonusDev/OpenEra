#include "UnwindRegistration.h"

#include <cstdint>
#include <span>

#include <Windows.h>

namespace OpenEra::Loader
{
void UnwindTableDeleter::operator()(void* table) const noexcept
{
    RtlDeleteFunctionTable(static_cast<PRUNTIME_FUNCTION>(table));
}

std::optional<UnwindTable> RegisterUnwindTable(const PeImage& image, ImageMemory& memory) noexcept
{
    const auto directory = image.Directory(DataDirectoryIndex::Exception);
    if (!directory || directory->Size == 0 || image.RuntimeFunctions.empty())
    {
        return UnwindTable{};
    }

    constexpr uint32_t kEntrySize = sizeof(RUNTIME_FUNCTION);
    const std::span<std::byte> bytes = memory.Bytes();
    const size_t offset = directory->VirtualAddress;
    const uint32_t count = directory->Size / kEntrySize;
    const size_t entries = count;
    const size_t size = entries * kEntrySize;
    if (count == 0 || offset > bytes.size() || size > bytes.size() - offset)
    {
        return std::nullopt;
    }

    // The table lives in the mapped image, so it stays valid while registered.
    void* const raw = bytes.subspan(offset, size).data();
    auto* const table = static_cast<PRUNTIME_FUNCTION>(raw);
    if (RtlAddFunctionTable(table, count, memory.Base()) == FALSE)
    {
        return std::nullopt;
    }
    return UnwindTable(table);
}
} // namespace OpenEra::Loader
