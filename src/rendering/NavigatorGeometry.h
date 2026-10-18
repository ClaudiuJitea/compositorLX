#pragma once
#include <QRectF>
#include <algorithm>
namespace compositor
{
struct NavigatorGeometry {
    QSizeF documentSize, box;
    QRectF imageRect() const
    {
        if (documentSize.width() <= 0 || documentSize.height() <= 0 || box.width() <= 0 || box.height() <= 0)
            return {};
        const double scale =
            std::min(box.width() / documentSize.width(), box.height() / documentSize.height());
        const QSizeF size = documentSize * scale;
        return QRectF(QPointF((box.width() - size.width()) / 2, (box.height() - size.height()) / 2), size);
    }
    QRectF thumbnailRect(const QRectF &documentRect) const
    {
        const auto image = imageRect();
        const double scale = documentSize.width() > 0 ? image.width() / documentSize.width() : 0;
        return QRectF(image.topLeft() + documentRect.topLeft() * scale, documentRect.size() * scale);
    }
    QPointF documentPoint(const QPointF &point) const
    {
        const auto image = imageRect();
        const double scale = documentSize.width() > 0 ? image.width() / documentSize.width() : 0;
        if (scale <= 0)
            return {};
        return QPointF(std::clamp((point.x() - image.left()) / scale, 0.0, documentSize.width()),
                       std::clamp((point.y() - image.top()) / scale, 0.0, documentSize.height()));
    }
};
} // namespace compositor
