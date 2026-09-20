#pragma once

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>

namespace compositor {

// Render vector geometry at the requested size instead of scaling a tiny bitmap.
class EditorIconEngine final : public QIconEngine {
public:
    explicit EditorIconEngine(int kind) : kind_(kind) {}
    QIconEngine *clone() const override { return new EditorIconEngine(kind_); }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap result(size); result.fill(Qt::transparent);
        QPainter painter(&result); paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
    {
        QPainter &p = *painter;
        p.save();
        const qreal side = std::min(rect.width(), rect.height());
        p.translate(rect.x() + (rect.width() - side) / 2, rect.y() + (rect.height() - side) / 2);
        p.scale(side / 24, side / 24);
        p.translate(12, 12); p.scale(1.15, 1.15); p.translate(-12, -12);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor ink = mode == QIcon::Disabled ? QColor(108, 113, 123)
            : state == QIcon::On ? QColor(159, 207, 255)
            : mode == QIcon::Active ? QColor(255, 255, 255) : QColor(222, 228, 237);
        p.setPen(QPen(ink, 1.25, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        const int kind = kind_;
        switch (kind) {
        case 0: // Move
            p.drawLine(QPointF(12, 4.5), QPointF(12, 19.5));
            p.drawLine(QPointF(4.5, 12), QPointF(19.5, 12));
            p.drawPolyline(QPolygonF{QPointF(9.5, 7), QPointF(12, 4.5), QPointF(14.5, 7)});
            p.drawPolyline(QPolygonF{QPointF(9.5, 17), QPointF(12, 19.5), QPointF(14.5, 17)});
            p.drawPolyline(QPolygonF{QPointF(7, 9.5), QPointF(4.5, 12), QPointF(7, 14.5)});
            p.drawPolyline(QPolygonF{QPointF(17, 9.5), QPointF(19.5, 12), QPointF(17, 14.5)}); break;
        case 1: { // Marquee
            QPen dashed(ink, 1.25, Qt::DashLine, Qt::FlatCap, Qt::MiterJoin); dashed.setDashPattern({2, 1.7}); p.setPen(dashed);
            p.drawRect(QRectF(5, 5.5, 14, 13)); break;
        }
        case 2: { // Lasso
            QPainterPath path; path.moveTo(5.5, 10.0); path.cubicTo(6.3, 6.1, 11.0, 4.9, 15.6, 6.5);
            path.cubicTo(19.4, 7.9, 19.5, 11.4, 16.4, 13.3); path.cubicTo(13.2, 15.3, 7.2, 14.5, 5.8, 11.2); path.closeSubpath();
            p.drawPath(path); p.drawLine(QPointF(11.2, 14.5), QPointF(14.8, 18.3)); p.drawEllipse(QPointF(15.4, 18.8), .65, .65); break;
        }
        case 3: // Magic wand
            p.drawLine(QPointF(7.0, 17.0), QPointF(15.8, 8.2)); p.drawLine(QPointF(8.2, 18.2), QPointF(17.0, 9.4));
            p.drawLine(QPointF(17.8, 4.8), QPointF(17.8, 8.0)); p.drawLine(QPointF(16.2, 6.4), QPointF(19.4, 6.4));
            p.drawLine(QPointF(7.0, 8.0), QPointF(7.0, 10.4)); p.drawLine(QPointF(5.8, 9.2), QPointF(8.2, 9.2));
            p.drawLine(QPointF(17.8, 15.0), QPointF(17.8, 17.4)); p.drawLine(QPointF(16.6, 16.2), QPointF(19.0, 16.2)); break;
        case 4: // Crop
            p.drawLine(QPointF(7.5, 4.8), QPointF(7.5, 16.5)); p.drawLine(QPointF(7.5, 16.5), QPointF(19.0, 16.5));
            p.drawLine(QPointF(4.8, 7.5), QPointF(16.5, 7.5)); p.drawLine(QPointF(16.5, 7.5), QPointF(16.5, 19.0)); break;
        case 5: { // Brush
            QPainterPath tip; tip.moveTo(6.2, 17.8); tip.cubicTo(6.6, 14.8, 8.2, 14.0, 10.1, 14.4);
            tip.lineTo(12.1, 16.3); tip.cubicTo(11.7, 18.3, 9.3, 19.3, 6.2, 17.8); p.drawPath(tip);
            QPainterPath handle; handle.moveTo(10.0, 14.4); handle.lineTo(16.9, 6.4); handle.cubicTo(18.7, 4.4, 21.0, 6.7, 19.0, 8.5); handle.lineTo(12.0, 16.4); p.drawPath(handle); break;
        }
        case 6: { // Healing
            p.save(); p.translate(12.0, 12.0); p.rotate(-43);
            p.drawRoundedRect(QRectF(-7.0, -3.0, 14.0, 6.0), 3.0, 3.0);
            p.drawLine(QPointF(-2.5, -2.8), QPointF(-2.5, 2.8));
            p.drawLine(QPointF(2.5, -2.8), QPointF(2.5, 2.8));
            p.drawPoint(QPointF(0, -1)); p.drawPoint(QPointF(0, 1)); p.restore(); break;
        }
        case 7: // Clone stamp
            p.drawEllipse(QRectF(10.0, 5.0, 4.0, 4.0)); p.drawLine(QPointF(12.0, 9.0), QPointF(12.0, 14.0));
            p.drawLine(QPointF(8.0, 14.0), QPointF(16.0, 14.0)); p.drawRoundedRect(QRectF(7.0, 14.0, 10.0, 3.0), 1.0, 1.0); p.drawLine(QPointF(6.0, 19.0), QPointF(18.0, 19.0)); break;
        case 8: { // Blur
            QPainterPath drop; drop.moveTo(12.0, 4.8); drop.cubicTo(12.0, 4.8, 7.5, 11.7, 7.5, 15.0);
            drop.cubicTo(7.5, 21.0, 16.5, 21.0, 16.5, 15.0); drop.cubicTo(16.5, 11.7, 12.0, 4.8, 12.0, 4.8); p.drawPath(drop); break;
        }
        case 9: { // Gradient
            QLinearGradient fade(5, 12, 19, 12);
            QColor transparent = ink; transparent.setAlpha(20);
            fade.setColorAt(0, ink); fade.setColorAt(1, transparent);
            p.setBrush(fade); p.drawRoundedRect(QRectF(5, 5.5, 14, 13), 1.5, 1.5); break;
        }
        case 10: // Shape
            p.drawEllipse(QRectF(5.0, 5.0, 10.0, 10.0)); p.drawRoundedRect(QRectF(10.0, 10.0, 9.0, 9.0), 1.3, 1.3); break;
        case 11: { // Eyedropper: distinct rubber bulb and glass pipette
            QPainterPath tube; tube.moveTo(13, 8); tube.lineTo(5.5, 15.5); tube.lineTo(5, 19);
            tube.lineTo(8.5, 18.5); tube.lineTo(16, 11); p.drawPath(tube);
            p.drawLine(QPointF(11.5, 6.5), QPointF(17.5, 12.5));
            QPainterPath bulb; bulb.moveTo(13, 8); bulb.lineTo(15.5, 5.5);
            bulb.cubicTo(18.5, 2.5, 21.5, 5.5, 18.5, 8.5); bulb.lineTo(16, 11);
            p.setBrush(ink); p.drawPath(bulb); break;
        }
        case 12: { // Hand
            QPainterPath hand; hand.moveTo(8.0, 18.0); hand.lineTo(5.7, 12.8); hand.cubicTo(5.0, 11.2, 6.8, 10.2, 7.8, 11.7);
            hand.lineTo(9.4, 13.7); hand.lineTo(7.8, 7.7); hand.cubicTo(7.4, 6.1, 9.7, 5.7, 10.0, 7.3); hand.lineTo(11.2, 11.6);
            hand.lineTo(10.4, 6.3); hand.cubicTo(10.2, 4.7, 12.5, 4.5, 12.8, 6.1); hand.lineTo(13.6, 11.2); hand.lineTo(13.1, 7.0);
            hand.cubicTo(12.9, 5.5, 15.1, 5.4, 15.4, 6.9); hand.lineTo(16.0, 11.8); hand.lineTo(16.0, 8.7); hand.cubicTo(15.9, 7.2, 18.0, 7.2, 18.2, 8.7);
            hand.lineTo(18.6, 13.4); hand.cubicTo(18.9, 17.3, 16.6, 19.2, 12.8, 19.4); hand.cubicTo(10.6, 19.5, 9.1, 18.9, 8.0, 18.0); p.drawPath(hand); break;
        }
        case 13: // Zoom out
            p.drawEllipse(QRectF(6.0, 5.5, 10.5, 10.5)); p.drawLine(QPointF(15.0, 14.5), QPointF(19.2, 18.7)); p.drawLine(QPointF(8.7, 10.8), QPointF(13.8, 10.8)); break;
        case 15: // Text
            p.setPen(QPen(ink, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawLine(QPointF(6.0, 6.5), QPointF(18.0, 6.5)); p.drawLine(QPointF(12.0, 6.5), QPointF(12.0, 18.0));
            p.drawLine(QPointF(8.8, 18.0), QPointF(15.2, 18.0)); break;
        case 16: { // Linked dimensions
            p.save(); p.translate(12.0, 12.0); p.rotate(-38.0);
            p.drawRoundedRect(QRectF(-8.0, -3.2, 9.5, 6.4), 3.2, 3.2);
            p.drawRoundedRect(QRectF(-1.5, -3.2, 9.5, 6.4), 3.2, 3.2);
            p.drawLine(QPointF(-3.1, 0.0), QPointF(3.1, 0.0));
            p.restore(); break;
        }
        case 17: { // New layer
            QPainterPath page; page.moveTo(13, 4.5); page.lineTo(6, 4.5); page.lineTo(6, 19.5);
            page.lineTo(18, 19.5); page.lineTo(18, 9.5); page.lineTo(13, 4.5);
            page.lineTo(13, 9.5); page.lineTo(18, 9.5); p.drawPath(page);
            p.drawLine(QPointF(9, 14), QPointF(15, 14)); p.drawLine(QPointF(12, 11), QPointF(12, 17)); break;
        }
        case 18: { // Layer group
            QPainterPath folder; folder.moveTo(4.5, 18.5); folder.lineTo(4.5, 6); folder.lineTo(10, 6);
            folder.lineTo(12, 8.5); folder.lineTo(19.5, 8.5); folder.lineTo(19.5, 18.5); folder.closeSubpath();
            p.drawPath(folder); break;
        }
        case 19: // Duplicate layer
            p.drawRoundedRect(QRectF(8.5, 8.5, 11, 11), 1.5, 1.5);
            p.drawPolyline(QPolygonF{QPointF(6, 15.5), QPointF(4.5, 15.5), QPointF(4.5, 4.5), QPointF(15.5, 4.5), QPointF(15.5, 6)}); break;
        case 20: // Layer mask
            p.drawRoundedRect(QRectF(4.5, 6, 15, 12), 1.5, 1.5);
            p.setBrush(ink); p.drawEllipse(QPointF(12, 12), 3, 3); break;
        case 21: // More layer actions
            p.setBrush(ink); p.setPen(Qt::NoPen);
            for (int x : {6, 12, 18}) p.drawEllipse(QPointF(x, 12), 1.3, 1.3);
            break;
        case 22: // Delete layer
            p.drawLine(QPointF(5, 7), QPointF(19, 7));
            p.drawPolyline(QPolygonF{QPointF(9, 7), QPointF(9, 4.5), QPointF(15, 4.5), QPointF(15, 7)});
            p.drawPolyline(QPolygonF{QPointF(6.5, 7), QPointF(7.5, 19.5), QPointF(16.5, 19.5), QPointF(17.5, 7)});
            p.drawLine(QPointF(10, 10.5), QPointF(10.5, 16)); p.drawLine(QPointF(14, 10.5), QPointF(13.5, 16)); break;
        case 23: // Zoom tool (neutral magnifier)
            p.drawEllipse(QRectF(5, 4.5, 12, 12)); p.drawLine(QPointF(15.5, 15), QPointF(20, 19.5)); break;
        default: // Zoom in
            p.drawEllipse(QRectF(6.0, 5.5, 10.5, 10.5)); p.drawLine(QPointF(15.0, 14.5), QPointF(19.2, 18.7));
            p.drawLine(QPointF(8.7, 10.8), QPointF(13.8, 10.8)); p.drawLine(QPointF(11.25, 8.25), QPointF(11.25, 13.35)); break;
        }

        p.restore();
    }
private:
    int kind_;
};
} // namespace compositor
