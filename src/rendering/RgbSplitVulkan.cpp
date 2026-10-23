#include "rendering/RgbSplitVulkan.h"
#include "rendering/GpuOperations.h"
namespace compositor {
QImage tryRgbSplitVulkan(const QImage &image, const RgbSplitSettings &settings, unsigned edges,
                         RgbSplitExecution &execution) {
    GpuExecution report;
    QImage result = GpuOperations::rgbSplit(image, settings, edges, &report);
    execution.deviceName = report.device;
    execution.fallbackReason = report.reason;
    if (report.accelerated)
        execution.backend = RgbSplitBackend::Vulkan;
    return result;
}
} // namespace compositor
