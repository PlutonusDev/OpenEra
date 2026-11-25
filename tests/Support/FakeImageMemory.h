#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "Loader/ImageMemory.h"

namespace OpenEra::Tests
{
struct ProtectCall
{
    size_t Offset = 0;
    size_t Size = 0;
    Loader::PageProtection Protection = Loader::PageProtection::NoAccess;

    bool operator==(const ProtectCall&) const = default;
};

// Heap-backed image memory at any reported base; records protection changes.
// Not executable: use CreateVirtualImageAllocator for code that runs.
class FakeMemory final : public Loader::ImageMemory
{
public:
    FakeMemory(uint64_t base, size_t size, std::vector<ProtectCall>* calls, bool failProtect)
        : m_base(base)
        , m_bytes(size)
        , m_calls(calls)
        , m_failProtect(failProtect)
    {
    }

    [[nodiscard]] uint64_t Base() const noexcept override { return m_base; }
    [[nodiscard]] std::span<std::byte> Bytes() noexcept override { return m_bytes; }

    [[nodiscard]] bool Protect(size_t offset, size_t size, Loader::PageProtection protection) noexcept override
    {
        try
        {
            m_calls->push_back({.Offset = offset, .Size = size, .Protection = protection});
        }
        catch (...)
        {
            return false;
        }
        return !m_failProtect;
    }

private:
    uint64_t m_base;
    std::vector<std::byte> m_bytes;
    std::vector<ProtectCall>* m_calls;
    bool m_failProtect;
};

class FakeAllocator final : public Loader::ImageAllocator
{
public:
    std::optional<uint64_t> ForcedBase;
    bool Fail = false;
    bool FailProtect = false;
    std::vector<ProtectCall> Calls;
    uint64_t RequestedBase = 0;
    size_t RequestedSize = 0;

    [[nodiscard]] std::unique_ptr<Loader::ImageMemory> Allocate(uint64_t preferredBase, size_t size) noexcept override
    {
        RequestedBase = preferredBase;
        RequestedSize = size;
        if (Fail)
        {
            return nullptr;
        }
        try
        {
            return std::make_unique<FakeMemory>(ForcedBase.value_or(preferredBase), size, &Calls, FailProtect);
        }
        catch (...)
        {
            return nullptr;
        }
    }
};
} // namespace OpenEra::Tests
