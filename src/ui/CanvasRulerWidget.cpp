#include "ui/CanvasRulerWidget.h"
#include "ui/CanvasWidget.h"

#include <QPainter>
#include <QMouseEvent>
#include <QFontDatabase>
#include <cmath>

namespace compositor {

static constexpr int kRulerThickness = 18;

CanvasRulerCornerWidget::CanvasRulerCornerWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(kRulerThickness, kRulerThickness);
}

void CanvasRulerCornerWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(51, 51, 51));
    painter.setPen(QPen(QColor(255, 255, 255, 71), 1));
    painter.drawLine(5, kRulerThickness - 4, kRulerThickness - 4, 5);
}

CanvasRulerWidget::CanvasRulerWidget(CanvasGuide::Axis axis, EditorSession &session, CanvasWidget *canvas, QWidget *parent)
    : QWidget(parent)
    , axis_(axis)
    , session_(session)
    , canvas_(canvas)
{
    if (axis_ == CanvasGuide::Axis::Horizontal) {
        setFixedHeight(kRulerThickness);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::SplitVCursor);
    } else {
        setFixedWidth(kRulerThickness);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        setCursor(Qt::SplitHCursor);
    }
}

double CanvasRulerWidget::majorStep(double pointsPerPixel)
{
    const double target = 70.0 / std::max(pointsPerPixel, 0.0001);
    static const double nice[] = {
        1.0, 2.0, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0, 200.0, 250.0,
        500.0, 1000.0, 2000.0, 2500.0, 5000.0, 10000.0, 20000.0, 25000.0
    };
    for (double step : nice) {
        if (step >= target) return step;
    }
    return 50000.0;
}

QString CanvasRulerWidget::label(double value)
{
    const double rounded = std::round(value);
    return (rounded == 0.0) ? QStringLiteral("0") : QString::number(qint64(rounded));
}

void CanvasRulerWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(51, 51, 51));

    if (!session_.hasDocument() || !canvas_) {
        return;
    }

    const QRectF cRect = canvas_->canvasRect();
    const double scale = std::max(canvas_->zoom(), 0.0001);
    const double step = majorStep(scale);
    const double minor = step / 10.0;
    if (minor <= 0.0) return;

    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(8);
    painter.setFont(font);

    const QColor tickColor(158, 158, 158);
    const QColor labelColor(199, 199, 199);
    const QColor hairlineColor(20, 20, 20);

    const QPointF originInThis = mapFromGlobal(canvas_->mapToGlobal(cRect.topLeft()));
    const double origin = (axis_ == CanvasGuide::Axis::Horizontal) ? originInThis.x() : originInThis.y();

    const double start = (-origin) / scale;
    const double end = (axis_ == CanvasGuide::Axis::Horizontal)
        ? (double(width()) - origin) / scale
        : (double(height()) - origin) / scale;

    const double first = std::floor(std::min(start, end) / minor) * minor;
    const double last = std::ceil(std::max(start, end) / minor) * minor;
    if (!std::isfinite(first) || !std::isfinite(last)) return;

    painter.setPen(tickColor);

    for (double value = first; value <= last + 0.001; value += minor) {
        const double view = origin + value * scale;

        const double rem = std::abs(std::remainder(value, step));
        const bool isMajor = (rem < 0.001 || std::abs(rem - step) < 0.001);
        const bool isMid = !isMajor && (std::abs(std::remainder(value, step / 2.0)) < 0.001);
        const double length = isMajor ? 8.0 : (isMid ? 5.0 : 3.0);

        if (axis_ == CanvasGuide::Axis::Horizontal) {
            painter.setPen(tickColor);
            painter.drawLine(QPointF(view, height() - length), QPointF(view, height()));
            if (isMajor) {
                painter.setPen(labelColor);
                painter.drawText(QPointF(view + 2.0, 7.0), label(value));
            }
        } else {
            painter.setPen(tickColor);
            painter.drawLine(QPointF(width() - length, view), QPointF(width(), view));
            if (isMajor) {
                painter.setPen(labelColor);
                const QString text = label(value);
                const int textWidth = painter.fontMetrics().horizontalAdvance(text);
                painter.save();
                painter.translate(1.0, view + 2.0);
                painter.rotate(-90.0);
                painter.drawText(QPointF(-textWidth, 7.0), text);
                painter.restore();
            }
        }
    }

    // Border hairline
    painter.setPen(hairlineColor);
    if (axis_ == CanvasGuide::Axis::Horizontal) {
        painter.drawLine(0, height() - 1, width(), height() - 1);
    } else {
        painter.drawLine(width() - 1, 0, width() - 1, height());
    }
}

void CanvasRulerWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !session_.canEditGuides() || !canvas_) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QRectF cRect = canvas_->canvasRect();
    const double scale = std::max(canvas_->zoom(), 0.0001);
    const QPointF canvasPos = canvas_->mapFromGlobal(event->globalPosition());
    const double pos = (axis_ == CanvasGuide::Axis::Horizontal)
        ? (canvasPos.y() - cRect.top()) / scale
        : (canvasPos.x() - cRect.left()) / scale;

    session_.beginGuideCreation(axis_, pos);
    dragging_ = true;
    canvas_->update();
    event->accept();
}

void CanvasRulerWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!dragging_ || !canvas_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QRectF cRect = canvas_->canvasRect();
    const double scale = std::max(canvas_->zoom(), 0.0001);
    const QPointF canvasPos = canvas_->mapFromGlobal(event->globalPosition());
    const double pos = (axis_ == CanvasGuide::Axis::Horizontal)
        ? (canvasPos.y() - cRect.top()) / scale
        : (canvasPos.x() - cRect.left()) / scale;

    session_.moveGuideDrag(pos);
    canvas_->update();
    event->accept();
}

void CanvasRulerWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (!dragging_ || !canvas_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const QRectF cRect = canvas_->canvasRect();
    const double scale = std::max(canvas_->zoom(), 0.0001);
    const QPointF canvasPos = canvas_->mapFromGlobal(event->globalPosition());
    const bool isOver = (canvasPos.x() < 0.0 || canvasPos.y() < 0.0);
    if (!isOver) {
        const double pos = (axis_ == CanvasGuide::Axis::Horizontal)
            ? (canvasPos.y() - cRect.top()) / scale
            : (canvasPos.x() - cRect.left()) / scale;
        session_.moveGuideDrag(pos);
    }
    session_.finishGuideDrag(isOver);
    dragging_ = false;
    canvas_->update();
    update();
    emit guideChanged();
    event->accept();
}

} // namespace compositor
