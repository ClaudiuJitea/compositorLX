// Shape editing (macOS Compositor 1.7, ShapeTool.swift: 3adce92, 82a9aeb, 8f8c721, 6dea5ac, 8997bfe, e63b825, a38cb04).
#include "core/DocumentLimits.h"
#include "core/EditorSession.h"
#include "rendering/ShapeLine.h"
#include "rendering/ShapePath.h"
#include <QJsonArray>
#include <QPainter>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace compositor {
namespace {
bool isLiveShape(const Layer &layer)
{
    return !layer.group && layer.shapeStyle && !layer.image.isNull() && !layer.text;
}

// The layer's shape record, kept in step with its style (LX keeps both).
void mirrorShapeJson(Layer &layer)
{
    if (!layer.shapeStyle) return;
    const LayerShapeStyle &style = *layer.shapeStyle;
    const QColor fill = style.color();
    layer.shape.insert(QStringLiteral("fill"), fill.name(QColor::HexArgb));
    layer.shape.insert(QStringLiteral("red"), style.red);
    layer.shape.insert(QStringLiteral("green"), style.green);
    layer.shape.insert(QStringLiteral("blue"), style.blue);
    layer.shape.insert(QStringLiteral("cornerRadius"), style.cornerRadius);
    if (style.kind == ShapeKind::Line) {
        layer.shape.insert(QStringLiteral("stroke"), fill.name(QColor::HexArgb));
        if (style.lineWidth) layer.shape.insert(QStringLiteral("lineWidth"), *style.lineWidth);
        if (style.start) layer.shape.insert(QStringLiteral("start"), QJsonArray{style.start->x(), style.start->y()});
        if (style.end) layer.shape.insert(QStringLiteral("end"), QJsonArray{style.end->x(), style.end->y()});
    } else {
        // The legacy centered-stroke keys stay empty: the inside stroke lives in the style.
        layer.shape.insert(QStringLiteral("stroke"), QColor(Qt::transparent).name(QColor::HexArgb));
        layer.shape.insert(QStringLiteral("strokeWidth"), 0.0);
    }
}

// A line's box around its two ends, as addShape lays one out: half the thickness each way, a square cap's corners too.
QRectF lineBox(QPointF from, QPointF to, double thickness, const QString &cap)
{
    const double length = QLineF(from, to).length();
    const double corner = length > 0 ? thickness / 2 * (std::abs(to.x() - from.x()) + std::abs(to.y() - from.y())) / length : thickness / 2;
    const double pad = thickness / 2 + (cap == QStringLiteral("Square") ? std::ceil(corner - thickness / 2 - .0001) : 0);
    return QRectF(std::min(from.x(), to.x()) - pad, std::min(from.y(), to.y()) - pad,
                  std::max(1.0, std::abs(to.x() - from.x()) + pad * 2), std::max(1.0, std::abs(to.y() - from.y()) + pad * 2));
}

// Puts `width` (and the style's cap) on a line and rebuilds its box around the same two points (mac relayoutLine).
void relayoutLine(LayerShapeStyle &style, LayerTransform &transform, double width)
{
    const auto placed = [&](QPointF unit) {
        return QPointF(transform.origin.x() + unit.x() * transform.size.width(), transform.origin.y() + unit.y() * transform.size.height());
    };
    const QPointF from = placed(style.start.value_or(QPointF(0, 0))), to = placed(style.end.value_or(QPointF(1, 1)));
    const double thickness = std::max(1.0, width);
    const QRectF rect = lineBox(from, to, thickness, style.lineCap.value_or(QStringLiteral("Round")));
    const auto unit = [&](QPointF point) {
        return QPointF(rect.width() > 0 ? (point.x() - rect.left()) / rect.width() : .5, rect.height() > 0 ? (point.y() - rect.top()) / rect.height() : .5);
    };
    style.lineWidth = width;
    style.start = unit(from);
    style.end = unit(to);
    transform.origin = rect.topLeft();
    transform.size = QSizeF(std::ceil(rect.width()), std::ceil(rect.height()));
}
} // namespace

