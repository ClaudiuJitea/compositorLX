#include "ui/CanvasWidget.h"

#include "core/EditorSession.h"
#include "rendering/LayerRenderer.h"
#include "rendering/DownsampleCache.h"
#include "ui/ShortcutManager.h"

#include <QPainter>
#include <QPainterPath>
#include <QLineF>
#include <QBitmap>
#include <QCursor>
#include <QPixmap>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QRegion>
#include <QWheelEvent>
#include <QTimer>
#include <QDateTime>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace compositor {

static QCursor zoomCursor(bool out)
{
    QPixmap pixmap(28, 28); pixmap.fill(Qt::transparent); QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::black, 4)); painter.drawEllipse(QRectF(3, 3, 14, 14)); painter.drawLine(QPointF(15, 15), QPointF(25, 25));
    painter.setPen(QPen(Qt::white, 2)); painter.drawEllipse(QRectF(3, 3, 14, 14)); painter.drawLine(QPointF(15, 15), QPointF(25, 25));
    painter.drawLine(QPointF(7, 10), QPointF(13, 10)); if (!out) painter.drawLine(QPointF(10, 7), QPointF(10, 13)); painter.end();
    return QCursor(pixmap, 10, 10);
}

static QCursor eyedropperCursor()
{
    QPixmap pixmap(28, 28); pixmap.fill(Qt::transparent); QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
    QPolygonF shape{QPointF(6,4),QPointF(10,2),QPointF(16,8),QPointF(14,10),QPointF(22,18),QPointF(18,22),QPointF(10,14),QPointF(8,16),QPointF(5,15),QPointF(12,8)};
    painter.setPen(QPen(Qt::black, 3)); painter.setBrush(QColor(235,235,235)); painter.drawPolygon(shape); painter.setPen(QPen(Qt::white,1)); painter.drawPolygon(shape); painter.end();
    return QCursor(pixmap, 6, 23);
}

static QRectF dragBox(const QPointF &anchor, const QPointF &point, bool square, bool fromCenter)
{
    double dx = std::round(point.x()) - std::round(anchor.x());
    double dy = std::round(point.y()) - std::round(anchor.y());
    if (square) {
        const double side = std::max(std::abs(dx), std::abs(dy));
        dx = dx < 0 ? -side : side;
        dy = dy < 0 ? -side : side;
    }
    if (fromCenter) return QRectF(anchor.x() - std::abs(dx), anchor.y() - std::abs(dy), std::abs(dx) * 2, std::abs(dy) * 2);
    return QRectF(QPointF(std::min(anchor.x(), anchor.x() + dx), std::min(anchor.y(), anchor.y() + dy)), QSizeF(std::abs(dx), std::abs(dy)));
}

static QRectF transformedBounds(const LayerTransform &placement)
{
    QTransform transform;
    transform.translate(placement.center().x(), placement.center().y());
    transform.rotate(placement.rotation);
    return transform.mapRect(QRectF(-placement.size.width() / 2.0, -placement.size.height() / 2.0,
                                    placement.size.width(), placement.size.height()));
}

static bool transformContains(const LayerTransform &placement, const QPointF &point)
{
    QTransform transform; transform.translate(placement.center().x(), placement.center().y()); transform.rotate(placement.rotation);
    bool invertible = false; const QPointF local = transform.inverted(&invertible).map(point);
    return invertible && QRectF(-placement.size.width() / 2.0, -placement.size.height() / 2.0,
                                placement.size.width(), placement.size.height()).contains(local);
}

static int effectiveSelectionMode(Qt::KeyboardModifiers modifiers, int chosen)
{
    return modifiers.testFlag(Qt::AltModifier) ? 2 : modifiers.testFlag(Qt::ShiftModifier) ? 1 : chosen;
}

struct SnapResult { QPointF offset; std::optional<double> x; std::optional<double> y; };

static SnapResult snappedOffset(const Document &document, const QSet<QUuid> &moving, const QRectF &box, double tolerance, const EditorSession *session = nullptr)
{
    QVector<double> xs;
    QVector<double> ys;
    if (session) {
        const auto targets = session->alignmentSnapTargets(moving, true);
        xs = targets.xs;
        ys = targets.ys;
    } else {
        xs = {0.0, document.canvasSize.width() / 2.0, double(document.canvasSize.width())};
        ys = {0.0, document.canvasSize.height() / 2.0, double(document.canvasSize.height())};
        for (const Layer &layer : document.layers) {
            if (!layer.visible || layer.group || layer.image.isNull() || moving.contains(layer.id)) continue;
            const QRectF bounds = transformedBounds(layer.transform);
            xs << std::round(bounds.left()) << std::round(bounds.center().x()) << std::round(bounds.right());
            ys << std::round(bounds.top()) << std::round(bounds.center().y()) << std::round(bounds.bottom());
        }
    }
    const auto best = [tolerance](const std::array<double, 3> &guides, const QVector<double> &targets) {
        double move = 0, distance = tolerance + 1; std::optional<double> target;
        for (double guide : guides) for (double candidate : targets) {
            const double delta = candidate - guide;
            if (std::abs(delta) <= tolerance && std::abs(delta) < distance) { move = delta; distance = std::abs(delta); target = candidate; }
        }
        return std::pair<double, std::optional<double>>(move, target);
    };
    const auto horizontal = best({box.left(), box.center().x(), box.right()}, xs);
    const auto vertical = best({box.top(), box.center().y(), box.bottom()}, ys);
    return {QPointF(horizontal.first, vertical.first), horizontal.second, vertical.second};
}

CanvasWidget::CanvasWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAutoFillBackground(false);
    connect(&renderWatcher_, &QFutureWatcher<QImage>::finished, this, [this] {
        const quint64 completed = renderWatcher_.property("generation").toULongLong();
        const QImage image = renderWatcher_.result();
        if (completed == renderGeneration_ && !image.isNull()) {
            renderedDocument_ = image;
            renderDirty_ = false;
            update();
        }
        if (renderAgain_ || completed != renderGeneration_) {
            renderAgain_ = false;
            startBackgroundRender();
        }
    });
    auto *ants = new QTimer(this); ants->setInterval(120);
    connect(ants, &QTimer::timeout, this, [this] { if (document_ && document_->selection) update(); });
    ants->start();
}

void CanvasWidget::invalidateDocument()
{
    renderDirty_ = true;
    ++renderGeneration_;
    if (!renderedDocument_.isNull()) {
        if (renderWatcher_.isRunning()) renderAgain_ = true;
        else startBackgroundRender();
    }
    update();
}

void CanvasWidget::setBlendModePreview(const std::optional<QUuid> &layerId, const std::optional<BlendMode> &mode)
{
    const std::optional<QPair<QUuid, BlendMode>> value = layerId && mode
        ? std::optional<QPair<QUuid, BlendMode>>(qMakePair(*layerId, *mode)) : std::nullopt;
    if (blendModePreview_ == value) return;
    blendModePreview_ = value;
    invalidateDocument();
}

void CanvasWidget::applyPreviewState(Document &snapshot) const
{
    if (textEditingLayer_) for (Layer &layer : snapshot.layers) if (layer.id == *textEditingLayer_) layer.visible = false;
    if (blendModePreview_) for (Layer &layer : snapshot.layers) if (layer.id == blendModePreview_->first) {
        layer.blendMode = blendModePreview_->second; break;
    }
    const auto alone = session_ ? session_->maskAloneLayerId() : std::nullopt;
    if (!alone) return;
    for (const Layer &source : snapshot.layers) {
        if (source.id != *alone || source.mask.isNull()) continue;
        // Just the mask, opaque and gray, where the mask sits; everything else out of the way.
        Layer view;
        view.id = source.id; view.name = source.name;
        QImage gray = source.mask.convertToFormat(QImage::Format_Grayscale8).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        view.image = gray;
        view.transform = source.maskPlacement.value_or(source.transform);
        if (source.mask.size() == QSize(1, 1)) view.transform.sampling = Sampling::Nearest;
        snapshot.layers = {view};
        return;
    }
}

QRectF CanvasWidget::maskAloneBadgeRect() const
{
    const QSizeF size(220, 26);
    return QRectF((width() - size.width()) / 2.0, height() - size.height() - 14.0, size.width(), size.height());
}

void CanvasWidget::startBackgroundRender()
{
    if (!document_ || renderedDocument_.isNull()) return;
    if (renderWatcher_.isRunning()) return;
    Document snapshot = *document_;
    applyPreviewState(snapshot);
    renderWatcher_.setProperty("generation", QVariant::fromValue(renderGeneration_));
    renderWatcher_.setFuture(QtConcurrent::run([snapshot] { return LayerRenderer::flattened(snapshot); }));
}

void CanvasWidget::setTool(Tool tool)
{
    if (floatingTransform_ && tool != Tool::Move) emit floatingTransformCommitRequested();
    if (pendingGradient_ && tool != Tool::Gradient) resolvePendingGradient(true);
    if (pendingDistortion_ && tool != Tool::Move) resolvePendingDistortion(true);
    if (tool_ == Tool::Crop && tool != Tool::Crop) resolvePendingCrop(false);
    tool_ = tool;
    if (tool_ == Tool::Crop && !cropRect_ && document_) {
        std::optional<QRect> selBounds;
        if (session_) {
            selBounds = session_->selectionBounds();
        } else if (document_->selection) {
            const QImage mask = document_->selection->convertToFormat(QImage::Format_Grayscale8);
            int left = mask.width(), top = mask.height(), right = -1, bottom = -1;
            for (int y = 0; y < mask.height(); ++y) {
                const uchar *row = mask.constScanLine(y);
                for (int x = 0; x < mask.width(); ++x) {
                    if (row[x] > 0) {
                        left = std::min(left, x); right = std::max(right, x);
                        top = std::min(top, y); bottom = std::max(bottom, y);
                    }
                }
            }
            if (right >= left) selBounds = QRect(left, top, right - left + 1, bottom - top + 1);
        }
        if (selBounds) {
            const QRect canvasBox(0, 0, document_->canvasSize.width(), document_->canvasSize.height());
            const QRect intersected = selBounds->intersected(canvasBox);
            if (!intersected.isEmpty()) {
                cropRect_ = intersected;
                emit pendingCropChanged(true);
            }
        }
    }
    if (editorInteractionBlocked_ && tool_ != Tool::Hand && tool_ != Tool::Zoom && tool_ != Tool::Eyedropper && !hueTargeting_) setCursor(Qt::ArrowCursor);
    else if (tool_ == Tool::Brush || tool_ == Tool::Eraser || tool_ == Tool::Healing || tool_ == Tool::Blur) setCursor(Qt::BlankCursor);
    else if (tool_ == Tool::Hand) setCursor(Qt::OpenHandCursor);
    else if (tool_ == Tool::Zoom) setCursor(zoomCursor(false));
    else if (tool_ == Tool::Text) setCursor(Qt::IBeamCursor);
    else if (tool_ == Tool::Eyedropper) setCursor(eyedropperCursor());
    else if (tool_ == Tool::Marquee || tool_ == Tool::Lasso || tool_ == Tool::Wand || tool_ == Tool::Gradient || tool_ == Tool::Shape || tool_ == Tool::Clone || tool_ == Tool::Crop) setCursor(Qt::CrossCursor);
    else unsetCursor();
    emit toolChanged(tool_);
    update();
}

QRectF CanvasWidget::widgetRectForDocumentRect(const QRectF &rect) const
{
    return QRectF(canvasRect().topLeft() + rect.topLeft() * zoom_, rect.size() * zoom_);
}

QRectF CanvasWidget::documentRectForWidgetRect(const QRectF &rect) const
{
    return QRectF((rect.topLeft() - canvasRect().topLeft()) / zoom_, rect.size() / zoom_);
}

void CanvasWidget::setEditorInteractionBlocked(bool blocked)
{
    editorInteractionBlocked_ = blocked;
    if (blocked) { brushDrawing_ = false; creationDragging_ = false; }
    if (hueTargeting_) setCursor(Qt::SizeHorCursor);
    else if (tool_ == Tool::Hand) setCursor(Qt::OpenHandCursor);
    else if (tool_ == Tool::Zoom) setCursor(zoomCursor(false));
    else if (tool_ == Tool::Text) setCursor(Qt::IBeamCursor);
    else if (tool_ == Tool::Eyedropper) setCursor(eyedropperCursor());
    else if (blocked) setCursor(Qt::ArrowCursor);
    else if (tool_ == Tool::Brush || tool_ == Tool::Eraser || tool_ == Tool::Healing || tool_ == Tool::Blur) setCursor(Qt::BlankCursor);
    else if (tool_ == Tool::Marquee || tool_ == Tool::Lasso || tool_ == Tool::Wand || tool_ == Tool::Gradient || tool_ == Tool::Shape || tool_ == Tool::Clone || tool_ == Tool::Crop) setCursor(Qt::CrossCursor);
    else unsetCursor();
    update();
}

