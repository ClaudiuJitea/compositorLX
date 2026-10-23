#include "rendering/RendererSelfCheck.h"
#include "rendering/DeepImage.h"
#include "rendering/GpuRenderer.h"
#include "rendering/RasterOperations.h"
#include "rendering/SubjectRemoval.h"
#include <QColor>
#include <cmath>

namespace compositor {
QJsonObject rendererSelfCheck() {
    QImage source(65, 49, QImage::Format_RGBA8888_Premultiplied);
    source.fill(QColor(120, 70, 30, 128));
    GpuGraph graph;
    int at = graph.source(source);
    GpuParameters params{};
    params[GpuParam::Exposure::multiplier] = 1.5;
    params[GpuParam::Exposure::inverseGamma] = 1;
    at = graph.pass(GpuOp::Exposure, at, -1, params, unsigned(GpuQuantization::Unorm8));
    at = graph.gaussian(at, 3, true);
    GpuExecution report;
    QImage gpu = graph.execute(at, source.format(), RenderBackend::Vulkan, &report);
    QImage cpu;
    {
        GpuRenderer::CpuScope scope;
        cpu = RasterOperations::gaussianBlur(RasterOperations::exposure(source, std::log2(1.5), 0, 1), 3);
    }
    if (gpu.isNull())
        gpu = cpu;
    const QImage a = DeepImage::floating(cpu), b = DeepImage::floating(gpu);
    double difference = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto *p = reinterpret_cast<const float *>(a.constScanLine(y)),
                   *q = reinterpret_cast<const float *>(b.constScanLine(y));
        for (int i = 0; i < a.width() * 4; ++i)
            difference = std::max(difference, double(std::abs(p[i] - q[i])));
    }
    const auto info = vulkanDeviceInfo(true);
    return QJsonObject{{"valid", !gpu.isNull() && difference <= 1.01 / 255},
                       {"backend", report.accelerated ? "Vulkan" : "CPU"},
                       {"device", report.device},
                       {"deviceType", info.type},
                       {"driver", info.driver},
                       {"vulkanApi", info.apiVersion},
                       {"fallbackReason", report.reason},
                       {"maximumChannelError", difference},
                       {"dispatches", report.dispatches},
                       {"milliseconds", report.milliseconds},
                       {"uploadedBytes", qint64(report.uploadedBytes)},
                       {"downloadedBytes", qint64(report.downloadedBytes)},
                       {"aiProviders", SubjectRemoval::executionProviders()}};
}
} // namespace compositor