QImage EditorSession::shapeImage(const LayerShapeStyle &style, QSize size)
{
    if (size.isEmpty() || qint64(size.width()) * size.height() > DocumentLimits::maxSurfacePixels) return {};
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    if (style.kind == ShapeKind::Line) {
        const QPointF start = style.start.value_or(QPointF(0, 0)), end = style.end.value_or(QPointF(1, 1));
        drawShapeLine(painter, QPointF(start.x() * size.width(), start.y() * size.height()),
                      QPointF(end.x() * size.width(), end.y() * size.height()), std::max(1.0, style.lineWidth.value_or(2)),
                      style.color(), style.lineStyle.value_or(QStringLiteral("Solid")), style.lineCap.value_or(QStringLiteral("Round")));
        return image;
    }
    const QRectF bounds(0, 0, size.width(), size.height());
    QPainterPath path;
    if (style.kind == ShapeKind::Ellipse) path.addEllipse(bounds);
    else path = rectanglePath(bounds, style.cornerRadius, style.smoothing.value_or(false));
    // The stroke sits inside the box, at most half the shorter side, so it meets in the middle and no further.
    fillShape(painter, path, style.color(), style.strokeColor(),
              std::min(style.strokeWidth.value_or(0), std::min(size.width(), size.height()) / 2.0));
    return image;
}

const Layer *EditorSession::editingShapeLayer() const
{
    if (!shapeToolActive || maskSelected_ || selectedLayerIds_.size() > 1) return nullptr;
    const Layer *layer = activeLayer();
    return layer && isLiveShape(*layer) && layer->shapeStyle->kind != ShapeKind::Line ? layer : nullptr;
}

const Layer *EditorSession::editingLineLayer() const
{
    if (!shapeToolActive || maskSelected_ || selectedLayerIds_.size() > 1) return nullptr;
    const Layer *layer = activeLayer();
    return layer && isLiveShape(*layer) && layer->shapeStyle->kind == ShapeKind::Line ? layer : nullptr;
}

std::optional<ShapeKind> EditorSession::selectedShapeKind() const
{
    if (selectedLayerIds_.size() > 1) return std::nullopt;
    const Layer *layer = activeLayer();
    if (!layer || !isLiveShape(*layer)) return std::nullopt;
    return layer->shapeStyle->kind;
}

QColor EditorSession::nextShapeFill() const
{
    if (shapeFillChoice && shapeFillChoice->second == foreground_) return shapeFillChoice->first;
    return foreground_;
}

QColor EditorSession::displayedShapeStroke() const
{
    if (const Layer *layer = editingShapeLayer(); layer && layer->shapeStyle->strokeColor()) return *layer->shapeStyle->strokeColor();
    return shapeStrokeColor;
}

double EditorSession::displayedStrokeWidth() const
{
    if (const Layer *layer = editingShapeLayer()) return layer->shapeStyle->strokeWidth.value_or(0);
    return shapeStrokeWidth;
}

double EditorSession::displayedCornerRadius() const
{
    if (const Layer *layer = editingShapeLayer(); layer && layer->shapeStyle->kind == ShapeKind::Rectangle) return layer->shapeStyle->cornerRadius;
    return shapeCornerRadius;
}

bool EditorSession::displayedShapeSmoothing() const
{
    if (const Layer *layer = editingShapeLayer(); layer && layer->shapeStyle->kind == ShapeKind::Rectangle) return layer->shapeStyle->smoothing.value_or(false);
    return shapeSmoothing;
}

double EditorSession::displayedLineWidth() const
{
    if (const Layer *layer = editingLineLayer(); layer && layer->shapeStyle->lineWidth) return *layer->shapeStyle->lineWidth;
    return shapeLineWidth;
}

QString EditorSession::displayedLineStyle() const
{
    if (const Layer *layer = editingLineLayer()) return layer->shapeStyle->lineStyle.value_or(QStringLiteral("Solid"));
    return shapeLineStyle;
}

QString EditorSession::displayedLineCap() const
{
    if (const Layer *layer = editingLineLayer()) return layer->shapeStyle->lineCap.value_or(QStringLiteral("Round"));
    return shapeLineCap;
}

void EditorSession::beginShapeStyleEdit()
{
    const Layer *layer = editingShapeLayer() ? editingShapeLayer() : editingLineLayer();
    if (shapeStyleEditLayerId_ || !layer) return;
    cancelShapeColorPreview();
    beginEdit(QStringLiteral("Shape Style"));
    shapeStyleEditLayerId_ = layer->id;
}

void EditorSession::finishShapeStyleEdit()
{
    if (!shapeStyleEditLayerId_) return;
    shapeStyleEditLayerId_.reset();
    endEdit();
}

