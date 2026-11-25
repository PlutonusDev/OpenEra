#include "ShimGuard.h"

#include "Core/Logger.h"

namespace OpenEra::Kernel
{
void ReportShimException(std::string_view shimName, std::string_view what) noexcept
{
    try
    {
        Core::DefaultLogger().Error("Shim {} threw: {}", shimName, what);
    }
    catch (...)
    {
        // Logging is best effort at this boundary.
        return;
    }
}

void ReportShimException(std::string_view shimName, const std::exception& exception) noexcept
{
    try
    {
        ReportShimException(shimName, exception.what());
    }
    catch (...)
    {
        // what() is virtual; a misbehaving override must not escape either.
        return;
    }
}

void ReportPartialShim(StubKind kind, std::string_view module, std::string_view symbol, std::string_view note) noexcept
{
    try
    {
        DefaultStubRegistry().RecordHit(kind, module, symbol, note);
    }
    catch (...)
    {
        // DefaultStubRegistry's first-use construction failed; nothing to record into.
        return;
    }
}
} // namespace OpenEra::Kernel
