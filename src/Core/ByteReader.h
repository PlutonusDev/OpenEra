#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace OpenEra::Core
{
// Sequential, bounds-checked reader over a borrowed byte buffer.
//
// - Never throws. Every read reports failure through std::optional or bool.
// - A failed read does not move the read position.
// - Multi-byte values are read in the host byte order, which is little-endian
//   on every supported target (x64), matching Xbox on-disk formats.
// - The reader does not own the buffer; it must outlive the reader.
class ByteReader
{
public:
    constexpr ByteReader() noexcept = default;

    constexpr explicit ByteReader(std::span<const std::byte> data) noexcept
        : m_data(data)
    {
    }

    [[nodiscard]] std::optional<uint8_t> ReadU8() noexcept;
    [[nodiscard]] std::optional<uint16_t> ReadU16() noexcept;
    [[nodiscard]] std::optional<uint32_t> ReadU32() noexcept;
    [[nodiscard]] std::optional<uint64_t> ReadU64() noexcept;

    // Returns a view of the next `count` bytes (not a copy) and advances past them.
    [[nodiscard]] std::optional<std::span<const std::byte>> ReadBytes(size_t count) noexcept;

    // Advances by `count` bytes. Returns false, without moving, if that passes the end.
    [[nodiscard]] bool Skip(size_t count) noexcept;

    // Moves to an absolute offset. Seeking to Size() (the end) is allowed.
    [[nodiscard]] bool Seek(size_t offset) noexcept;

    // Copies the next sizeof(T) bytes into a T. T must be trivially copyable;
    // ABI structs should be packed and size-checked with static_assert.
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] std::optional<T> ReadStruct() noexcept
    {
        if (!CanRead(sizeof(T)))
        {
            return std::nullopt;
        }

        std::array<std::byte, sizeof(T)> raw{};
        std::ranges::copy(m_data.subspan(m_offset, sizeof(T)), raw.begin());
        m_offset += sizeof(T);
        return std::bit_cast<T>(raw);
    }

    [[nodiscard]] constexpr size_t Offset() const noexcept { return m_offset; }

    [[nodiscard]] constexpr size_t Remaining() const noexcept { return m_data.size() - m_offset; }

    [[nodiscard]] constexpr size_t Size() const noexcept { return m_data.size(); }

private:
    // Written as a comparison against Remaining() so a huge `count` cannot overflow.
    [[nodiscard]] constexpr bool CanRead(size_t count) const noexcept { return count <= Remaining(); }

    std::span<const std::byte> m_data;
    size_t m_offset = 0;
};

static_assert(std::endian::native == std::endian::little, "ByteReader assumes a little-endian host.");
} // namespace OpenEra::Core
