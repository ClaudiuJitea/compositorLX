#pragma once
#include <QPainter>
#include <QLineF>
#include <cmath>
namespace compositor {
inline void drawShapeLine(QPainter &p, QPointF start, QPointF end, double width, const QColor &color,
                          const QString &style = "Solid", const QString &cap = "Round")
{
    const double length = QLineF(start, end).length();
    const QPointF direction = length > 0 ? (end - start) / length : QPointF(1, 0);
    const auto along = [&](double d) { return start + direction * (d - width / 2); };
    QVector<qreal> pattern;
    if (style == "Dotted") pattern = {1, 1};
    else if (style == "Dashed") pattern = {3, 2};
    else if (style == "Long Dash") pattern = {7, 2};
    p.save(); p.setBrush(color);
    if (cap == "Square") {
        QPen pen(color, width, Qt::SolidLine, Qt::FlatCap); if (!pattern.isEmpty()) pen.setDashPattern(pattern);
        p.setPen(pen); p.drawLine(along(0), along(length + width));
    } else {
        p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
        if (pattern.isEmpty()) {
            if (length > 0) p.drawLine(start, end);
            else { p.setPen(Qt::NoPen); p.drawEllipse(start, width / 2, width / 2); }
        } else for (double at = 0; at < length + width; at += (pattern[0] + pattern[1]) * width) {
            const double stop = std::min(length + width, at + pattern[0] * width);
            const double size = stop - at;
            if (size <= width) { p.setPen(Qt::NoPen); p.drawEllipse(along(at + size / 2), size / 2, width / 2); }
            else { p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap)); p.drawLine(along(at + width / 2), along(stop - width / 2)); }
        }
    }
    p.restore();
}
}
