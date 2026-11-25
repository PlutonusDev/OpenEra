#include "StubRegistry.h"

#include <algorithm>
#include <format>
#include <utility>

#include "Core/Logger.h"

namespace OpenEra::Kernel
{
std::string_view ToString(StubKind kind) noexcept
{
    switch (kind)
    {
    case StubKind::NtSyscall:
        return "NtSyscall";
    case StubKind::EraApi:
        return "EraApi";
    case StubKind::GameCoreApi:
        return "GameCoreApi";
    case StubKind::Win32Import:
        return "Win32Import";
    }
    return "Unknown";
}

std::string FormatStubId(const StubRecord& record)
{
    if (record.Ordinal)
    {
        return std::format("{}!#{}", record.Module, *record.Ordinal);
    }
    return std::format("{}!{}", record.Module, record.Symbol);
}

StubRegistry::StubRegistry(Core::Logger& logger)
    : m_logger(logger)
{
}

uint64_t StubRegistry::RecordHit(StubKind kind, std::string_view module, std::string_view symbol,
                                 std::string_view note) noexcept
{
    try
    {
        return Record({.Kind = kind, .Module = std::string(module), .Symbol = std::string(symbol), .Ordinal = {}},
                      note);
    }
    catch (...)
    {
        CountError();
        return 0;
    }
}

uint64_t StubRegistry::RecordHit(StubKind kind, std::string_view module, uint32_t ordinal,
                                 std::string_view note) noexcept
{
    try
    {
        return Record({.Kind = kind, .Module = std::string(module), .Symbol = {}, .Ordinal = ordinal}, note);
    }
    catch (...)
    {
        CountError();
        return 0;
    }
}

uint64_t StubRegistry::Record(const Key& key, std::string_view note)
{
    std::string noteText(note);

    bool firstHit = false;
    uint64_t hitCount = 0;
    {
        // Anything that can throw (allocation) happens before the counters change.
        const std::lock_guard lock(m_mutex);
        auto [it, inserted] = m_records.try_emplace(key, StubRecord{.Kind = key.Kind,
                                                                    .Module = key.Module,
                                                                    .Symbol = key.Symbol,
                                                                    .Ordinal = key.Ordinal,
                                                                    .HitCount = 0,
                                                                    .Note = {}});
        auto& record = it->second;
        if (record.Note.empty() && !noteText.empty())
        {
            record.Note = std::move(noteText);
        }
        hitCount = ++record.HitCount;
        ++m_totalHits;
        firstHit = inserted;
    }

    // Logged outside the lock, once per stub.
    if (firstHit)
    {
        const StubRecord identity{.Kind = key.Kind,
                                  .Module = key.Module,
                                  .Symbol = key.Symbol,
                                  .Ordinal = key.Ordinal,
                                  .HitCount = 0,
                                  .Note = {}};
        if (note.empty())
        {
            m_logger.get().Warn("Unimplemented {} {}", ToString(key.Kind), FormatStubId(identity));
        }
        else
        {
            m_logger.get().Warn("Unimplemented {} {} ({})", ToString(key.Kind), FormatStubId(identity), note);
        }
    }
    return hitCount;
}

uint64_t StubRegistry::Lookup(const Key& key) const
{
    const std::lock_guard lock(m_mutex);
    const auto it = m_records.find(key);
    return it == m_records.end() ? 0 : it->second.HitCount;
}

uint64_t StubRegistry::GetHitCount(StubKind kind, std::string_view module, std::string_view symbol) const noexcept
{
    try
    {
        return Lookup({.Kind = kind, .Module = std::string(module), .Symbol = std::string(symbol), .Ordinal = {}});
    }
    catch (...)
    {
        CountError();
        return 0;
    }
}

uint64_t StubRegistry::GetHitCount(StubKind kind, std::string_view module, uint32_t ordinal) const noexcept
{
    try
    {
        return Lookup({.Kind = kind, .Module = std::string(module), .Symbol = {}, .Ordinal = ordinal});
    }
    catch (...)
    {
        CountError();
        return 0;
    }
}

std::vector<StubRecord> StubRegistry::Snapshot() const
{
    std::vector<StubRecord> records;
    const std::lock_guard lock(m_mutex);
    records.reserve(m_records.size());
    for (const auto& [key, record] : m_records)
    {
        records.push_back(record);
    }
    return records;
}

std::vector<StubRecord> StubRegistry::GetAllRecords() const noexcept
{
    try
    {
        return Snapshot();
    }
    catch (...)
    {
        CountError();
        return {};
    }
}

size_t StubRegistry::UniqueCount() const noexcept
{
    try
    {
        const std::lock_guard lock(m_mutex);
        return m_records.size();
    }
    catch (...)
    {
        CountError();
        return 0;
    }
}

uint64_t StubRegistry::TotalHits() const noexcept
{
    try
    {
        const std::lock_guard lock(m_mutex);
        return m_totalHits;
    }
    catch (...)
    {
        CountError();
        return 0;
    }
}

void StubRegistry::Reset() noexcept
{
    try
    {
        std::map<Key, StubRecord> released;
        const std::lock_guard lock(m_mutex);
        released.swap(m_records);
        m_totalHits = 0;
    }
    catch (...)
    {
        CountError();
    }
}

void StubRegistry::LogSummary() const noexcept
{
    try
    {
        auto records = Snapshot();
        auto& logger = m_logger.get();
        if (records.empty())
        {
            logger.Info("Stub summary: no unimplemented calls");
            return;
        }

        std::ranges::stable_sort(records, std::ranges::greater{}, &StubRecord::HitCount);
        uint64_t totalHits = 0;
        for (const auto& record : records)
        {
            totalHits += record.HitCount;
        }

        logger.Info("Stub summary: {} unimplemented stubs, {} hits", records.size(), totalHits);
        for (const auto& record : records)
        {
            logger.Info("  {:>8}  {:<11}  {}{}{}", record.HitCount, ToString(record.Kind), FormatStubId(record),
                        record.Note.empty() ? "" : " - ", record.Note);
        }
    }
    catch (...)
    {
        CountError();
    }
}

uint64_t StubRegistry::InternalErrors() const noexcept
{
    return m_internalErrors.load(std::memory_order_relaxed);
}

void StubRegistry::CountError() const noexcept
{
    m_internalErrors.fetch_add(1, std::memory_order_relaxed);
}

StubRegistry& DefaultStubRegistry()
{
    static StubRegistry registry(Core::DefaultLogger());
    return registry;
}
} // namespace OpenEra::Kernel