bool EditorSession::paintShape(int index, const LayerShapeStyle &style, std::optional<LayerTransform> placement)
{
    if (!document_ || index < 0 || index >= document_->layers.size()) return false;
    Layer &layer = document_->layers[index];
    const QSize size = placement ? QSize(std::max(1, qCeil(placement->size.width())), std::max(1, qCeil(placement->size.height()))) : layer.image.size();
    const QImage image = shapeImage(style, size);
    if (image.isNull()) return false;
    if (placement) {
        if (!layer.mask.isNull() && !layer.maskPlacement) layer.maskPlacement = layer.transform;
        layer.transform = *placement;
    }
    layer.image = image;
    layer.shapeStyle = style;
    mirrorShapeJson(layer);
    return true;
}

bool EditorSession::changeLiveShape(const std::function<void(LayerShapeStyle &, LayerTransform &)> &change, bool line)
{
    if (!shapeStyleEditLayerId_) cancelShapeColorPreview();
    const Layer *layer = line ? editingLineLayer() : editingShapeLayer();
    if (!layer) return false;
    LayerShapeStyle style = *layer->shapeStyle;
    LayerTransform transform = layer->transform;
    const LayerShapeStyle beforeStyle = style;
    const LayerTransform beforeTransform = transform;
    change(style, transform);
    if (style == beforeStyle && transform == beforeTransform) return false;
    if (!style.isValid() || qint64(qCeil(transform.size.width())) * qCeil(transform.size.height()) > DocumentLimits::maxSurfacePixels) return false;
    const bool open = shapeStyleEditLayerId_.has_value();
    if (!open) beginEdit(QStringLiteral("Shape Style"));
    const bool painted = paintShape(indexOf(layer->id), style, transform == beforeTransform ? std::nullopt : std::optional<LayerTransform>(transform));
    if (!open) endEdit();
    return painted;
}

bool EditorSession::setDisplayedStrokeWidth(double width)
{
    width = std::isfinite(width) ? std::clamp(width, 0.0, 5000.0) : 0;
    if (!editingShapeLayer()) { shapeStrokeWidth = width; return true; }
    return changeLiveShape([&](LayerShapeStyle &style, LayerTransform &) {
        style.setStroke(width, style.strokeColor().value_or(shapeStrokeColor));
    }, false);
}

bool EditorSession::setDisplayedCornerRadius(double radius)
{
    radius = std::isfinite(radius) ? std::clamp(radius, 0.0, 5000.0) : 0;
    const Layer *layer = editingShapeLayer();
    if (!layer || layer->shapeStyle->kind != ShapeKind::Rectangle) { shapeCornerRadius = radius; return true; }
    return changeLiveShape([&](LayerShapeStyle &style, LayerTransform &) { style.cornerRadius = radius; }, false);
}

bool EditorSession::setDisplayedShapeSmoothing(bool on)
{
    const Layer *layer = editingShapeLayer();
    if (!layer || layer->shapeStyle->kind != ShapeKind::Rectangle) { shapeSmoothing = on; return true; }
    return changeLiveShape([&](LayerShapeStyle &style, LayerTransform &) { style.smoothing = on ? std::optional<bool>(true) : std::nullopt; }, false);
}

bool EditorSession::setDisplayedLineWidth(double width)
{
    width = std::isfinite(width) ? std::clamp(width, 1.0, 5000.0) : 4;
    if (!editingLineLayer()) { shapeLineWidth = width; return true; }
    return changeLiveShape([&](LayerShapeStyle &style, LayerTransform &transform) { relayoutLine(style, transform, width); }, true);
}

bool EditorSession::setDisplayedLineStyle(const QString &value)
{
    if (!editingLineLayer()) { shapeLineStyle = value; return true; }
    return changeLiveShape([&](LayerShapeStyle &style, LayerTransform &) {
        style.lineStyle = value == QStringLiteral("Solid") ? std::nullopt : std::optional<QString>(value);
    }, true);
}

bool EditorSession::setDisplayedLineCap(const QString &cap)
{
    if (!editingLineLayer()) { shapeLineCap = cap; return true; }
    return changeLiveShape([&](LayerShapeStyle &style, LayerTransform &transform) {
        style.lineCap = cap == QStringLiteral("Round") ? std::nullopt : std::optional<QString>(cap);
        relayoutLine(style, transform, style.lineWidth.value_or(shapeLineWidth));
    }, true);
}

