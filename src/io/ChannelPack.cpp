#include "io/ChannelPack.h"
#include "rendering/LayerRenderer.h"
#include "rendering/DeepImage.h"
#include <QBuffer>
#include <QImageWriter>
#include <QSet>
namespace compositor {
QImage ChannelPack::render(const Document &document, const std::array<ChannelSource, 4> &sources, const QSize &size)
{
    if (size.isEmpty() || size.width() > DocumentLimits::maxSide || size.height() > DocumentLimits::maxSide || qint64(size.width()) * size.height() > DocumentLimits::maxSurfacePixels) return {};
    QImage output(size, QImage::Format_RGBA8888); if (output.isNull()) return {};
    QHash<QUuid, std::optional<QUuid>> parents;
    for (const auto &layer : document.layers) parents.insert(layer.id, layer.parentId);
    const auto inside = [&](QUuid id, QUuid root) {
        QSet<QUuid> seen;
        while (parents.value(id) && !seen.contains(id)) { seen.insert(id); id = *parents.value(id); if (id == root) return true; }
        return false;
    };
    for (int c = 0; c < 4; ++c) {
        QImage plane;
        if (!sources[c].layer.isNull()) {
            if (!parents.contains(sources[c].layer)) return {};
            Document isolated = document;
            for (auto &layer : isolated.layers) {
                if (layer.id == sources[c].layer || inside(sources[c].layer, layer.id)) layer.visible = true;
                else if (!inside(layer.id, sources[c].layer)) layer.visible = false;
            }
            plane = DeepImage::dithered(LayerRenderer::flattened(isolated)).scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
            if (plane.isNull()) return {};
        }
        for (int y = 0; y < size.height(); ++y) {
            auto *out = output.scanLine(y);
            const auto *in = plane.isNull() ? nullptr : plane.constScanLine(y);
            for (int x = 0; x < size.width(); ++x) out[x * 4 + c] = in ? uchar(std::clamp(qRound(.2126 * in[x * 4] + .7152 * in[x * 4 + 1] + .0722 * in[x * 4 + 2]), 0, 255)) : sources[c].white ? 255 : 0;
        }
    }
    return output;
}
QByteArray ChannelPack::png(const QImage &image, double resolution)
{
    if (image.format() != QImage::Format_RGBA8888) return {};
    QImage output = image; output.setDotsPerMeterX(qRound(resolution / .0254)); output.setDotsPerMeterY(qRound(resolution / .0254));
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "PNG");
    return writer.write(output) ? bytes : QByteArray();
}
}
