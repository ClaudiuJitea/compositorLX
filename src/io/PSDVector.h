#pragma once

#include "core/Document.h"

#include <QByteArray>
#include <QImage>
#include <QMap>
#include <QPainterPath>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <optional>

namespace compositor {

// Rasterizes Photoshop vector masks ("vmsk"/"vsms") and maps fill rectangles/ellipses onto live shape layers
// (macOS PSDVector.swift; Adobe's 2019 Photoshop File Formats Specification: vmsk, vogk, SoCo, vstk).
struct PSDVectorRaster {
    QImage image;
    QRectF bounds;
};

struct PSDVectorLive {
    LayerShapeStyle style;
    QRectF bounds;
    QImage image;
    QStringList notes;
};

namespace PSDVector {
// `tooLarge` is set when a size or stroke width is beyond the limits (the import is refused, as on macOS).
std::optional<PSDVectorLive> live(const QMap<QString, QByteArray> &extra, const QSizeF &canvas, qint64 remainingPixels, bool *tooLarge);
std::optional<PSDVectorRaster> raster(const QMap<QString, QByteArray> &extra, const QSizeF &canvas, qint64 remainingPixels, bool *tooLarge);
std::optional<QPainterPath> path(const QByteArray &data, const QSizeF &canvas);
// The shape layer's raster: a filled rectangle (optionally rounded) or ellipse filling `size`.
QImage shapeImage(ShapeKind kind, const QSize &size, const QColor &color, double cornerRadius);
} // namespace PSDVector

} // namespace compositor