void CanvasWidget::setCropRatio(double ratio)
{
    cropRatio_ = std::isfinite(ratio) && ratio > 0 ? ratio : 0;
    if (!cropRatio_ || !document_) return;
    QRectF rect = cropRect_.value_or(QRectF(QPointF(), QSizeF(document_->canvasSize)));
    const double height = rect.width() / cropRatio_;
    rect = QRectF(rect.left(), rect.center().y() - height / 2, rect.width(), height);
    rect = QRectF(std::round(rect.x()), std::round(rect.y()), std::max(1.0, std::round(rect.width())), std::max(1.0, std::round(rect.height())));
    cropRect_ = rect; emit pendingCropChanged(true); update();
}

void CanvasWidget::refreshPendingGradient()
{
    if (pendingGradient_) emit gradientRequested(pendingGradient_->first, pendingGradient_->second);
}

void CanvasWidget::resolvePendingGradient(bool commit)
{
    if (!pendingGradient_) return;
    commit ? emit gradientCommitRequested() : emit gradientCancelRequested();
    pendingGradient_.reset();
    update();
}

void CanvasWidget::resolvePendingDistortion(bool apply)
{
    if (!pendingDistortion_) return;
    if (apply) { QPolygonF polygon; for (const QPointF &point : distortionCorners_) polygon << point; emit layerDistortionRequested(polygon, transformMask_); }
    pendingDistortion_ = false;
    emit pendingDistortionChanged(false);
    update();
}

void CanvasWidget::resolvePendingCrop(bool apply)
{
    if (!cropRect_) return;
    if (apply) {
        const QRectF value = cropRect_->normalized();
        emit cropRequested(QRect(qRound(value.x()), qRound(value.y()), std::max(1, qRound(value.width())), std::max(1, qRound(value.height()))));
    }
    cropRect_.reset(); cropDrag_ = CropDrag::None; emit pendingCropChanged(false); update();
}

void CanvasWidget::setDocument(std::shared_ptr<Document> document, bool invalidate)
{
    if (document_ == document) { if (invalidate) invalidateDocument(); else update(); return; }
    document_ = std::move(document);
    lastBrushPoint_.reset(); lastBrushLayerId_.reset();
    renderedDocument_ = {};
    renderDirty_ = true;
    renderAgain_ = false;
    ++renderGeneration_;
    panOffset_ = {};
    fitPending_ = true;
    fitCanvas();
}

void CanvasWidget::fitCanvas()
{
    if (!document_ || document_->canvasSize.isEmpty() || width() < 1 || height() < 1) return;
    constexpr double margin = 48.0;
    zoom_ = std::clamp(std::min((width() - margin) / document_->canvasSize.width(),
                                (height() - margin) / document_->canvasSize.height()),
                       0.001, 32.0);
    fitPending_ = false;
    panOffset_ = {};
    emit zoomChanged(zoom_);
    emit viewportChanged();
    update();
}

void CanvasWidget::actualPixels()
{
    zoomTo(1.0, rect().center());
}

void CanvasWidget::zoomIn()
{
    zoomTo(zoom_ * 1.25, rect().center());
}

void CanvasWidget::zoomOut()
{
    zoomTo(zoom_ / 1.25, rect().center());
}

void CanvasWidget::zoomTo(double value, const QPointF &anchor)
{
    if (!document_ || !std::isfinite(value)) return;
    const QRectF before = canvasRect();
    const QPointF documentPoint = (anchor - before.topLeft()) / zoom_;
    zoom_ = std::clamp(value, .001, 32.0);
    fitPending_ = false;
    const QSizeF shown(document_->canvasSize.width() * zoom_, document_->canvasSize.height() * zoom_);
    const QPointF centered((width() - shown.width()) / 2.0, (height() - shown.height()) / 2.0);
    panOffset_ = anchor - documentPoint * zoom_ - centered;
    emit zoomChanged(zoom_);
    emit viewportChanged();
    update();
}

QRectF CanvasWidget::canvasRect() const
{
    if (!document_) return {};
    const QSizeF size(document_->canvasSize.width() * zoom_, document_->canvasSize.height() * zoom_);
    return QRectF(QPointF(std::floor((width() - size.width()) / 2.0), std::floor((height() - size.height()) / 2.0)) + panOffset_, size);
}

std::optional<QPointF> CanvasWidget::documentPointAt(const QPointF &widgetPoint) const
{
    if (!document_) return std::nullopt;
    const QRectF shown = canvasRect();
    if (!shown.contains(widgetPoint)) return std::nullopt;
    return (widgetPoint - shown.topLeft()) / zoom_;
}

QRectF CanvasWidget::selectedLayersBounds() const
{
    QRectF result;
    bool first = true;
    if (!document_) return result;
    const QSet<QUuid> targets = transformLayerIds();
    for (const Layer &layer : document_->layers) {
        if (!targets.contains(layer.id)) continue;
        const LayerTransform &placement = layer.transform;
        QTransform transform;
        transform.translate(placement.center().x(), placement.center().y());
        transform.rotate(placement.rotation);
        const QRectF local(-placement.size.width() / 2.0, -placement.size.height() / 2.0,
                           placement.size.width(), placement.size.height());
        const QRectF bounds = transform.mapRect(local);
        result = first ? bounds : result.united(bounds);
        first = false;
    }
    return result;
}

QSet<QUuid> CanvasWidget::transformLayerIds() const
{
    QSet<QUuid> result;
    if (!document_) return result;
    for (const Layer &layer : document_->layers) {
        if (layer.group || layer.image.isNull() || !layer.transform.isValid()) continue;
        bool visible = layer.visible, selected = selectedLayerIds_.contains(layer.id);
        std::optional<QUuid> parent = layer.parentId;
        QSet<QUuid> visited;
        while (parent && !visited.contains(*parent)) {
            visited.insert(*parent);
            const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [&](const Layer &candidate) { return candidate.id == *parent; });
            if (it == document_->layers.cend()) { visible = false; break; }
            visible = visible && it->visible;
            selected = selected || selectedLayerIds_.contains(it->id);
            parent = it->parentId;
        }
        if (visible && selected) result.insert(layer.id);
    }
    return result;
}

bool CanvasWidget::transformsAsGroup() const
{
    if (!document_) return false;
    if (selectedLayerIds_.size() > 1) return true;
    if (!document_->activeLayerId) return false;
    const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [this](const Layer &layer) { return layer.id == *document_->activeLayerId; });
    return it != document_->layers.cend() && it->group;
}

void CanvasWidget::drawCheckerboard(QPainter &painter, const QRectF &area) const
{
    constexpr int tile = 12;
    painter.save();
    painter.setClipRect(area);
    painter.fillRect(area, QColor(211, 211, 211));
    painter.setBrush(QColor(238, 238, 238));
    painter.setPen(Qt::NoPen);
    const int left = int(std::floor(area.left() / tile)) * tile;
    const int top = int(std::floor(area.top() / tile)) * tile;
    for (int y = top; y < area.bottom(); y += tile) {
        for (int x = left; x < area.right(); x += tile) {
            if (((x / tile) + (y / tile)) % 2 == 0) painter.drawRect(x, y, tile, tile);
        }
    }
    painter.restore();
}

void CanvasWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), QColor(30, 30, 32));
    if (!document_) {
        painter.setPen(QColor(180, 180, 180));
        painter.drawText(rect(), Qt::AlignCenter, tr("Open a Compositor .comp project"));
        return;
    }

    const QRectF target = canvasRect();
    painter.setRenderHint(QPainter::Antialiasing, true);
    drawCheckerboard(painter, target);
    if (renderDirty_ || renderedDocument_.size() != document_->canvasSize) {
        if (renderedDocument_.isNull() || renderedDocument_.size() != document_->canvasSize) {
            Document snapshot = *document_;
            applyPreviewState(snapshot);
            renderedDocument_ = LayerRenderer::flattened(snapshot);
            renderDirty_ = false;
        } else startBackgroundRender();
    }
    const QImage displayImage = DownsampleCache::shared().image(renderedDocument_, zoom_);
    painter.save();
    painter.setClipRect(target);
    painter.translate(target.topLeft());
    painter.scale(zoom_, zoom_);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 2.0);
    painter.drawImage(QRectF(QPointF(), QSizeF(document_->canvasSize)), displayImage);
    painter.restore();

    // Match the source editor's document-pixel grid: it is optional and only
    // becomes useful once an individual pixel spans at least eight view pixels.
    if (showPixelGrid_ && zoom_ >= 8.0) {
        painter.save();
        painter.setClipRect(target);
        painter.setPen(QPen(QColor(140, 140, 140, 115), 1.0));
        const QRectF visible = target.intersected(rect());
        const int firstColumn = std::max(0, int(std::ceil((visible.left() - target.left()) / zoom_)));
        const int lastColumn = std::min(document_->canvasSize.width(),
                                        int(std::floor((visible.right() - target.left()) / zoom_)));
        const int firstRow = std::max(0, int(std::ceil((visible.top() - target.top()) / zoom_)));
        const int lastRow = std::min(document_->canvasSize.height(),
                                    int(std::floor((visible.bottom() - target.top()) / zoom_)));
        for (int column = firstColumn; column <= lastColumn; ++column) {
            const qreal x = target.left() + column * zoom_;
            painter.drawLine(QPointF(x, visible.top()), QPointF(x, visible.bottom()));
        }
        for (int row = firstRow; row <= lastRow; ++row) {
            const qreal y = target.top() + row * zoom_;
            painter.drawLine(QPointF(visible.left(), y), QPointF(visible.right(), y));
        }
        painter.restore();
    }

    if (session_ && session_->showsGrid()) {
        painter.save();
        painter.setClipRect(target);
        const LayoutGrid &grid = session_->layoutGrid();
        const GridAppearance &look = session_->gridAppearance();
        QColor base = look.color();
        const double subdivisionGap = grid.step() * zoom_;
        if (subdivisionGap >= 4.0) {
            QColor sub = base; sub.setAlphaF(look.subdivisionAlpha());
            QPen subPen(sub, 1.0);
            subPen.setDashPattern({1.0, 2.0});
            painter.setPen(subPen);
            for (double x : grid.lines(document_->canvasSize.width())) {
                if (!grid.isMajor(x)) {
                    const double vx = target.left() + x * zoom_;
                    painter.drawLine(QPointF(vx, target.top()), QPointF(vx, target.bottom()));
                }
            }
            for (double y : grid.lines(document_->canvasSize.height())) {
                if (!grid.isMajor(y)) {
                    const double vy = target.top() + y * zoom_;
                    painter.drawLine(QPointF(target.left(), vy), QPointF(target.right(), vy));
                }
            }
        }
        QColor major = base; major.setAlphaF(look.majorAlpha());
        QPen majorPen(major, 1.0);
        if (!look.dashes().isEmpty()) majorPen.setDashPattern(look.dashes());
        painter.setPen(majorPen);
        for (double x : grid.lines(document_->canvasSize.width())) {
            if (grid.isMajor(x)) {
                const double vx = target.left() + x * zoom_;
                painter.drawLine(QPointF(vx, target.top()), QPointF(vx, target.bottom()));
            }
        }
        for (double y : grid.lines(document_->canvasSize.height())) {
            if (grid.isMajor(y)) {
                const double vy = target.top() + y * zoom_;
                painter.drawLine(QPointF(target.left(), vy), QPointF(target.right(), vy));
            }
        }
        painter.restore();
    }

    if (document_->selection) {
        const QImage mask = document_->selection->convertToFormat(QImage::Format_Grayscale8);
        const QPointF selectionShift = QPointF(pixelDragOffset_) * zoom_;
        painter.save();
        painter.setClipRect(target);
        painter.setOpacity(0.14);
        painter.setCompositionMode(QPainter::CompositionMode_Screen);
        painter.drawImage(target.translated(selectionShift), mask);
        painter.restore();
        if (selectionOutlineKey_ != mask.cacheKey()) {
            selectionOutlineKey_ = mask.cacheKey(); selectionOutline_ = {};
            const auto selected = [&mask](int x, int y) { return x >= 0 && y >= 0 && x < mask.width() && y < mask.height() && mask.constScanLine(y)[x] >= 128; };
            for (int y = 0; y < mask.height(); ++y) for (int x = 0; x < mask.width(); ++x) if (selected(x, y)) {
                const QPointF base(x, y);
                if (!selected(x, y - 1)) { selectionOutline_.moveTo(base); selectionOutline_.lineTo(base + QPointF(1, 0)); }
                if (!selected(x + 1, y)) { selectionOutline_.moveTo(base + QPointF(1, 0)); selectionOutline_.lineTo(base + QPointF(1, 1)); }
                if (!selected(x, y + 1)) { selectionOutline_.moveTo(base + QPointF(1, 1)); selectionOutline_.lineTo(base + QPointF(0, 1)); }
                if (!selected(x - 1, y)) { selectionOutline_.moveTo(base + QPointF(0, 1)); selectionOutline_.lineTo(base); }
            }
        }
        QTransform selectionTransform; selectionTransform.translate(target.left() + selectionShift.x(), target.top() + selectionShift.y()); selectionTransform.scale(zoom_, zoom_);
        const QPainterPath outline = selectionTransform.map(selectionOutline_);
        if (!outline.isEmpty()) {
            painter.setBrush(Qt::NoBrush); painter.setPen(QPen(Qt::white, 1)); painter.drawPath(outline);
            QPen ants(Qt::black, 1); ants.setDashPattern({4, 4}); ants.setDashOffset((QDateTime::currentMSecsSinceEpoch() / 120) % 8);
            painter.setPen(ants); painter.drawPath(outline);
        }
    }
    if (samplingColor_ && showSampleRing_ && cursorDocument_) {
        const QPointF center = target.topLeft() + *cursorDocument_ * zoom_ + QPointF(42, -42);
        const QRectF ring(center.x() - 34, center.y() - 34, 68, 68);
        painter.save(); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(QPen(QColor(35,35,35), 5)); painter.setBrush(Qt::NoBrush); painter.drawEllipse(ring);
        painter.setClipRect(QRectF(ring.left(), ring.top(), ring.width(), ring.height()/2)); painter.setPen(QPen(sampleCurrent_, 12)); painter.drawEllipse(ring.adjusted(8,8,-8,-8));
        painter.setClipRect(QRectF(ring.left(), ring.center().y(), ring.width(), ring.height()/2)); painter.setPen(QPen(sampleOriginal_, 12)); painter.drawEllipse(ring.adjusted(8,8,-8,-8)); painter.restore();
    }
    painter.setPen(QColor(0, 0, 0, 180));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(target);

    painter.save(); painter.setPen(QPen(QColor(255, 45, 160, 210), 1));
    if (snapGuideX_) { const double x = target.left() + *snapGuideX_ * zoom_; painter.drawLine(QPointF(x, target.top()), QPointF(x, target.bottom())); }
    if (snapGuideY_) { const double y = target.top() + *snapGuideY_ * zoom_; painter.drawLine(QPointF(target.left(), y), QPointF(target.right(), y)); }
    painter.restore();

    if (session_ && (session_->showsGuides() || session_->guideDrag())) {
        painter.save();
        painter.setPen(QPen(QColor(0, 255, 255, 230), 1));
        for (const CanvasGuide &guide : session_->displayedGuides()) {
            if (guide.axis == CanvasGuide::Axis::Vertical) {
                const double x = target.left() + guide.position * zoom_;
                painter.drawLine(QPointF(x, 0), QPointF(x, height()));
            } else {
                const double y = target.top() + guide.position * zoom_;
                painter.drawLine(QPointF(0, y), QPointF(width(), y));
            }
        }
        painter.restore();
    }

    if (tool_ == Tool::Crop) {
        const QRectF crop = cropRect_.value_or(QRectF(QPointF(), QSizeF(document_->canvasSize))).normalized();
        const QRectF box(target.topLeft() + crop.topLeft() * zoom_, crop.size() * zoom_);
        painter.save();
        QPainterPath shade; shade.addRect(target); QPainterPath opening; opening.addRect(box); shade = shade.subtracted(opening);
        painter.fillPath(shade, QColor(0, 0, 0, 125));
        painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor(238, 242, 247), 1)); painter.drawRect(box);
        painter.setPen(QPen(QColor(238, 242, 247, 100), 1));
        painter.drawLine(QPointF(box.left() + box.width() / 3, box.top()), QPointF(box.left() + box.width() / 3, box.bottom()));
        painter.drawLine(QPointF(box.left() + box.width() * 2 / 3, box.top()), QPointF(box.left() + box.width() * 2 / 3, box.bottom()));
        painter.drawLine(QPointF(box.left(), box.top() + box.height() / 3), QPointF(box.right(), box.top() + box.height() / 3));
        painter.drawLine(QPointF(box.left(), box.top() + box.height() * 2 / 3), QPointF(box.right(), box.top() + box.height() * 2 / 3));
        painter.setBrush(QColor(245, 249, 255)); painter.setPen(QPen(QColor(36, 145, 255), 1));
        constexpr qreal handle = 7;
        const QPointF points[] = {box.topLeft(), QPointF(box.center().x(), box.top()), box.topRight(), QPointF(box.right(), box.center().y()),
                                  box.bottomRight(), QPointF(box.center().x(), box.bottom()), box.bottomLeft(), QPointF(box.left(), box.center().y())};
        for (const QPointF &point : points) painter.drawRect(QRectF(point.x() - handle / 2, point.y() - handle / 2, handle, handle));
        painter.restore();
    }

    const bool distortionActive = pendingDistortion_ || (movingLayer_ && transformDrag_ == TransformDrag::Distort);
    if (distortionActive) {
        QPolygonF polygon;
        for (const QPointF &point : distortionCorners_) polygon << target.topLeft() + point * zoom_;
        painter.save(); painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor(36, 145, 255), 1.15)); painter.drawPolygon(polygon);
        painter.setPen(QPen(QColor(232, 244, 255), 1)); painter.setBrush(QColor(245, 249, 255));
        for (const QPointF &point : polygon) painter.drawRect(QRectF(point.x() - 3, point.y() - 3, 6, 6));
        for (int i = 0; i < 4; ++i) { const QPointF midpoint = (polygon[i] + polygon[(i + 1) % 4]) / 2; painter.drawRect(QRectF(midpoint.x() - 3, midpoint.y() - 3, 6, 6)); }
        painter.restore();
    }
    const QRectF groupBounds = !distortionActive && transformControlsVisible() && transformsAsGroup() && !transformMask_ ? selectedLayersBounds() : QRectF();
    if (!groupBounds.isEmpty()) {
        painter.save();
        const QRectF box(target.topLeft() + groupBounds.topLeft() * zoom_, groupBounds.size() * zoom_);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(36, 145, 255), 1.15));
        painter.drawRect(box);
        painter.setPen(QPen(QColor(232, 244, 255), 1.0));
        painter.setBrush(QColor(245, 249, 255));
        constexpr qreal handle = 6.0;
        const QPointF points[] = {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight(),
                                  QPointF(box.center().x(), box.top()), QPointF(box.center().x(), box.bottom()),
                                  QPointF(box.left(), box.center().y()), QPointF(box.right(), box.center().y())};
        for (const QPointF &point : points) painter.drawRect(QRectF(point.x() - handle / 2, point.y() - handle / 2, handle, handle));
        painter.setPen(QPen(QColor(36, 145, 255), 1.0));
        painter.drawLine(QPointF(box.center().x(), box.top()), QPointF(box.center().x(), box.top() - 24));
        painter.setBrush(QColor(245, 249, 255));
        painter.drawEllipse(QPointF(box.center().x(), box.top() - 28), 4, 4);
        painter.restore();
    } else if (!distortionActive && transformControlsVisible() && document_->activeLayerId) {
        const auto it = std::find_if(document_->layers.cbegin(), document_->layers.cend(), [this](const Layer &layer) {
            return layer.id == *document_->activeLayerId;
        });
        if (it != document_->layers.cend() && !it->group && !it->image.isNull()) {
            const LayerTransform displayed=transformMask_?it->maskPlacement.value_or(it->transform):it->transform;
            painter.save();
            const QPointF center = target.topLeft() + displayed.center() * zoom_;
            painter.translate(center);
            painter.rotate(displayed.rotation);
            const QSizeF size = displayed.size * zoom_;
            QRectF box(-size.width() / 2.0, -size.height() / 2.0, size.width(), size.height());
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(36, 145, 255), 1.15));
            painter.drawRect(box);
            painter.setPen(QPen(QColor(232, 244, 255), 1.0));
            painter.setBrush(QColor(245, 249, 255));
            constexpr qreal handle = 6.0;
            const QPointF points[] = {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight(),
                                      QPointF(box.center().x(), box.top()), QPointF(box.center().x(), box.bottom()),
                                      QPointF(box.left(), box.center().y()), QPointF(box.right(), box.center().y())};
            for (const QPointF &point : points) painter.drawRect(QRectF(point.x() - handle / 2, point.y() - handle / 2, handle, handle));
            painter.setPen(QPen(QColor(36, 145, 255), 1.0));
            painter.drawLine(QPointF(0, box.top()), QPointF(0, box.top() - 24));
            painter.setBrush(QColor(245, 249, 255));
            painter.drawEllipse(QPointF(0, box.top() - 28), 4, 4);
            painter.restore();
        }
    }
    if (selectionDragging_) {
        const QRect selection = QRect(selectionAnchor_, selectionCurrent_).normalized();
        const QRectF shown(target.topLeft() + QPointF(selection.topLeft()) * zoom_, QSizeF(selection.size()) * zoom_);
        painter.setBrush(QColor(30, 130, 230, 35));
        painter.setPen(QPen(QColor(235, 242, 250), 1, Qt::DashLine));
        if (tool_ == Tool::Marquee && ellipticalMarquee_) painter.drawEllipse(shown); else painter.drawRect(shown);
    }
    if (!lassoPoints_.isEmpty()) {
        QPolygonF shown;
        for (const QPointF &point : lassoPoints_) shown << target.topLeft() + point * zoom_;
        painter.setBrush(QColor(30, 130, 230, 25));
        painter.setPen(QPen(QColor(235, 242, 250), 1, Qt::DashLine));
        painter.drawPolygon(shown);
    }
    if (creationDragging_ || (tool_ == Tool::Gradient && pendingGradient_)) {
        const QPointF gradientStart = creationDragging_ ? creationAnchor_ : pendingGradient_->first;
        const QPointF gradientEnd = creationDragging_ ? creationCurrent_ : pendingGradient_->second;
        const QRectF creationRect = dragBox(creationAnchor_, creationCurrent_, creationSquare_, creationFromCenter_);
        const QPointF first = target.topLeft() + (tool_ == Tool::Gradient ? gradientStart : creationRect.topLeft()) * zoom_;
        const QPointF last = target.topLeft() + (tool_ == Tool::Gradient ? gradientEnd : creationRect.bottomRight()) * zoom_;
        if (tool_ == Tool::Gradient) {
            painter.drawLine(first, last); painter.drawEllipse(first, 3, 3); painter.drawEllipse(last, 3, 3);
        } else if (tool_ == Tool::Shape && shapeKind_ == ShapeKind::Line) {
            QPointF lineStart = creationAnchor_;
            QPointF lineEnd = creationCurrent_;
            if (creationSquare_) {
                const double dx = lineEnd.x() - lineStart.x();
                const double dy = lineEnd.y() - lineStart.y();
                const double angle = std::round(std::atan2(dy, dx) / (M_PI / 4.0)) * (M_PI / 4.0);
                const double len = std::hypot(dx, dy);
                lineEnd = QPointF(lineStart.x() + std::cos(angle) * len, lineStart.y() + std::sin(angle) * len);
            }
            const QPointF p1 = target.topLeft() + lineStart * zoom_;
            const QPointF p2 = target.topLeft() + lineEnd * zoom_;
            const double thickness = std::max(1.0, shapeLineWidth_ * zoom_);
            QPen linePen(paletteForeground_, thickness, Qt::SolidLine, Qt::RoundCap);
            painter.setPen(linePen);
            painter.drawLine(p1, p2);
        } else {
            const QRectF box(first, last);
            if (shapeKind_ == ShapeKind::Ellipse || (tool_ != Tool::Shape && ellipticalShape_)) {
                painter.drawEllipse(box.normalized());
            } else if (tool_ == Tool::Shape && shapeCornerRadius_ > 0) {
                const double radius = std::min({shapeCornerRadius_ * zoom_, box.width() / 2.0, box.height() / 2.0});
                painter.drawRoundedRect(box.normalized(), radius, radius);
            } else {
                painter.drawRect(box.normalized());
            }
        }
    }
    if (cloneSource_ && tool_ == Tool::Clone) {
        const QPointF point = target.topLeft() + *cloneIndicatorPosition() * zoom_;
        painter.setPen(QPen(QColor(245, 245, 245), 1)); painter.drawLine(point + QPointF(-7, 0), point + QPointF(7, 0)); painter.drawLine(point + QPointF(0, -7), point + QPointF(0, 7));
    }
    if (cursorDocument_ && (tool_ == Tool::Brush || tool_ == Tool::Eraser || tool_ == Tool::Healing || tool_ == Tool::Clone || tool_ == Tool::Blur)) {
        const QPointF center = target.topLeft() + *cursorDocument_ * zoom_;
        const qreal diameter = brushDiameter_ * zoom_;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(245, 245, 245, 210), 1));
        painter.drawEllipse(center, diameter / 2, diameter / 2);
        painter.setPen(QPen(QColor(20, 20, 20, 190), 1));
        painter.drawEllipse(center, diameter / 2 + 1, diameter / 2 + 1);
    }
    if (session_ && session_->maskAloneLayerId()) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF badge = maskAloneBadgeRect();
        painter.setPen(Qt::NoPen); painter.setBrush(QColor(30, 30, 32, 225));
        painter.drawRoundedRect(badge, 6, 6);
        painter.setPen(QColor(225, 226, 228));
        painter.drawText(badge.adjusted(10, 0, -64, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("Mask — Show Image"));
        painter.setPen(QPen(QColor(120, 170, 235), 1.0)); painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(badge.right() - 58, badge.top() + 4, 50, badge.height() - 8), 4, 4);
        painter.drawText(QRectF(badge.right() - 58, badge.top() + 4, 50, badge.height() - 8), Qt::AlignCenter, tr("Back"));
        painter.restore();
    }
}

void CanvasWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (fitPending_) fitCanvas();
    else emit viewportChanged();
}

void CanvasWidget::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        zoomTo(event->angleDelta().y() > 0 ? zoom_ * 1.25 : zoom_ / 1.25, event->position());
        event->accept();
        return;
    }
    QPoint delta = event->pixelDelta();
    if (delta.isNull()) delta = event->angleDelta() / 4;
    panOffset_ += QPointF(delta.x(), delta.y()); fitPending_ = false; emit viewportChanged(); update(); event->accept();
}

void CanvasWidget::mousePressEvent(QMouseEvent *event)
{
    // The badge under a mask shown alone has its own button back to the image.
    if (event->button() == Qt::LeftButton && session_ && session_->maskAloneLayerId() && maskAloneBadgeRect().contains(event->position())) {
        emit maskAloneExitRequested(); event->accept(); return;
    }
    if (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton) {
        panning_ = true;
        lastMousePosition_ = event->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && document_ && tool_ == Tool::Hand) {
        panning_ = true; lastMousePosition_ = event->position().toPoint(); setCursor(Qt::ClosedHandCursor); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && document_ && tool_ == Tool::Zoom) {
        zoomTo(event->modifiers().testFlag(Qt::AltModifier) ? zoom_ / 1.25 : zoom_ * 1.25, event->position()); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && document_ && hueTargeting_) {
        const QPointF position = (event->position() - canvasRect().topLeft()) / zoom_;
        hueTargetStart_ = event->position(); hueTargetDragging_ = true;
        emit hueTargetStarted(QPoint(qFloor(position.x()), qFloor(position.y())));
        setCursor(Qt::SizeHorCursor); event->accept(); return;
    }
    // Option temporarily turns Brush, Eraser, Spot Healing and Gradient into the Eyedropper (mac palettePicking).
    const bool optionPick = event->modifiers().testFlag(Qt::AltModifier) && !brushDrawing_ && !creationDragging_
        && (tool_ == Tool::Brush || tool_ == Tool::Eraser || tool_ == Tool::Healing || tool_ == Tool::Gradient);
    if (event->button() == Qt::LeftButton && document_ && (tool_ == Tool::Eyedropper || optionPick)) {
        const QPointF position = (event->position() - canvasRect().topLeft()) / zoom_;
        samplingColor_ = true; sampleOriginal_ = paletteForeground_;
        const QPoint pixel(qFloor(position.x()), qFloor(position.y())); if (QRect(QPoint(), renderedDocument_.size()).contains(pixel)) sampleCurrent_ = renderedDocument_.pixelColor(pixel);
        emit colorSampleRequested(QPoint(qFloor(position.x()), qFloor(position.y()))); event->accept(); return;
    }
    if (editorInteractionBlocked_) { event->accept(); return; }
    if (event->button() == Qt::LeftButton && document_ && tool_ == Tool::Crop) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        const QRectF visible = cropRect_.value_or(QRectF(QPointF(), QSizeF(document_->canvasSize))).normalized();
        const double tolerance = 8.0 / zoom_;
        const std::array<QPair<QPoint, QPointF>, 8> handles{{
            {QPoint(-1,-1),visible.topLeft()},{QPoint(0,-1),QPointF(visible.center().x(),visible.top())},{QPoint(1,-1),visible.topRight()},
            {QPoint(1,0),QPointF(visible.right(),visible.center().y())},{QPoint(1,1),visible.bottomRight()},{QPoint(0,1),QPointF(visible.center().x(),visible.bottom())},
            {QPoint(-1,1),visible.bottomLeft()},{QPoint(-1,0),QPointF(visible.left(),visible.center().y())}}};
        cropHandle_ = {};
        for (const auto &[sign, position] : handles) if (QLineF(point, position).length() <= tolerance) { cropHandle_ = sign; break; }
        cropStart_ = point; cropOriginal_ = visible;
        if (!cropHandle_.isNull()) cropDrag_ = CropDrag::Resize;
        else if (cropRect_ && visible.contains(point)) cropDrag_ = CropDrag::Move;
        else { cropDrag_ = CropDrag::Create; cropOriginal_ = {}; cropRect_.reset(); }
        setFocus(); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && document_ && (tool_ == Tool::Marquee || tool_ == Tool::Wand)) {
        const QPoint point = ((event->position() - canvasRect().topLeft()) / zoom_).toPoint();
        const bool insideSelection = document_->selection && QRect(QPoint(), document_->selection->size()).contains(point)
            && document_->selection->convertToFormat(QImage::Format_Grayscale8).constScanLine(point.y())[point.x()] > 0;
        if (insideSelection && event->modifiers().testFlag(Qt::ControlModifier)) {
            emit selectedPixelsDragStarted(event->modifiers().testFlag(Qt::AltModifier));
            if (floatingTransform_ && document_->activeLayerId) for (const Layer &layer : document_->layers) if (layer.id == *document_->activeLayerId) {
                pixelDragging_ = true; pixelDragStart_ = point; pixelDragOriginalOrigin_ = layer.transform.origin; pixelDragOffset_ = {}; setCursor(event->modifiers().testFlag(Qt::AltModifier) ? Qt::DragCopyCursor : Qt::SizeAllCursor); break;
            }
            event->accept(); return;
        }
        if (tool_ != Tool::Crop && selectionMode_ == 0 && document_->selection && !(event->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))
            && insideSelection) {
            movingSelection_ = true; selectionAnchor_ = selectionCurrent_ = point; event->accept(); return;
        }
        if (tool_ == Tool::Wand) {
            const int mode = effectiveSelectionMode(event->modifiers(), selectionMode_);
            if (wandMode_ == WandMode::Object) {
                emit objectSelectionRequested(point, mode);
            } else {
                emit magicWandRequested(point, mode);
            }
        } else {
            selectionDragging_ = true;
            if (tool_ == Tool::Marquee) { const QPointF snapped = snapDragPoint(QPointF(point), event->modifiers()); selectionAnchor_ = selectionCurrent_ = snapped.toPoint(); }
            else { selectionAnchor_ = point; selectionCurrent_ = point; }
            update();
        }
        event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && document_ && tool_ == Tool::Lasso) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        const QPoint integer = point.toPoint();
        const bool insideSelection = document_->selection && QRect(QPoint(), document_->selection->size()).contains(integer)
            && document_->selection->convertToFormat(QImage::Format_Grayscale8).constScanLine(integer.y())[integer.x()] > 0;
        if (lassoPoints_.isEmpty() && insideSelection && event->modifiers().testFlag(Qt::ControlModifier)) {
            emit selectedPixelsDragStarted(event->modifiers().testFlag(Qt::AltModifier));
            if (floatingTransform_ && document_->activeLayerId) for (const Layer &layer : document_->layers) if (layer.id == *document_->activeLayerId) {
                pixelDragging_ = true; pixelDragStart_ = point; pixelDragOriginalOrigin_ = layer.transform.origin; pixelDragOffset_ = {}; setCursor(event->modifiers().testFlag(Qt::AltModifier) ? Qt::DragCopyCursor : Qt::SizeAllCursor); break;
            }
            event->accept(); return;
        }
        if (lassoPoints_.isEmpty() && selectionMode_ == 0 && document_->selection && !(event->modifiers() & (Qt::ShiftModifier | Qt::AltModifier))
            && insideSelection) {
            movingSelection_ = true; selectionAnchor_ = selectionCurrent_ = integer; event->accept(); return;
        }
        if (polygonalLasso_) {
            if (lassoPoints_.isEmpty()) lassoMode_ = effectiveSelectionMode(event->modifiers(), selectionMode_);
            if (lassoPoints_.size() >= 3 && QLineF(lassoPoints_.constFirst(), point).length() <= 8.0 / zoom_) {
                const QPolygonF completed = lassoPoints_; lassoPoints_.clear(); emit polygonSelectionRequested(completed, lassoMode_, selectionAntialiased_);
            } else lassoPoints_ << point;
            update(); setFocus(); event->accept(); return;
        }
        lassoPoints_ = {point}; selectionDragging_ = true; lassoMode_ = effectiveSelectionMode(event->modifiers(), selectionMode_);
        update(); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && document_ && tool_ == Tool::Text) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        for (auto it = document_->layers.crbegin(); it != document_->layers.crend(); ++it) {
            if (it->visible && !it->group && (it->text.has_value() || it->shape.value(QStringLiteral("kind")).toString() == QStringLiteral("Text"))
                && QRectF(it->transform.origin, it->transform.size).contains(point)) {
                emit textLayerEditRequested(it->id); event->accept(); return;
            }
        }
    }
    if (event->button() == Qt::LeftButton && document_ && (tool_ == Tool::Gradient || tool_ == Tool::Shape || tool_ == Tool::Text)) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        gradientHandle_ = 0;
        if (tool_ == Tool::Gradient && pendingGradient_) {
            const double tolerance = 10.0 / zoom_;
            if (QLineF(point, pendingGradient_->first).length() <= tolerance) gradientHandle_ = 1;
            else if (QLineF(point, pendingGradient_->second).length() <= tolerance) gradientHandle_ = 2;
        }
        if (tool_ == Tool::Gradient && gradientHandle_ != 0) {
            creationAnchor_ = pendingGradient_->first;
            creationCurrent_ = pendingGradient_->second;
        } else creationAnchor_ = creationCurrent_ = (tool_ == Tool::Shape ? snapDragPoint(point, event->modifiers()) : point);
        creationSquare_ = event->modifiers().testFlag(Qt::ShiftModifier);
        creationFromCenter_ = event->modifiers().testFlag(Qt::AltModifier);
        creationDragging_ = true; update(); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && document_ && (tool_ == Tool::Brush || tool_ == Tool::Eraser)) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        brushDrawing_ = true; cursorDocument_ = point;
        const bool continued = event->modifiers().testFlag(Qt::ShiftModifier) && lastBrushPoint_ && lastBrushLayerId_ == document_->activeLayerId && lastBrushMask_ == maskTarget_;
        emit brushStrokeStarted(continued ? *lastBrushPoint_ : point, tool_ == Tool::Eraser);
        if (continued) emit brushStrokeContinued(point);
        event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && document_ && tool_ == Tool::Clone) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        cursorDocument_ = point;
        if (event->modifiers().testFlag(Qt::AltModifier)) emit cloneSourceRequested(point);
        else { brushDrawing_ = true; const bool continued = event->modifiers().testFlag(Qt::ShiftModifier) && lastBrushPoint_ && lastBrushLayerId_ == document_->activeLayerId && lastBrushMask_ == maskTarget_; emit cloneStrokeStarted(continued ? *lastBrushPoint_ : point); if (continued) emit brushStrokeContinued(point); }
        event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && document_ && tool_ == Tool::Healing) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        brushDrawing_ = true; cursorDocument_ = point; const bool continued = event->modifiers().testFlag(Qt::ShiftModifier) && lastBrushPoint_ && lastBrushLayerId_ == document_->activeLayerId && lastBrushMask_ == maskTarget_; emit healingStrokeStarted(continued ? *lastBrushPoint_ : point); if (continued) emit brushStrokeContinued(point); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && document_ && tool_ == Tool::Blur) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        brushDrawing_ = true; cursorDocument_ = point; const bool continued = event->modifiers().testFlag(Qt::ShiftModifier) && lastBrushPoint_ && lastBrushLayerId_ == document_->activeLayerId && lastBrushMask_ == maskTarget_; emit blurStrokeStarted(continued ? *lastBrushPoint_ : point); if (continued) emit brushStrokeContinued(point); event->accept(); return;
    }
    if (event->button() == Qt::LeftButton && tool_ == Tool::Move && session_ && session_->canEditGuides()) {
        const QPointF pressPoint = (event->position() - canvasRect().topLeft()) / zoom_;
        if (auto hit = session_->hitGuide(pressPoint, 5.0 / zoom_)) {
            session_->beginGuideMove(*hit);
            guideDragging_ = true;
            setCursor(hit->axis == CanvasGuide::Axis::Vertical ? Qt::SplitHCursor : Qt::SplitVCursor);
            update();
            event->accept();
            return;
        }
    }
    if (event->button() == Qt::LeftButton && tool_ == Tool::Move && document_ && document_->activeLayerId) {
        snapGuideX_.reset(); snapGuideY_.reset();
        const QPointF pressPoint = (event->position() - canvasRect().topLeft()) / zoom_;
        if (pendingDistortion_) {
            const double tolerance = 8.0 / zoom_;
            QPoint handle;
            const std::array<QPoint, 8> signs = {QPoint(-1,-1), QPoint(0,-1), QPoint(1,-1), QPoint(1,0),
                                                 QPoint(1,1), QPoint(0,1), QPoint(-1,1), QPoint(-1,0)};
            for (int i = 0; i < 8; ++i) {
                const QPointF position = i % 2 == 0 ? distortionCorners_[size_t(i / 2)]
                    : (distortionCorners_[size_t(i / 2)] + distortionCorners_[size_t((i / 2 + 1) % 4)]) / 2;
                if (QLineF(pressPoint, position).length() <= tolerance) { handle = signs[size_t(i)]; break; }
            }
            QPolygonF shape; for (const QPointF &point : distortionCorners_) shape << point;
            if (!handle.isNull() || shape.containsPoint(pressPoint, Qt::OddEvenFill)) {
                movingLayer_ = true; transformDrag_ = TransformDrag::Distort; moveStartDocument_ = pressPoint;
                distortionOriginal_ = distortionCorners_; resizeSign_ = handle; distortMoveAll_ = handle.isNull();
                setCursor(distortMoveAll_ ? Qt::SizeAllCursor : Qt::CrossCursor); event->accept(); return;
            }
            resolvePendingDistortion(true);
        }
        std::optional<QUuid> underPointer;
        for (auto layer = document_->layers.crbegin(); layer != document_->layers.crend(); ++layer) {
            if (layer->visible && !layer->group && !layer->image.isNull() && transformContains(layer->transform, pressPoint)) { underPointer = layer->id; break; }
        }
        const QRectF currentGroupBounds = transformsAsGroup() && !transformMask_ ? selectedLayersBounds() : QRectF();
        const Layer *currentActive = nullptr;
        for (const Layer &layer : document_->layers) if (layer.id == *document_->activeLayerId) { currentActive = &layer; break; }
        const bool activeContains = currentActive && !currentActive->group && transformContains(transformMask_ ? currentActive->maskPlacement.value_or(currentActive->transform) : currentActive->transform, pressPoint);
        const bool groupContains = !currentGroupBounds.isEmpty() && currentGroupBounds.contains(pressPoint);
        const bool forcePick = event->modifiers().testFlag(Qt::ControlModifier);
        if (underPointer && *underPointer != *document_->activeLayerId
            // Control flips Auto Select for as long as it is held (mac bca8f13): with the box off it picks the layer under
            // the pointer; with it on, a press drags the active layer instead of picking.
            && (forcePick ? !transformAutoSelect_ : (transformAutoSelect_ && !activeContains && !groupContains))) {
            emit layerSelectionRequested(*underPointer);
        }
        const QRectF groupBounds = transformsAsGroup() && !transformMask_ ? selectedLayersBounds() : QRectF();
        if (!groupBounds.isEmpty()) {
            const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
            const double tolerance = 8.0 / zoom_;
            const QPointF rotationPoint(groupBounds.center().x(), groupBounds.top() - 28.0 / zoom_);
            QPoint handle;
            const std::array<QPoint, 8> handles = {QPoint(-1,-1), QPoint(0,-1), QPoint(1,-1), QPoint(-1,0),
                                                   QPoint(1,0), QPoint(-1,1), QPoint(0,1), QPoint(1,1)};
            for (const QPoint candidate : handles) {
                const QPointF position(groupBounds.center().x() + candidate.x() * groupBounds.width() / 2.0,
                                       groupBounds.center().y() + candidate.y() * groupBounds.height() / 2.0);
                if (QLineF(point, position).length() <= tolerance) { handle = candidate; break; }
            }
            TransformDrag drag = TransformDrag::None;
            if (transformControlsVisible() && QLineF(point, rotationPoint).length() <= tolerance) drag = TransformDrag::Rotate;
            else if (transformControlsVisible() && !handle.isNull()) drag = event->modifiers().testFlag(Qt::ControlModifier) ? TransformDrag::Distort : TransformDrag::Resize;
            else if (groupBounds.contains(point) || !transformAutoSelect_ || forcePick || !underPointer) drag = TransformDrag::Move;
            if (drag != TransformDrag::None) {
                transformOriginals_.clear();
                maskPlacementOriginals_.clear();
                const QSet<QUuid> targets = transformLayerIds();
                for (const Layer &layer : document_->layers) {
                    if (!targets.contains(layer.id)) continue;
                    transformOriginals_.insert(layer.id, layer.transform);
                    if (layer.maskPlacement) maskPlacementOriginals_.insert(layer.id, *layer.maskPlacement);
                }
                movingLayer_ = true;
                transformDrag_ = drag;
                transformStart_.origin = groupBounds.topLeft();
                transformStart_.size = groupBounds.size();
                transformStart_.rotation = 0.0;
                moveStartDocument_ = point;
                moveStartOrigin_ = groupBounds.topLeft();
                rotateStartAngle_ = std::atan2(point.y() - groupBounds.center().y(), point.x() - groupBounds.center().x());
                if (drag == TransformDrag::Distort) {
                    distortionOriginal_ = {groupBounds.topLeft(), groupBounds.topRight(), groupBounds.bottomRight(), groupBounds.bottomLeft()};
                    distortionCorners_ = distortionOriginal_; resizeSign_ = handle; distortMoveAll_ = false; setCursor(Qt::CrossCursor);
                } else if (drag == TransformDrag::Resize) {
                    resizeSign_ = handle;
                    resizeOpposite_ = QPointF(groupBounds.center().x() - handle.x() * groupBounds.width() / 2.0,
                                              groupBounds.center().y() - handle.y() * groupBounds.height() / 2.0);
                    setCursor(handle.x() == handle.y() ? Qt::SizeFDiagCursor : handle.x() == -handle.y() ? Qt::SizeBDiagCursor
                                                       : handle.x() == 0 ? Qt::SizeVerCursor : Qt::SizeHorCursor);
                } else setCursor(drag == TransformDrag::Rotate ? Qt::CrossCursor : Qt::SizeAllCursor);
                duplicateOnMove_ = false; duplicateIssued_ = false;
                if (drag != TransformDrag::Distort) emit layerTransformStarted(false);
                event->accept();
                return;
            }
        }
        const auto it = std::find_if(document_->layers.begin(), document_->layers.end(), [this](const Layer &layer) { return layer.id == *document_->activeLayerId; });
        if (it != document_->layers.end() && !it->group && !it->image.isNull()) {
            const LayerTransform displayed=transformMask_?it->maskPlacement.value_or(it->transform):it->transform;
            const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
            QTransform transform;
            transform.translate(displayed.center().x(), displayed.center().y());
            transform.rotate(displayed.rotation);
            bool invertible = false;
            const QPointF local = transform.inverted(&invertible).map(point);
            const QRectF bounds(-displayed.size.width() / 2.0, -displayed.size.height() / 2.0,displayed.size.width(), displayed.size.height());
            const double tolerance = 8.0 / zoom_;
            const QPointF rotationPoint(0, bounds.top() - 28.0 / zoom_);
            if (transformControlsVisible() && invertible && QLineF(local, rotationPoint).length() <= tolerance) {
                movingLayer_ = true; transformDrag_ = TransformDrag::Rotate; transformStart_ = displayed; maskPlacementStart_=it->maskPlacement;
                duplicateOnMove_ = false; duplicateIssued_ = false; setCursor(Qt::CrossCursor); emit layerTransformStarted(false); event->accept(); return;
            }
            QPoint handle;
            const std::array<QPoint, 8> handles = {QPoint(-1,-1), QPoint(0,-1), QPoint(1,-1), QPoint(-1,0),
                                                   QPoint(1,0), QPoint(-1,1), QPoint(0,1), QPoint(1,1)};
            for (const QPoint candidate : handles) {
                const QPointF position(candidate.x() * bounds.width() / 2.0, candidate.y() * bounds.height() / 2.0);
                if (QLineF(local, position).length() <= tolerance) { handle = candidate; break; }
            }
            if (transformControlsVisible() && !handle.isNull()) {
                const bool distort = event->modifiers().testFlag(Qt::ControlModifier);
                movingLayer_ = true; transformDrag_ = distort ? TransformDrag::Distort : TransformDrag::Resize; transformStart_ = displayed; maskPlacementStart_=it->maskPlacement; resizeSign_ = handle;
                moveStartDocument_ = point;
                const QPointF oppositeLocal(-handle.x() * bounds.width() / 2.0, -handle.y() * bounds.height() / 2.0);
                resizeOpposite_ = transform.map(oppositeLocal);
                if (distort) {
                    distortionOriginal_ = {transform.map(bounds.topLeft()), transform.map(bounds.topRight()),
                                           transform.map(bounds.bottomRight()), transform.map(bounds.bottomLeft())};
                    distortionCorners_ = distortionOriginal_; distortMoveAll_ = false; setCursor(Qt::CrossCursor);
                } else setCursor(handle.x() == handle.y() ? Qt::SizeFDiagCursor : handle.x() == -handle.y() ? Qt::SizeBDiagCursor
                                                          : handle.x() == 0 ? Qt::SizeVerCursor : Qt::SizeHorCursor);
                duplicateOnMove_ = false; duplicateIssued_ = false; if (!distort) emit layerTransformStarted(false); event->accept(); return;
            }
            if (invertible && (bounds.contains(local) || !transformAutoSelect_ || forcePick || !underPointer)) {
                movingLayer_ = true;
                transformDrag_ = TransformDrag::Move;
                moveStartDocument_ = point;
                moveStartOrigin_ = displayed.origin;
                transformStart_ = displayed;
                maskPlacementStart_=it->maskPlacement;
                duplicateOnMove_ = event->modifiers().testFlag(Qt::AltModifier) && selectedLayerIds_.size() <= 1 && !transformMask_;
                duplicateIssued_ = false;
                setCursor(Qt::SizeAllCursor);
                emit layerTransformStarted(duplicateOnMove_);
                event->accept();
                return;
            }
        }
    }
    QWidget::mousePressEvent(event);
}

