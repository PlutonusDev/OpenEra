#include "Core/ByteReader.h"

namespace OpenEra::Core
{
std::optional<uint8_t> ByteReader::ReadU8() noexcept
{
    return ReadStruct<uint8_t>();
}

std::optional<uint16_t> ByteReader::ReadU16() noexcept
{
    return ReadStruct<uint16_t>();
}

std::optional<uint32_t> ByteReader::ReadU32() noexcept
{
    return ReadStruct<uint32_t>();
}

std::optional<uint64_t> ByteReader::ReadU64() noexcept
{
    return ReadStruct<uint64_t>();
}

std::optional<std::span<const std::byte>> ByteReader::ReadBytes(size_t count) noexcept
{
    if (!CanRead(count))
    {
        return std::nullopt;
    }

    const auto bytes = m_data.subspan(m_offset, count);
    m_offset += count;
    return bytes;
}

bool ByteReader::Skip(size_t count) noexcept
{
    if (!CanRead(count))
    {
        return false;
    }

    m_offset += count;
    return true;
}

bool ByteReader::Seek(size_t offset) noexcept
{
    if (offset > m_data.size())
    {
        return false;
    }

    m_offset = offset;
    return true;
}
} // namespace OpenEra::Core
