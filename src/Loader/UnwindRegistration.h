#pragma once

#include <memory>
#include <optional>

#include "Loader/ImageMemory.h"
#include "Loader/PeReader.h"

namespace OpenEra::Loader
{
struct UnwindTableDeleter
{
    void operator()(void* table) const noexcept;
};

// An image's function table, registered with the OS while this is alive.
using UnwindTable = std::unique_ptr<void, UnwindTableDeleter>;

// Registers the image's exception directory (.pdata) so exceptions can unwind
// through it. Holds no table if the image has none; nullopt if registration failed.
[[nodiscard]] std::optional<UnwindTable> RegisterUnwindTable(const PeImage& image, ImageMemory& memory) noexcept;
} // namespace OpenEra::Loader
