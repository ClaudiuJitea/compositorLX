#pragma once
#include <QPainterPath>
#include <QTransform>
#include <algorithm>
#include <cmath>

namespace compositor
{
// Upstream ShapeTool.swift: Figma's cubic–arc–cubic construction at 60% smoothing.
inline QPainterPath rectanglePath(const QRectF &rect, double cornerRadius, bool smooth = false)
{
    QPainterPath path;
    const double r = std::min({std::max(0.0, cornerRadius), rect.width() / 2, rect.height() / 2});
    if (r <= 0) {
        path.addRect(rect);
        return path;
    }
    const double s =
        smooth ? std::min(.6, std::max(0.0, std::min(rect.width(), rect.height()) / (2 * r) - 1)) : 0;
    if (s <= 0) {
        path.addRoundedRect(rect, r, r);
        return path;
    }
    const double reach = (1 + s) * r, angle = s * M_PI / 4, tangent = r * std::tan(angle / 2);
    const double c = tangent * std::cos(angle), d = tangent * std::sin(angle);
    const double arcLength = r * std::sqrt(2.0) * std::sin((1 - s) * M_PI / 4),
                 b = (reach - arcLength - c - d) / 3, a = 2 * b;
    const QPointF corners[] = {rect.topRight(), rect.bottomRight(), rect.bottomLeft(), rect.topLeft()};
    for (int i = 0; i < 4; ++i) {
        QTransform t;
        t.translate(corners[i].x(), corners[i].y());
        t.rotate(i * 90);
        const auto pt = [&](double x, double y) { return t.map(QPointF(x, y)); };
        if (i == 0)
            path.moveTo(pt(-reach, 0));
        else
            path.lineTo(pt(-reach, 0));
        path.cubicTo(pt(-reach + a, 0), pt(-reach + a + b, 0), pt(-reach + a + b + c, d));
        QPainterPath arc;
        arc.arcMoveTo(QRectF(-2 * r, 0, 2 * r, 2 * r), 90 - angle * 180 / M_PI);
        arc.arcTo(QRectF(-2 * r, 0, 2 * r, 2 * r), 90 - angle * 180 / M_PI, -90 + 2 * angle * 180 / M_PI);
        const QPainterPath mapped = t.map(arc);
        path.connectPath(mapped);
        path.cubicTo(pt(0, reach - a - b), pt(0, reach - a), pt(0, reach));
    }
    path.closeSubpath();
    return path;
}
} // namespace compositor
