#pragma once
#include <QRectF>
#include <QSizeF>
#include <QVector>

namespace compositor {
// View › Show › Distances (mac 8fb163e LayerDistances.swift): the pixel gaps the Move tool shows while a layer is
// dragged, and with Ctrl (macOS's Command) held over another layer.
struct DistanceLine {
    // Along x (`horizontal`) or y from `from` to `to`, at `across` on the other axis, in document pixels.
    bool horizontal = true;
    double from = 0, to = 0, across = 0;
    [[nodiscard]] double length() const { return to - from; }
    bool operator==(const DistanceLine &) const = default;
};

namespace LayerDistances {
// While `box` moves: from each of its sides to the nearest edge facing it, of a layer level with it or the canvas.
QVector<DistanceLine> moving(const QRectF &box, const QVector<QRectF> &others, const QSizeF &canvas);
// Between the selected `box` and `other`: the gap across each axis they're apart on; where they overlap, from each side
// of one to the same side of the other.
QVector<DistanceLine> between(const QRectF &box, const QRectF &other);
} // namespace LayerDistances
} // namespace compositor
