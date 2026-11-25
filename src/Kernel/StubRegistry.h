#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenEra::Core
{
class Logger;
} // namespace OpenEra::Core

namespace OpenEra::Kernel
{
enum class StubKind : uint8_t
{
    NtSyscall,
    EraApi,
    GameCoreApi,
    Win32Import,
};

[[nodiscard]] std::string_view ToString(StubKind kind) noexcept;

struct StubRecord
{
    StubKind Kind = StubKind::Win32Import;
    std::string Module;
    std::string Symbol;              // Empty when identified by ordinal.
    std::optional<uint32_t> Ordinal; // Export ordinal or syscall number.
    uint64_t HitCount = 0;
    std::string Note; // First non-empty note recorded.
};

// "module!symbol" or "module!#ordinal".
[[nodiscard]] std::string FormatStubId(const StubRecord& record);

// Tracks calls into unimplemented stubs. Thread-safe. The first hit of each
// stub is logged as a warning; repeats only increment its count.
class StubRegistry
{
public:
    explicit StubRegistry(Core::Logger& logger);

    // Returns the stub's new hit count, or 0 if it could not be recorded.
    uint64_t RecordHit(StubKind kind, std::string_view module, std::string_view symbol,
                       std::string_view note = {}) noexcept;
    uint64_t RecordHit(StubKind kind, std::string_view module, uint32_t ordinal, std::string_view note = {}) noexcept;

    [[nodiscard]] uint64_t GetHitCount(StubKind kind, std::string_view module, std::string_view symbol) const noexcept;
    [[nodiscard]] uint64_t GetHitCount(StubKind kind, std::string_view module, uint32_t ordinal) const noexcept;

    // Sorted by kind, module, then symbol or ordinal. Empty if allocation fails.
    [[nodiscard]] std::vector<StubRecord> GetAllRecords() const noexcept;
    [[nodiscard]] size_t UniqueCount() const noexcept;
    [[nodiscard]] uint64_t TotalHits() const noexcept;

    void Reset() noexcept;

    // Logs one line per stub, most-hit first.
    void LogSummary() const noexcept;

    // Operations that failed on allocation or locking errors.
    [[nodiscard]] uint64_t InternalErrors() const noexcept;

private:
    struct Key
    {
        StubKind Kind = StubKind::Win32Import;
        std::string Module;
        std::string Symbol;
        std::optional<uint32_t> Ordinal;

        auto operator<=>(const Key&) const = default;
    };

    uint64_t Record(const Key& key, std::string_view note);
    [[nodiscard]] uint64_t Lookup(const Key& key) const;
    [[nodiscard]] std::vector<StubRecord> Snapshot() const;
    void CountError() const noexcept;

    std::reference_wrapper<Core::Logger> m_logger;
    mutable std::mutex m_mutex;
    std::map<Key, StubRecord> m_records;
    uint64_t m_totalHits = 0;
    mutable std::atomic<uint64_t> m_internalErrors{0};
};

// Process-wide registry that reports through Core::DefaultLogger().
[[nodiscard]] StubRegistry& DefaultStubRegistry();
} // namespace OpenEra::Kernel
