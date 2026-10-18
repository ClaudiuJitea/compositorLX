#pragma once
#include <QImage>
#include <QPointF>
struct ProgressiveBlurSettings {
    double from = 0, to = 20;
    QPointF start{0, 0}, end{0, 100};
    bool radial = false, inverted = false;
    QImage apply(const QImage &image) const;
};