bool EditorSession::pickShapeFill(const QColor &color)
{
    if (!color.isValid()) return false;
    cancelShapeColorPreview();
    const Layer *layer = editingShapeLayer() ? editingShapeLayer() : editingLineLayer();
    if (!layer) {
        // Nothing to recolor: it sets the foreground, which the next shape takes.
        setForegroundColor(color);
        return true;
    }
    shapeFillChoice = std::make_pair(color.toRgb(), foreground_);
    LayerShapeStyle style = *layer->shapeStyle;
    const QColor rgb = color.toRgb();
    if (style.red == rgb.redF() && style.green == rgb.greenF() && style.blue == rgb.blueF()) return true;
    style.red = rgb.redF(); style.green = rgb.greenF(); style.blue = rgb.blueF();
    finishShapeStyleEdit();
    beginEdit(QStringLiteral("Shape Style"));
    const bool painted = paintShape(indexOf(layer->id), style);
    endEdit();
    return painted;
}

bool EditorSession::pickShapeStroke(const QColor &color)
{
    if (!color.isValid()) return false;
    cancelShapeColorPreview();
    const Layer *layer = editingShapeLayer();
    if (!layer || !layer->shapeStyle->strokeWidth) { shapeStrokeColor = color.toRgb(); return true; }
    LayerShapeStyle style = *layer->shapeStyle;
    const auto before = style.strokeColor();
    style.setStroke(*style.strokeWidth, color);
    if (style.strokeColor() == before) return true;
    finishShapeStyleEdit();
    beginEdit(QStringLiteral("Shape Style"));
    const bool painted = paintShape(indexOf(layer->id), style);
    endEdit();
    return painted;
}

void EditorSession::previewShapeColor(const QColor &color, bool fill)
{
    const Layer *layer = fill ? (editingShapeLayer() ? editingShapeLayer() : editingLineLayer()) : editingShapeLayer();
    if (!layer || !color.isValid()) return;
    if (!shapeColorPreview_ || shapeColorPreview_->layerId != layer->id)
        shapeColorPreview_ = ShapeColorPreview{layer->id, layer->image, layer->shapeStyle, layer->shape};
    LayerShapeStyle style = *shapeColorPreview_->style;
    if (fill) {
        const QColor rgb = color.toRgb();
        style.red = rgb.redF(); style.green = rgb.greenF(); style.blue = rgb.blueF();
    } else if (style.strokeWidth) {
        style.setStroke(*style.strokeWidth, color);
    } else return;
    paintShape(indexOf(layer->id), style);
}

void EditorSession::cancelShapeColorPreview()
{
    if (!shapeColorPreview_) return;
    // Put back as it was: a color only being previewed is not an edit, and is kept out of a save (mac 6dea5ac).
    const int index = indexOf(shapeColorPreview_->layerId);
    if (document_ && index >= 0) {
        Layer &layer = document_->layers[index];
        layer.image = shapeColorPreview_->image;
        layer.shapeStyle = shapeColorPreview_->style;
        layer.shape = shapeColorPreview_->shape;
    }
    shapeColorPreview_.reset();
}

bool EditorSession::recolorShape(const QUuid &id, const QColor &color)
{
    cancelShapeColorPreview();
    const int index = indexOf(id);
    if (!document_ || index < 0 || !isLiveShape(document_->layers.at(index)) || !color.isValid()) return false;
    LayerShapeStyle style = *document_->layers.at(index).shapeStyle;
    const QColor rgb = color.toRgb();
    if (style.red == rgb.redF() && style.green == rgb.greenF() && style.blue == rgb.blueF()) return true;
    style.red = rgb.redF(); style.green = rgb.greenF(); style.blue = rgb.blueF();
    if (shapeImage(style, document_->layers.at(index).image.size()).isNull()) return false;
    finishShapeStyleEdit();
    beginEdit(QStringLiteral("Fill"));
    paintShape(index, style);
    endEdit();
    return true;
}
bool EditorSession::drawShape(ShapeKind kind, const QRectF &rect, const std::optional<QPointF> &start, const std::optional<QPointF> &end)
{
    const QColor fill = nextShapeFill();
    if (kind == ShapeKind::Line) {
        const QRectF box = start && end ? lineBox(*start, *end, std::max(1.0, displayedLineWidth()), displayedLineCap()) : rect;
        return addShape(kind, box, fill, fill, displayedLineWidth(), 0, start, end, false, displayedLineStyle(), displayedLineCap());
    }
    return addShape(kind, rect, fill, displayedShapeStroke(), displayedStrokeWidth(), kind == ShapeKind::Rectangle ? displayedCornerRadius() : 0,
                    std::nullopt, std::nullopt, kind == ShapeKind::Rectangle && displayedShapeSmoothing());
}

} // namespace compositor
