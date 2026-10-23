#pragma once
#include <QImage>
#include <QJsonObject>
#include <QPointF>

namespace compositor {
enum class RgbSplitBackend { Auto, Cpu, Vulkan };
struct RgbSplitExecution {
    RgbSplitBackend backend = RgbSplitBackend::Cpu;
    QString deviceName;
    QString fallbackReason;
};
struct RgbSplitSettings {
    double distance = 10, blur = 0, angle = 0, reach = 1000;
    bool radial = false;
    QPointF center;
    QImage apply(const QImage &image, unsigned extendedEdges = 0,
                 RgbSplitBackend backend = RgbSplitBackend::Auto, RgbSplitExecution *execution = nullptr) const;
    QImage applyCpu(const QImage &image, unsigned extendedEdges = 0) const;
};
// Filter-layer positions are in document pixels, like guides and the filter's on-canvas handles.
QImage applyFilterLayer(const QImage &image, const QJsonObject &adjustment,
                        const QPointF &origin = {}, double unitsPerPixel = 1);
}