QPointF CanvasWidget::snapDragPoint(const QPointF &point, Qt::KeyboardModifiers modifiers)
{
    snapGuideX_.reset(); snapGuideY_.reset();
    if (!session_ || modifiers.testFlag(Qt::ControlModifier)) return point;
    return session_->snappedPoint(point, 10.0 / zoom_, &snapGuideX_, &snapGuideY_);
}

QPoint CanvasWidget::snapSelectionMoveEnd(const QPoint &current, Qt::KeyboardModifiers modifiers)
{
    snapGuideX_.reset(); snapGuideY_.reset();
    const QPoint offset = current - selectionAnchor_;
    if (!session_ || modifiers.testFlag(Qt::ControlModifier)) return current;
    const auto bounds = session_->selectionBounds();
    if (!bounds) return current;
    const QPointF snapped = session_->snappedSelectionOffset(QRectF(*bounds), QPointF(offset), 10.0 / zoom_, true, true, &snapGuideX_, &snapGuideY_);
    return selectionAnchor_ + snapped.toPoint();
}

void CanvasWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (document_) cursorDocument_ = (event->position() - canvasRect().topLeft()) / zoom_;
    if (samplingColor_ && cursorDocument_) {
        const QPoint pixel(qFloor(cursorDocument_->x()), qFloor(cursorDocument_->y()));
        if (QRect(QPoint(), renderedDocument_.size()).contains(pixel)) { sampleCurrent_ = renderedDocument_.pixelColor(pixel); emit colorSampleRequested(pixel); }
        update(); event->accept(); return;
    }
    if (guideDragging_ && session_ && session_->guideDrag()) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        const double pos = (session_->guideDrag()->axis == CanvasGuide::Axis::Vertical) ? point.x() : point.y();
        session_->moveGuideDrag(pos);
        update();
        event->accept();
        return;
    }
    if (hueTargetDragging_) {
        emit hueTargetDragged(event->position().x() - hueTargetStart_.x(), event->modifiers().testFlag(Qt::ControlModifier));
        setCursor(Qt::SizeHorCursor); event->accept(); return;
    }
    if (panning_) {
        const QPoint position = event->position().toPoint();
        panOffset_ += position - lastMousePosition_;
        lastMousePosition_ = position;
        fitPending_ = false;
        emit viewportChanged();
        update();
        event->accept();
        return;
    }
    if (pixelDragging_ && document_ && document_->activeLayerId) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        pixelDragOffset_ = QPoint(qRound(point.x() - pixelDragStart_.x()), qRound(point.y() - pixelDragStart_.y()));
        // Shift keeps moved pixels on a straight line, across or down, whichever the drag has gone further along (mac 60bde4f).
        if (event->modifiers().testFlag(Qt::ShiftModifier)) { if (std::abs(pixelDragOffset_.x()) >= std::abs(pixelDragOffset_.y())) pixelDragOffset_.setY(0); else pixelDragOffset_.setX(0); }
        for (Layer &layer : document_->layers) if (layer.id == *document_->activeLayerId) { layer.transform.origin = pixelDragOriginalOrigin_ + pixelDragOffset_; break; }
        renderDirty_ = true; update(); event->accept(); return;
    }
    if (cropDrag_ != CropDrag::None && document_ && tool_ == Tool::Crop) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        const QPointF delta = point - cropStart_;
        const bool symmetric = event->modifiers().testFlag(Qt::AltModifier);
        QRectF next;
        if (cropDrag_ == CropDrag::Create) {
            double dx = delta.x(), dy = delta.y();
            if (cropRatio_ > 0) { if (std::abs(dx) > std::abs(dy) * cropRatio_) dy = (dy < 0 ? -1 : 1) * std::abs(dx) / cropRatio_; else dx = (dx < 0 ? -1 : 1) * std::abs(dy) * cropRatio_; }
            next = symmetric ? QRectF(cropStart_.x() - std::abs(dx), cropStart_.y() - std::abs(dy), std::abs(dx) * 2, std::abs(dy) * 2)
                             : QRectF(cropStart_, cropStart_ + QPointF(dx, dy)).normalized();
        } else if (cropDrag_ == CropDrag::Move) next = cropOriginal_.translated(delta);
        else {
            const QPointF initial = cropOriginal_.center() + QPointF(cropHandle_.x() * cropOriginal_.width() / 2, cropHandle_.y() * cropOriginal_.height() / 2);
            const QPointF anchor = symmetric ? cropOriginal_.center() : cropOriginal_.center() - QPointF(cropHandle_.x() * cropOriginal_.width() / 2, cropHandle_.y() * cropOriginal_.height() / 2);
            const QPointF span = (initial + delta - anchor) * (symmetric ? 2.0 : 1.0);
            double width = cropHandle_.x() ? std::max(1.0, span.x() * cropHandle_.x()) : cropOriginal_.width();
            double height = cropHandle_.y() ? std::max(1.0, span.y() * cropHandle_.y()) : cropOriginal_.height();
            if (cropRatio_ > 0) { const double factor = !cropHandle_.x() ? height / cropOriginal_.height() : !cropHandle_.y() ? width / cropOriginal_.width() : std::max(width / cropOriginal_.width(), height / cropOriginal_.height()); width = cropOriginal_.width() * factor; height = width / cropRatio_; }
            const QPointF center = symmetric ? anchor : anchor + QPointF(cropHandle_.x() * width / 2, cropHandle_.y() * height / 2);
            next = QRectF(center - QPointF(width / 2, height / 2), QSizeF(width, height));
        }
        next = next.normalized(); snapGuideX_.reset(); snapGuideY_.reset();
        if (!event->modifiers().testFlag(Qt::ControlModifier)) {
            QVector<double> xs;
            QVector<double> ys;
            if (session_) {
                xs = session_->cropSnapTargetsX();
                ys = session_->cropSnapTargetsY();
            } else {
                xs = {0.0, double(document_->canvasSize.width())};
                ys = {0.0, double(document_->canvasSize.height())};
                for (const Layer &layer : document_->layers) if (layer.visible && !layer.group && !layer.image.isNull()) { const QRectF bounds = transformedBounds(layer.transform); xs << std::round(bounds.left()) << std::round(bounds.right()); ys << std::round(bounds.top()) << std::round(bounds.bottom()); }
            }
            const auto nearest = [this](double value, const QVector<double> &targets) -> std::optional<double> { std::optional<double> best; for (double target : targets) if (std::abs(target-value) <= 8.0/zoom_ && (!best || std::abs(target-value) < std::abs(*best-value))) best=target; return best; };
            if (cropDrag_ == CropDrag::Move) {
                double dx = 0, dy = 0; std::optional<double> gx, gy;
                for (double edge : {next.left(), next.right()}) if (auto target=nearest(edge,xs);target&&(!gx||std::abs(*target-edge)<std::abs(dx))){dx=*target-edge;gx=target;}
                for (double edge : {next.top(), next.bottom()}) if (auto target=nearest(edge,ys);target&&(!gy||std::abs(*target-edge)<std::abs(dy))){dy=*target-edge;gy=target;}
                next.translate(dx,dy);snapGuideX_=gx;snapGuideY_=gy;
            } else if (cropRatio_ <= 0) {
                if (cropHandle_.x() <= 0 || cropDrag_ == CropDrag::Create) if (auto x=nearest(next.left(),xs)) { next.setLeft(*x); snapGuideX_=x; }
                if (cropHandle_.x() >= 0 || cropDrag_ == CropDrag::Create) if (auto x=nearest(next.right(),xs)) { next.setRight(*x); snapGuideX_=x; }
                if (cropHandle_.y() <= 0 || cropDrag_ == CropDrag::Create) if (auto y=nearest(next.top(),ys)) { next.setTop(*y); snapGuideY_=y; }
                if (cropHandle_.y() >= 0 || cropDrag_ == CropDrag::Create) if (auto y=nearest(next.bottom(),ys)) { next.setBottom(*y); snapGuideY_=y; }
            }
        }
        next = QRectF(std::round(next.x()), std::round(next.y()), std::max(1.0, std::round(next.width())), std::max(1.0, std::round(next.height())));
        if (next.width() <= 30000 && next.height() <= 30000 && std::abs(next.x()) <= 1000000 && std::abs(next.y()) <= 1000000) cropRect_ = next;
        update(); event->accept(); return;
    }
    if (movingLayer_ && document_ && document_->activeLayerId) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        if (duplicateOnMove_ && !duplicateIssued_ && QLineF(point, moveStartDocument_).length() > 0) {
            duplicateIssued_ = true; emit duplicateLayerForTransformRequested();
        }
        if (transformDrag_ == TransformDrag::Distort) {
            QPointF delta = point - moveStartDocument_;
            if (event->modifiers().testFlag(Qt::ShiftModifier)) { if (std::abs(delta.x()) >= std::abs(delta.y())) delta.setY(0); else delta.setX(0); }
            distortionCorners_ = distortionOriginal_;
            QVector<int> moved;
            if (distortMoveAll_) moved = {0, 1, 2, 3};
            else if (resizeSign_ == QPoint(-1, -1)) moved = {0};
            else if (resizeSign_ == QPoint(1, -1)) moved = {1};
            else if (resizeSign_ == QPoint(1, 1)) moved = {2};
            else if (resizeSign_ == QPoint(-1, 1)) moved = {3};
            else if (resizeSign_ == QPoint(0, -1)) moved = {0, 1};
            else if (resizeSign_ == QPoint(1, 0)) moved = {1, 2};
            else if (resizeSign_ == QPoint(0, 1)) moved = {2, 3};
            else if (resizeSign_ == QPoint(-1, 0)) moved = {3, 0};
            for (int index : moved) distortionCorners_[size_t(index)] += delta;
            update(); event->accept(); return;
        }
        if (!transformOriginals_.isEmpty()) {
            LayerTransform group = transformStart_;
            double rotationDelta = 0.0;
            if (transformDrag_ == TransformDrag::Move) {
                QPointF delta = point - moveStartDocument_;
                if (event->modifiers().testFlag(Qt::ShiftModifier)) { if (std::abs(delta.x()) >= std::abs(delta.y())) delta.setY(0); else delta.setX(0); }
                group.origin = moveStartOrigin_ + delta;
                group.origin = QPointF(std::round(group.origin.x()), std::round(group.origin.y()));
                if (!event->modifiers().testFlag(Qt::ControlModifier)) {
                    const SnapResult snap = snappedOffset(*document_, QSet<QUuid>(transformOriginals_.keyBegin(), transformOriginals_.keyEnd()),
                                                          QRectF(group.origin, group.size), 10.0 / zoom_, session_);
                    group.origin += snap.offset; snapGuideX_ = snap.x; snapGuideY_ = snap.y;
                } else { snapGuideX_.reset(); snapGuideY_.reset(); }
            } else if (transformDrag_ == TransformDrag::Rotate) {
                snapGuideX_.reset(); snapGuideY_.reset();
                const double angle = std::atan2(point.y() - transformStart_.center().y(), point.x() - transformStart_.center().x());
                rotationDelta = std::round((angle - rotateStartAngle_) * 180.0 / M_PI);
                if (event->modifiers().testFlag(Qt::ShiftModifier)) rotationDelta = std::round(rotationDelta / 15.0) * 15.0;
            } else if (transformDrag_ == TransformDrag::Resize) {
                const bool fromCenter = event->modifiers().testFlag(Qt::AltModifier);
                const bool lock = lockTransformRatio_ != event->modifiers().testFlag(Qt::ShiftModifier);
                QPointF resizePoint = point;
                snapGuideX_.reset(); snapGuideY_.reset();
                if (session_ && !event->modifiers().testFlag(Qt::ControlModifier))   // Control drags freely
                    resizePoint = session_->snappedResizePoint(point, transformStart_, moveStartDocument_, resizeSign_, fromCenter, lock,
                                                               QSet<QUuid>(transformOriginals_.keyBegin(), transformOriginals_.keyEnd()),
                                                               10.0 / zoom_, &snapGuideX_, &snapGuideY_);
                const LayerTransform resized = resizedByHandle(transformStart_, resizeSign_, moveStartDocument_, resizePoint, fromCenter, lock);
                group.size = resized.size; group.origin = resized.origin;
            }
            const double scaleX = group.size.width() / transformStart_.size.width();
            const double scaleY = group.size.height() / transformStart_.size.height();
            for (Layer &layer : document_->layers) {
                const auto originalIt = transformOriginals_.constFind(layer.id);
                if (originalIt == transformOriginals_.cend()) continue;
                const LayerTransform original = *originalIt;
                LayerTransform edited = original;
                if (transformDrag_ == TransformDrag::Rotate) {
                    QTransform rotation;
                    rotation.translate(transformStart_.center().x(), transformStart_.center().y());
                    rotation.rotate(rotationDelta);
                    rotation.translate(-transformStart_.center().x(), -transformStart_.center().y());
                    const QPointF center = rotation.map(original.center());
                    edited.origin = center - QPointF(original.size.width() / 2.0, original.size.height() / 2.0);
                    edited.rotation = original.rotation + rotationDelta;
                } else {
                    const QPointF relative = original.center() - transformStart_.origin;
                    const QPointF center(group.origin.x() + relative.x() * scaleX,
                                         group.origin.y() + relative.y() * scaleY);
                    edited.size = QSizeF(std::max(1.0, original.size.width() * scaleX),
                                        std::max(1.0, original.size.height() * scaleY));
                    edited.origin = center - QPointF(edited.size.width() / 2.0, edited.size.height() / 2.0);
                }
                layer.transform = edited;
                if (!layer.mask.isNull() && layer.mask.size() != QSize(1, 1) && layer.maskLinked) {
                    const auto maskIt = maskPlacementOriginals_.constFind(layer.id);
                    if (maskIt != maskPlacementOriginals_.cend()) layer.maskPlacement = maskIt->following(original, edited);
                    else layer.maskPlacement.reset();
                }
            }
            renderDirty_ = true; update();
            emit layerTransformChanged();
            event->accept();
            return;
        }
        for (Layer &layer : document_->layers) if (layer.id == *document_->activeLayerId) {
            if(transformMask_&&!layer.maskPlacement)layer.maskPlacement=layer.transform;
            LayerTransform &edited=transformMask_?*layer.maskPlacement:layer.transform;
            if (transformDrag_ == TransformDrag::Move) {
                QPointF delta = point - moveStartDocument_;
                if (event->modifiers().testFlag(Qt::ShiftModifier)) { if (std::abs(delta.x()) >= std::abs(delta.y())) delta.setY(0); else delta.setX(0); }
                edited.origin = moveStartOrigin_ + delta;
                edited.origin = QPointF(std::round(edited.origin.x()), std::round(edited.origin.y()));
                if (!event->modifiers().testFlag(Qt::ControlModifier)) {
                    const SnapResult snap = snappedOffset(*document_, selectedLayerIds_.isEmpty() ? QSet<QUuid>{layer.id} : selectedLayerIds_,
                                                          transformedBounds(edited), 10.0 / zoom_, session_);
                    edited.origin += snap.offset; snapGuideX_ = snap.x; snapGuideY_ = snap.y;
                } else { snapGuideX_.reset(); snapGuideY_.reset(); }
            }
            else if (transformDrag_ == TransformDrag::Rotate) {
                snapGuideX_.reset(); snapGuideY_.reset();
                const QPointF delta = point - transformStart_.center();
                edited.rotation = std::round(std::atan2(delta.y(), delta.x()) * 180.0 / M_PI + 90.0);
                if (event->modifiers().testFlag(Qt::ShiftModifier)) edited.rotation = std::round(edited.rotation / 15.0) * 15.0;
            } else if (transformDrag_ == TransformDrag::Resize) {
                const bool fromCenter = event->modifiers().testFlag(Qt::AltModifier);
                const bool lock = lockTransformRatio_ != event->modifiers().testFlag(Qt::ShiftModifier);
                QPointF resizePoint = point;
                snapGuideX_.reset(); snapGuideY_.reset();
                if (session_ && !transformMask_ && !event->modifiers().testFlag(Qt::ControlModifier))
                    resizePoint = session_->snappedResizePoint(point, transformStart_, moveStartDocument_, resizeSign_, fromCenter, lock,
                                                               selectedLayerIds_.isEmpty() ? QSet<QUuid>{layer.id} : selectedLayerIds_,
                                                               10.0 / zoom_, &snapGuideX_, &snapGuideY_);
                const LayerTransform resized = resizedByHandle(transformStart_, resizeSign_, moveStartDocument_, resizePoint, fromCenter, lock);
                edited.size = resized.size; edited.origin = resized.origin;
            }
            if(!transformMask_&&!layer.mask.isNull()&&layer.mask.size()!=QSize(1,1)){if(layer.maskLinked){if(maskPlacementStart_)layer.maskPlacement=maskPlacementStart_->following(transformStart_,layer.transform);else layer.maskPlacement.reset();}else layer.maskPlacement=maskPlacementStart_.value_or(transformStart_);}
            break;
        }
        renderDirty_ = true; update();
        emit layerTransformChanged();
        event->accept();
        return;
    }
    if (movingSelection_) {
        selectionCurrent_ = snapSelectionMoveEnd(((event->position() - canvasRect().topLeft()) / zoom_).toPoint(), event->modifiers());
        update(); event->accept(); return;
    }
    if (selectionDragging_) {
        if (tool_ == Tool::Lasso && !polygonalLasso_) {
            const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
            if (lassoPoints_.isEmpty() || QLineF(lassoPoints_.constLast(), point).length() >= .25) lassoPoints_ << point;
            update(); event->accept(); return;
        }
        selectionCurrent_ = ((event->position() - canvasRect().topLeft()) / zoom_).toPoint();
        if (tool_ == Tool::Marquee) selectionCurrent_ = snapDragPoint(QPointF(selectionCurrent_), event->modifiers()).toPoint();
        update(); event->accept(); return;
    }
    if (brushDrawing_) {
        emit brushStrokeContinued(*cursorDocument_); update(); event->accept(); return;
    }
    if (creationDragging_) {
        if (tool_ == Tool::Gradient && gradientHandle_ == 1) creationAnchor_ = *cursorDocument_;
        else creationCurrent_ = tool_ == Tool::Shape ? snapDragPoint(*cursorDocument_, event->modifiers()) : *cursorDocument_;
        creationSquare_ = event->modifiers().testFlag(Qt::ShiftModifier);
        creationFromCenter_ = event->modifiers().testFlag(Qt::AltModifier);
        if (tool_ == Tool::Gradient && QLineF(creationAnchor_, creationCurrent_).length() >= .5)
            emit gradientRequested(creationAnchor_, creationCurrent_);
        update(); event->accept(); return;
    }
    const QPointF hover = cursorDocument_.value_or(QPointF());
    if (hueTargeting_) setCursor(Qt::SizeHorCursor);
    else if (tool_ == Tool::Brush || tool_ == Tool::Eraser || tool_ == Tool::Healing || tool_ == Tool::Blur
        || (tool_ == Tool::Clone && cloneSource_ && !event->modifiers().testFlag(Qt::AltModifier))) setCursor(Qt::BlankCursor);
    else if (tool_ == Tool::Hand) setCursor(Qt::OpenHandCursor);
    else if (tool_ == Tool::Zoom) setCursor(zoomCursor(event->modifiers().testFlag(Qt::AltModifier)));
    else if (tool_ == Tool::Text) setCursor(Qt::IBeamCursor);
    else if (tool_ == Tool::Eyedropper) setCursor(eyedropperCursor());
    else if ((tool_ == Tool::Marquee || tool_ == Tool::Lasso || tool_ == Tool::Wand) && document_) {
        const QPoint point = hover.toPoint();
        const bool insideSelection = document_->selection && QRect(QPoint(), document_->selection->size()).contains(point)
            && document_->selection->convertToFormat(QImage::Format_Grayscale8).constScanLine(point.y())[point.x()] > 0;
        if (insideSelection && event->modifiers().testFlag(Qt::ControlModifier))
            setCursor(event->modifiers().testFlag(Qt::AltModifier) ? Qt::DragCopyCursor : Qt::SizeAllCursor);
        else setCursor(Qt::CrossCursor);
    } else if (tool_ == Tool::Gradient || tool_ == Tool::Shape || tool_ == Tool::Clone) setCursor(Qt::CrossCursor);
    else if (tool_ == Tool::Crop && document_) {
        const QRectF crop = cropRect_.value_or(QRectF(QPointF(), QSizeF(document_->canvasSize))).normalized();
        const double tolerance = 8.0 / zoom_; QPoint handle;
        const std::array<QPair<QPoint,QPointF>,8> handles{{{QPoint(-1,-1),crop.topLeft()},{QPoint(0,-1),QPointF(crop.center().x(),crop.top())},{QPoint(1,-1),crop.topRight()},
            {QPoint(1,0),QPointF(crop.right(),crop.center().y())},{QPoint(1,1),crop.bottomRight()},{QPoint(0,1),QPointF(crop.center().x(),crop.bottom())},{QPoint(-1,1),crop.bottomLeft()},{QPoint(-1,0),QPointF(crop.left(),crop.center().y())}}};
        for (const auto &[sign,position] : handles) if (QLineF(hover,position).length()<=tolerance){handle=sign;break;}
        if (!handle.isNull()) setCursor(handle.x()==handle.y()?Qt::SizeFDiagCursor:handle.x()==-handle.y()?Qt::SizeBDiagCursor:handle.x()==0?Qt::SizeVerCursor:Qt::SizeHorCursor);
        else setCursor(cropRect_ && crop.contains(hover) ? Qt::SizeAllCursor : Qt::CrossCursor);
    } else if (tool_ == Tool::Move && document_) {
        if (session_ && session_->canEditGuides()) {
            auto hit = session_->hitGuide(hover, 5.0 / zoom_);
            if (hit) {
                setCursor(hit->axis == CanvasGuide::Axis::Vertical ? Qt::SplitHCursor : Qt::SplitVCursor);
                update();
                QWidget::mouseMoveEvent(event);
                return;
            }
        }
        if (document_->activeLayerId) {
        QRectF group = selectedLayerIds_.size()>1&&!transformMask_?selectedLayersBounds():QRectF();
        LayerTransform placement; bool hasPlacement=false;
        if (group.isEmpty()) for(const Layer &layer:document_->layers)if(layer.id==*document_->activeLayerId&&!layer.group&&!layer.image.isNull()){placement=transformMask_?layer.maskPlacement.value_or(layer.transform):layer.transform;hasPlacement=true;break;}
        bool underPointer=false;
        for(auto layer=document_->layers.crbegin();layer!=document_->layers.crend();++layer)if(layer->visible&&!layer->group&&!layer->image.isNull()&&transformContains(layer->transform,hover)){underPointer=true;break;}
        QPoint handle; const double tolerance=8.0/zoom_;
        if (!group.isEmpty()) {
            const QPointF rotationPoint(group.center().x(),group.top()-28.0/zoom_);
            const std::array<QPoint,8> signs={QPoint(-1,-1),QPoint(0,-1),QPoint(1,-1),QPoint(-1,0),QPoint(1,0),QPoint(-1,1),QPoint(0,1),QPoint(1,1)};
            if (transformControlsVisible()) for(const QPoint &sign:signs){const QPointF position=group.center()+QPointF(sign.x()*group.width()/2,sign.y()*group.height()/2);if(QLineF(hover,position).length()<=tolerance){handle=sign;break;}}
            if(transformControlsVisible()&&QLineF(hover,rotationPoint).length()<=tolerance)setCursor(Qt::CrossCursor);
            else if(!handle.isNull())setCursor(event->modifiers().testFlag(Qt::ControlModifier)?Qt::CrossCursor:handle.x()==handle.y()?Qt::SizeFDiagCursor:handle.x()==-handle.y()?Qt::SizeBDiagCursor:handle.x()==0?Qt::SizeVerCursor:Qt::SizeHorCursor);
            else setCursor((group.contains(hover)||!transformAutoSelect_||!underPointer)?(event->modifiers().testFlag(Qt::AltModifier)?Qt::DragCopyCursor:Qt::SizeAllCursor):Qt::ArrowCursor);
        } else if (hasPlacement) {
            QTransform transform;transform.translate(placement.center().x(),placement.center().y());transform.rotate(placement.rotation);bool ok=false;const QPointF local=transform.inverted(&ok).map(hover);const QRectF box(-placement.size.width()/2,-placement.size.height()/2,placement.size.width(),placement.size.height());
            const QPointF rotationPoint(0,box.top()-28.0/zoom_);
            const std::array<QPoint,8> signs={QPoint(-1,-1),QPoint(0,-1),QPoint(1,-1),QPoint(-1,0),QPoint(1,0),QPoint(-1,1),QPoint(0,1),QPoint(1,1)};
            if(transformControlsVisible()&&ok)for(const QPoint &sign:signs){const QPointF position(sign.x()*box.width()/2,sign.y()*box.height()/2);if(QLineF(local,position).length()<=tolerance){handle=sign;break;}}
            if(transformControlsVisible()&&ok&&QLineF(local,rotationPoint).length()<=tolerance)setCursor(Qt::CrossCursor);
            else if(!handle.isNull())setCursor(event->modifiers().testFlag(Qt::ControlModifier)?Qt::CrossCursor:handle.x()==handle.y()?Qt::SizeFDiagCursor:handle.x()==-handle.y()?Qt::SizeBDiagCursor:handle.x()==0?Qt::SizeVerCursor:Qt::SizeHorCursor);
            else setCursor(ok&&(box.contains(local)||!transformAutoSelect_||!underPointer)?(event->modifiers().testFlag(Qt::AltModifier)?Qt::DragCopyCursor:Qt::SizeAllCursor):Qt::ArrowCursor);
        } else setCursor(Qt::ArrowCursor);
        } else setCursor(Qt::ArrowCursor);
    } else unsetCursor();
    update();
    QWidget::mouseMoveEvent(event);
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (guideDragging_ && event->button() == Qt::LeftButton) {
        guideDragging_ = false;
        if (session_) {
            const bool isOverRuler = session_->showsRulers() && (event->position().x() < 0 || event->position().y() < 0);
            session_->finishGuideDrag(isOverRuler);
        }
        setTool(tool_);
        update();
        emit guideChanged();
        event->accept();
        return;
    }
    if (samplingColor_ && event->button() == Qt::LeftButton) { samplingColor_ = false; update(); event->accept(); return; }
    if (hueTargetDragging_ && event->button() == Qt::LeftButton) {
        hueTargetDragging_ = false; emit hueTargetFinished();
        setCursor(hueTargeting_ ? Qt::SizeHorCursor : Qt::ArrowCursor); event->accept(); return;
    }
    if (panning_ && (event->button() == Qt::MiddleButton || event->button() == Qt::RightButton
                     || event->button() == Qt::LeftButton)) {
        panning_ = false;
        unsetCursor();
        event->accept();
        return;
    }
    if (cropDrag_ != CropDrag::None && event->button() == Qt::LeftButton) {
        cropDrag_ = CropDrag::None; snapGuideX_.reset(); snapGuideY_.reset();
        if (cropRect_) emit pendingCropChanged(true);
        update(); event->accept(); return;
    }
    if (pixelDragging_ && event->button() == Qt::LeftButton) {
        pixelDragging_ = false; unsetCursor(); emit floatingTransformCommitRequested(); event->accept(); return;
    }
    if (movingLayer_ && event->button() == Qt::LeftButton) {
        const bool distorted = transformDrag_ == TransformDrag::Distort;
        movingLayer_ = false;
        transformDrag_ = TransformDrag::None;
        transformOriginals_.clear();
        maskPlacementOriginals_.clear();
        duplicateOnMove_ = false; duplicateIssued_ = false;
        snapGuideX_.reset(); snapGuideY_.reset();
        unsetCursor();
        update();
        if (distorted) { pendingDistortion_ = true; emit pendingDistortionChanged(true); }
        else emit layerTransformFinished();
        event->accept();
        return;
    }
    if (movingSelection_ && event->button() == Qt::LeftButton) {
        movingSelection_ = false;
        selectionCurrent_ = snapSelectionMoveEnd(((event->position() - canvasRect().topLeft()) / zoom_).toPoint(), event->modifiers());
        snapGuideX_.reset(); snapGuideY_.reset();
        const QPoint offset = selectionCurrent_ - selectionAnchor_; if (!offset.isNull()) emit selectionMoveRequested(offset);
        update(); event->accept(); return;
    }
    if (selectionDragging_ && event->button() == Qt::LeftButton) {
        selectionDragging_ = false;
        const int mode = effectiveSelectionMode(event->modifiers(), selectionMode_);
        if (tool_ == Tool::Lasso && !polygonalLasso_) {
            const QPolygonF points = lassoPoints_; lassoPoints_.clear();
            emit polygonSelectionRequested(points, lassoMode_, selectionAntialiased_); update(); event->accept(); return;
        }
        selectionCurrent_ = ((event->position() - canvasRect().topLeft()) / zoom_).toPoint();
        if (tool_ == Tool::Marquee) selectionCurrent_ = snapDragPoint(QPointF(selectionCurrent_), event->modifiers()).toPoint();
        snapGuideX_.reset(); snapGuideY_.reset();
        const QRect rect = QRect(selectionAnchor_, selectionCurrent_).normalized();
        if (tool_ == Tool::Crop) emit cropRequested(rect);
        else if (ellipticalMarquee_) emit ellipticalSelectionRequested(rect, mode, selectionAntialiased_);
        else emit rectangularSelectionRequested(rect, mode);
        update(); event->accept(); return;
    }
    if (brushDrawing_ && event->button() == Qt::LeftButton) {
        brushDrawing_ = false;
        if (cursorDocument_ && document_ && document_->activeLayerId) { lastBrushPoint_ = *cursorDocument_; lastBrushLayerId_ = document_->activeLayerId; lastBrushMask_ = maskTarget_; }
        emit brushStrokeFinished(); update(); event->accept(); return;
    }
    if (creationDragging_ && event->button() == Qt::LeftButton) {
        creationDragging_ = false;
        if (tool_ == Tool::Gradient && gradientHandle_ == 1) creationAnchor_ = (event->position() - canvasRect().topLeft()) / zoom_;
        else creationCurrent_ = tool_ == Tool::Shape ? snapDragPoint((event->position() - canvasRect().topLeft()) / zoom_, event->modifiers())
                                                     : (event->position() - canvasRect().topLeft()) / zoom_;
        if (tool_ == Tool::Shape) { snapGuideX_.reset(); snapGuideY_.reset(); }
        if (tool_ == Tool::Gradient) {
            if (QLineF(creationAnchor_, creationCurrent_).length() < .5) {
                pendingGradient_.reset(); emit gradientCancelRequested();
            } else {
                pendingGradient_ = qMakePair(creationAnchor_, creationCurrent_);
                emit gradientRequested(creationAnchor_, creationCurrent_);
            }
        } else if (tool_ == Tool::Shape && shapeKind_ == ShapeKind::Line) {
            QPointF lineStart = creationAnchor_;
            QPointF lineEnd = creationCurrent_;
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                const double dx = lineEnd.x() - lineStart.x();
                const double dy = lineEnd.y() - lineStart.y();
                const double angle = std::round(std::atan2(dy, dx) / (M_PI / 4.0)) * (M_PI / 4.0);
                const double len = std::hypot(dx, dy);
                lineEnd = QPointF(lineStart.x() + std::cos(angle) * len, lineStart.y() + std::sin(angle) * len);
            }
            if (QLineF(lineStart, lineEnd).length() >= 1.0) {
                const double thickness = std::max(1.0, shapeLineWidth_);
                const double minX = std::min(lineStart.x(), lineEnd.x());
                const double minY = std::min(lineStart.y(), lineEnd.y());
                const double spanW = std::abs(lineEnd.x() - lineStart.x());
                const double spanH = std::abs(lineEnd.y() - lineStart.y());
                const double pad = thickness / 2.0;
                const QRectF lineBox(minX - pad, minY - pad, std::max(1.0, spanW + thickness), std::max(1.0, spanH + thickness));
                emit shapeCreated(ShapeKind::Line, lineBox, thickness, 0.0, lineStart, lineEnd);
            }
        } else {
            const QRectF shape = dragBox(creationAnchor_, creationCurrent_, event->modifiers().testFlag(Qt::ShiftModifier),
                                          event->modifiers().testFlag(Qt::AltModifier));
            if (tool_ == Tool::Text) {
                const QRectF box = shape.width() >= 12 && shape.height() >= 12 ? shape : QRectF(creationAnchor_, QSizeF(320, 100));
                emit textBoxRequested(box, shape.width() >= 12 && shape.height() >= 12);
            } else if (shape.width() >= 1 && shape.height() >= 1) {
                emit shapeCreated(shapeKind_, shape, 0.0, shapeCornerRadius_, std::nullopt, std::nullopt);
                emit shapeRequested(shape, shapeKind_ == ShapeKind::Ellipse, shapeCornerRadius_);
            }
        }
        update(); event->accept(); return;
    }
    QWidget::mouseReleaseEvent(event);
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (editorInteractionBlocked_) { event->accept(); return; }
    if (event->button() == Qt::LeftButton && document_ && (tool_ == Tool::Move || tool_ == Tool::Text)) {
        const QPointF point = (event->position() - canvasRect().topLeft()) / zoom_;
        for (auto it = document_->layers.crbegin(); it != document_->layers.crend(); ++it) {
            if (it->visible && !it->group && (it->text.has_value() || it->shape.value(QStringLiteral("kind")).toString() == QStringLiteral("Text"))
                && QRectF(it->transform.origin, it->transform.size).contains(point)) {
                if (tool_ != Tool::Text) setTool(Tool::Text);
                emit textLayerEditRequested(it->id); event->accept(); return;
            }
        }
    }
    if (event->button() == Qt::LeftButton && tool_ == Tool::Lasso && polygonalLasso_ && lassoPoints_.size() >= 3) {
        const QPolygonF completed = lassoPoints_; lassoPoints_.clear(); emit polygonSelectionRequested(completed, lassoMode_, selectionAntialiased_); update(); event->accept(); return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void CanvasWidget::leaveEvent(QEvent *event)
{
    cursorDocument_.reset();
    if (!panning_) setCursor(Qt::ArrowCursor);
    update();
    QWidget::leaveEvent(event);
}

void CanvasWidget::keyPressEvent(QKeyEvent *event)
{
    if (editorInteractionBlocked_) { QWidget::keyPressEvent(event); return; }

    const auto translated = ShortcutManager::instance().translateCanvasKeyEvent(event);
    if (translated.suppressed) {
        event->accept();
        return;
    }
    const int effectiveKey = translated.key;
    const Qt::KeyboardModifiers effectiveModifiers = translated.modifiers;

    if (guideDragging_ && effectiveKey == Qt::Key_Escape) {
        guideDragging_ = false;
        if (session_) session_->cancelGuideDrag();
        setTool(tool_);
        update();
        emit guideChanged();
        event->accept();
        return;
    }
    if (brushDrawing_ && effectiveKey == Qt::Key_Escape) {
        brushDrawing_ = false; emit brushStrokeCancelRequested(); update(); event->accept(); return;
    }
    if (creationDragging_ && effectiveKey == Qt::Key_Escape) {
        creationDragging_ = false;
        if (tool_ == Tool::Gradient) emit gradientCancelRequested();
        update(); event->accept(); return;
    }
    if (persistentTransform_ && effectiveKey == Qt::Key_Escape) {
        emit transformCancelRequested(); event->accept(); return;
    }
    if (persistentTransform_ && (effectiveKey == Qt::Key_Return || effectiveKey == Qt::Key_Enter)) {
        emit transformApplyRequested(); event->accept(); return;
    }
    if (effectiveKey == Qt::Key_Left || effectiveKey == Qt::Key_Right
        || effectiveKey == Qt::Key_Up || effectiveKey == Qt::Key_Down) {
        const int step = effectiveModifiers.testFlag(Qt::ShiftModifier) ? 10 : 1;
        const QPointF delta(effectiveKey == Qt::Key_Left ? -step : effectiveKey == Qt::Key_Right ? step : 0,
                            effectiveKey == Qt::Key_Up ? -step : effectiveKey == Qt::Key_Down ? step : 0);
        if (document_ && document_->selection && effectiveModifiers.testFlag(Qt::ControlModifier)
            && !effectiveModifiers.testFlag(Qt::AltModifier)) {
            emit selectedPixelsNudgeRequested(delta.toPoint()); event->accept(); return;
        }
        if (tool_ == Tool::Move && document_ && document_->activeLayerId) {
            Layer *active = nullptr;
            for (Layer &layer : document_->layers) if (layer.id == *document_->activeLayerId) { active = &layer; break; }
            const QSet<QUuid> groupTargets = transformsAsGroup() ? transformLayerIds() : QSet<QUuid>();
            if (!groupTargets.isEmpty() && !transformMask_) {
                emit layerTransformStarted(false);
                for (Layer &layer : document_->layers) if (groupTargets.contains(layer.id)) {
                    layer.transform.origin += delta;
                    if (layer.maskLinked && layer.maskPlacement) layer.maskPlacement->origin += delta;
                }
                renderDirty_ = true; update(); emit layerTransformChanged(); emit layerTransformFinished(); event->accept(); return;
            }
            if (active && !active->group && (transformMask_ ? !active->mask.isNull() : !active->image.isNull())) {
                emit layerTransformStarted(false);
                if (transformMask_) {
                    if (!active->maskPlacement) active->maskPlacement = active->transform;
                    active->maskPlacement->origin += delta;
                } else {
                    const QSet<QUuid> targets = selectedLayerIds_.isEmpty() ? QSet<QUuid>{active->id} : selectedLayerIds_;
                    for (Layer &layer : document_->layers) if (!layer.group && targets.contains(layer.id)) {
                        layer.transform.origin += delta;
                        if (layer.maskLinked && layer.maskPlacement) layer.maskPlacement->origin += delta;
                    }
                }
                renderDirty_ = true; update(); emit layerTransformChanged(); emit layerTransformFinished(); event->accept(); return;
            }
        }
        if (document_ && document_->selection && selectionMode_ == 0
            && !(effectiveModifiers & (Qt::ControlModifier | Qt::AltModifier))
            && (tool_ == Tool::Marquee || tool_ == Tool::Lasso || tool_ == Tool::Wand)) {
            emit selectionMoveRequested(delta.toPoint()); event->accept(); return;
        }
    }
    if (tool_ == Tool::Crop && cropRect_ && (effectiveKey == Qt::Key_Return || effectiveKey == Qt::Key_Enter)) {
        resolvePendingCrop(true); event->accept(); return;
    }
    if (tool_ == Tool::Crop && effectiveKey == Qt::Key_Escape) {
        resolvePendingCrop(false); event->accept(); return;
    }
    if (tool_ == Tool::Crop && cropRect_ && (effectiveKey == Qt::Key_Left || effectiveKey == Qt::Key_Right || effectiveKey == Qt::Key_Up || effectiveKey == Qt::Key_Down)) {
        const int step = effectiveModifiers.testFlag(Qt::ShiftModifier) ? 10 : 1;
        cropRect_->translate(effectiveKey == Qt::Key_Left ? -step : effectiveKey == Qt::Key_Right ? step : 0,
                             effectiveKey == Qt::Key_Up ? -step : effectiveKey == Qt::Key_Down ? step : 0);
        update(); event->accept(); return;
    }
    if (pendingDistortion_ && (effectiveKey == Qt::Key_Return || effectiveKey == Qt::Key_Enter)) {
        resolvePendingDistortion(true); event->accept(); return;
    }
    if (pendingDistortion_ && effectiveKey == Qt::Key_Escape) {
        resolvePendingDistortion(false); event->accept(); return;
    }
    if (pendingGradient_ && (effectiveKey == Qt::Key_Return || effectiveKey == Qt::Key_Enter)) {
        resolvePendingGradient(true); event->accept(); return;
    }
    if (pendingGradient_ && effectiveKey == Qt::Key_Escape) {
        resolvePendingGradient(false); event->accept(); return;
    }
    if (floatingTransform_ && (effectiveKey == Qt::Key_Return || effectiveKey == Qt::Key_Enter)) {
        emit floatingTransformCommitRequested(); event->accept(); return;
    }
    if (floatingTransform_ && effectiveKey == Qt::Key_Escape) {
        emit floatingTransformCancelRequested(); event->accept(); return;
    }
    if (tool_ == Tool::Lasso && polygonalLasso_ && !lassoPoints_.isEmpty()) {
        if ((effectiveKey == Qt::Key_Return || effectiveKey == Qt::Key_Enter) && lassoPoints_.size() >= 3) {
            const QPolygonF completed = lassoPoints_; lassoPoints_.clear(); emit polygonSelectionRequested(completed, lassoMode_, selectionAntialiased_); update(); event->accept(); return;
        }
        if (effectiveKey == Qt::Key_Backspace || effectiveKey == Qt::Key_Delete) {
            lassoPoints_.removeLast(); update(); event->accept(); return;
        }
        if (effectiveKey == Qt::Key_Escape) { lassoPoints_.clear(); update(); event->accept(); return; }
    }
    if (effectiveKey == Qt::Key_Tab) {
        if (tool_ == Tool::Wand) {
            toggleWandMode();
            event->accept();
            return;
        }
        if (tool_ == Tool::Blur) {
            emit cycleSmearModeRequested();
            event->accept();
            return;
        }
        emit cycleToolModeRequested();
        event->accept();
        return;
    }
    if (ShortcutManager::instance().dispatchKeyEvent(event)) {
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace compositor
