#pragma once

#include <QtGlobal>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace compositor {

/// The size and memory ceilings a document is held to (mac Document/DocumentLimits.swift, commits 7dadc88 and
/// bc865fb). "How large may one surface be" and "how much raster may a whole document hold across its layers" are
/// separate budgets: a 58-megapixel print banner with 29 layers is an ordinary Photoshop document.
namespace DocumentLimits {

/// Longest side, in pixels, of any canvas, layer, mask or generated surface.
inline constexpr int maxSide = 30000;

/// Largest single surface: a canvas, an export, a filter target, an adjustment or mask render.
inline constexpr qint64 maxSurfacePixels = 200000000LL;

/// The machine's physical memory in bytes (0 when it cannot be read).
inline qint64 physicalMemoryBytes()
{
#if defined(Q_OS_UNIX) && defined(_SC_PHYS_PAGES) && defined(_SC_PAGE_SIZE)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) return qint64(pages) * qint64(pageSize);
#endif
    return 0;
}

/// The pure form of `documentPixelBudget`: a quarter of the memory at 4 bytes a pixel (physicalMemory / 16), never
/// less than one surface and never more than 800 MP.
inline constexpr qint64 pixelBudgetForMemory(qint64 physicalMemory)
{
    const qint64 scaled = physicalMemory / 16;
    const qint64 floorValue = scaled > maxSurfacePixels ? scaled : maxSurfacePixels;
    return floorValue < 800000000LL ? floorValue : 800000000LL;
}

/// Total imported raster one document may hold, summed across every layer and (separately) every mask.
inline qint64 documentPixelBudget()
{
    static const qint64 budget = pixelBudgetForMemory(physicalMemoryBytes());
    return budget;
}

inline int documentBudgetMegapixels() { return int(documentPixelBudget() / 1000000); }
inline int maxSurfaceMegapixels() { return int(maxSurfacePixels / 1000000); }

} // namespace DocumentLimits
} // namespace compositor
