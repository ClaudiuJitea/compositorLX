#pragma once
#include "rendering/RgbSplit.h"
namespace compositor {
// An empty image means the caller must use the CPU path. No window/surface is needed.
QImage tryRgbSplitVulkan(const QImage &image, const RgbSplitSettings &settings,
                        unsigned edges, RgbSplitExecution &execution);
}
