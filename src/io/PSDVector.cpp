#include "io/PSDVector.h"

#include <QPainter>
#include <QPen>

#include <cmath>
#include <cstring>

namespace compositor {
namespace {

constexpr double kMaxSide = 30000.0;

int indexOfKey(const QByteArray &data, const char *key, int from = 0)
{
    if (from < 0 || from >= data.size()) return -1;
    return data.indexOf(QByteArray(key), from);
}

quint32 be32(const QByteArray &d, int at)
{
    return (quint32(quint8(d[at])) << 24) | (quint32(quint8(d[at + 1])) << 16) | (quint32(quint8(d[at + 2])) << 8) | quint32(quint8(d[at + 3]));
}
qint32 i32(const QByteArray &d, int at) { return qint32(be32(d, at)); }
qint16 i16(const QByteArray &d, int at) { return qint16((quint16(quint8(d[at])) << 8) | quint16(quint8(d[at + 1]))); }

std::optional<double> doubleAt(const QByteArray &d, int at)
{
    if (at < 0 || at + 8 > d.size()) return std::nullopt;
    quint64 bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | quint8(d[at + i]);
    double out; std::memcpy(&out, &bits, sizeof(out));
    return out;
}

bool hasTag(const QByteArray &d, int at, const char *tag) { return at >= 0 && at + 4 <= d.size() && d.mid(at, 4) == QByteArray(tag); }

std::optional<double> keyedDouble(const QByteArray &data, const char *key)
{
    const int start = indexOfKey(data, key);
    if (start < 0) return std::nullopt;
    const int type = start + int(std::strlen(key));
    if (type + 12 > data.size() || !hasTag(data, type, "doub")) return std::nullopt;
    return doubleAt(data, type + 4);
}

std::optional<bool> keyedBool(const QByteArray &data, const char *key)
{
    const int start = indexOfKey(data, key);
    if (start < 0) return std::nullopt;
    const int type = start + int(std::strlen(key));
    if (type + 5 > data.size() || !hasTag(data, type, "bool")) return std::nullopt;
    return data[type + 4] != 0;
}

std::optional<double> keyedUnit(const QByteArray &data, const char *key, int from = 0)
{
    const int keyAt = indexOfKey(data, key, from);
    if (keyAt < 0) return std::nullopt;
    const int unit = indexOfKey(data, "UntF", keyAt);
    if (unit < 0) return std::nullopt;
    return doubleAt(data, unit + 8);
}

std::optional<qint32> keyedLong(const QByteArray &data, const char *key)
{
    const int start = indexOfKey(data, key);
    if (start < 0) return std::nullopt;
    const int type = start + int(std::strlen(key));
    if (type + 8 > data.size() || !hasTag(data, type, "long")) return std::nullopt;
    return i32(data, type + 4);
}

struct Rgb { double r, g, b; };
std::optional<Rgb> rgbOf(const QByteArray &data)
{
    const auto r = keyedDouble(data, "Rd  "), g = keyedDouble(data, "Grn "), b = keyedDouble(data, "Bl  ");
    if (!r || !g || !b) return std::nullopt;
    const auto channel = [](double v) { return v > 1 ? std::clamp(v, 0.0, 255.0) / 255.0 : std::clamp(v, 0.0, 1.0); };
    return Rgb{channel(*r), channel(*g), channel(*b)};
}

QPointF pathPoint(const QByteArray &body, int at, const QSizeF &canvas)
{
    const double y = double(i32(body, at)) / 0x1000000;
    const double x = double(i32(body, at + 4)) / 0x1000000;
    return QPointF(x * canvas.width(), y * canvas.height());
}

// CGRect.integral: the smallest integer rectangle containing it.
QRectF integral(const QRectF &r)
{
    const double left = std::floor(r.left()), top = std::floor(r.top());
    return QRectF(left, top, std::ceil(r.right()) - left, std::ceil(r.bottom()) - top);
}

// Rejects sizes beyond 30,000 px or the remaining-pixel budget. Nothing when the size is not finite.
std::optional<QSize> pixelSize(const QSizeF &size, qint64 remainingPixels, bool *tooLarge)
{
    if (!std::isfinite(size.width()) || !std::isfinite(size.height())) return std::nullopt;
    if (std::abs(size.width()) > kMaxSide || std::abs(size.height()) > kMaxSide) { if (tooLarge) *tooLarge = true; return std::nullopt; }
    const double budget = double(std::min<qint64>(100000000LL, std::max<qint64>(0, remainingPixels)));
    if (size.width() * size.height() > budget) { if (tooLarge) *tooLarge = true; return std::nullopt; }
    const int width = std::max(1, int(size.width())), height = std::max(1, int(size.height()));
    if (double(width) * height > budget) { if (tooLarge) *tooLarge = true; return std::nullopt; }
    return QSize(width, height);
}

struct Origination {
    ShapeKind kind = ShapeKind::Rectangle;
    QRectF bounds;
    double cornerRadius = 0;
};

// Photoshop vogk origination: 1/2 = rectangle (2 is rounded), 5 = ellipse.
std::optional<Origination> origination(const QByteArray &data)
{
    if (data.isEmpty()) return std::nullopt;
    const auto type = keyedLong(data, "keyOriginType");
    if (!type) return std::nullopt;
    ShapeKind kind;
    switch (*type) {
    case 1: case 2: kind = ShapeKind::Rectangle; break;
    case 5: kind = ShapeKind::Ellipse; break;
    default: return std::nullopt;
    }
    const int bboxAt = indexOfKey(data, "keyOriginShapeBBox");
    const int from = bboxAt < 0 ? 0 : bboxAt;
    const auto left = keyedUnit(data, "Left", from), top = keyedUnit(data, "Top ", from),
               right = keyedUnit(data, "Rght", from), bottom = keyedUnit(data, "Btom", from);
    if (!left || !top || !right || !bottom) return std::nullopt;
    const QRectF bounds(*left, *top, *right - *left, *bottom - *top);
    if (!(bounds.width() >= 1) || !(bounds.height() >= 1) || !std::isfinite(bounds.x()) || !std::isfinite(bounds.y())
        || !std::isfinite(bounds.width()) || !std::isfinite(bounds.height())) return std::nullopt;
    Origination out; out.kind = kind; out.bounds = bounds;
    const int radiiAt = indexOfKey(data, "keyOriginRRectRadii");
    if (kind == ShapeKind::Rectangle && radiiAt >= 0) {
        QVector<double> radii;
        for (const char *key : {"topLeft", "topRight", "bottomRight", "bottomLeft"}) if (const auto v = keyedUnit(data, key, radiiAt)) radii.append(*v);
        if (radii.size() == 4) {
            const double lo = *std::min_element(radii.cbegin(), radii.cend()), hi = *std::max_element(radii.cbegin(), radii.cend());
            if (hi - lo > 0.5) return std::nullopt;
            out.cornerRadius = hi;
        }
    }
    return out;
}

std::optional<Origination> sharpRect(const QByteArray &data, const QSizeF &canvas)
{
    if (data.isEmpty()) return std::nullopt;
    const auto outline = PSDVector::path(data, canvas);
    if (!outline) return std::nullopt;
    int offset = 8, remaining = 0;
    QVector<QPointF> anchors;
    bool sharp = true;
    while (offset + 26 <= data.size()) {
        const int type = i16(data, offset);
        const QByteArray body = data.mid(offset + 2, 24);
        offset += 26;
        switch (type) {
        case 0: case 3:
            if (!anchors.isEmpty()) return std::nullopt;
            remaining = i16(body, 0);
            break;
        case 1: case 2: case 4: case 5: {
            if (remaining <= 0 || body.size() < 24) break;
            --remaining;
            const QPointF incoming = pathPoint(body, 0, canvas), anchor = pathPoint(body, 8, canvas), outgoing = pathPoint(body, 16, canvas);
            if (std::hypot(incoming.x() - anchor.x(), incoming.y() - anchor.y()) > 0.5
                || std::hypot(outgoing.x() - anchor.x(), outgoing.y() - anchor.y()) > 0.5) sharp = false;
            anchors.append(anchor);
            break;
        }
        default: break;
        }
    }
    if (!sharp || anchors.size() != 4) return std::nullopt;
    const QRectF box = outline->boundingRect();
    if (box.width() < 1 || box.height() < 1) return std::nullopt;
    Origination out; out.kind = ShapeKind::Rectangle; out.bounds = box;
    return out;
}

} // namespace

namespace PSDVector {

QImage shapeImage(ShapeKind kind, const QSize &size, const QColor &color, double cornerRadius)
{
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    const QRectF bounds(QPointF(0, 0), QSizeF(size));
    if (kind == ShapeKind::Ellipse) path.addEllipse(bounds);
    else if (cornerRadius > 0) {
        const double radius = std::min({cornerRadius, bounds.width() / 2.0, bounds.height() / 2.0});
        path.addRoundedRect(bounds, radius, radius);
    } else path.addRect(bounds);
    painter.fillPath(path, color);
    return image;
}

std::optional<QPainterPath> path(const QByteArray &data, const QSizeF &canvas)
{
    if (data.size() < 8 || canvas.width() <= 0 || canvas.height() <= 0) return std::nullopt;
    QPainterPath out;
    int offset = 8, remaining = 0;
    bool closed = true, first = true;
    QPointF previousOut;
    while (offset + 26 <= data.size()) {
        const int type = i16(data, offset);
        const QByteArray body = data.mid(offset + 2, 24);
        offset += 26;
        switch (type) {
        case 0: case 3:
            if (!first && closed) out.closeSubpath();
            remaining = i16(body, 0);
            closed = type == 0;
            first = true;
            break;
        case 1: case 2: case 4: case 5: {
            if (remaining <= 0 || body.size() < 24) break;
            --remaining;
            const QPointF incoming = pathPoint(body, 0, canvas), anchor = pathPoint(body, 8, canvas), outgoing = pathPoint(body, 16, canvas);
            if (first) { out.moveTo(anchor); first = false; }
            else out.cubicTo(previousOut, incoming, anchor);
            previousOut = outgoing;
            break;
        }
        default: break;
        }
    }
    if (!first && closed) out.closeSubpath();
    if (out.isEmpty()) return std::nullopt;
    return out;
}

std::optional<PSDVectorLive> live(const QMap<QString, QByteArray> &extra, const QSizeF &canvas, qint64 remainingPixels, bool *tooLarge)
{
    const QByteArray stroke = extra.value(QStringLiteral("vstk"));
    const QByteArray solid = extra.value(QStringLiteral("SoCo"));
    const bool hasSolid = extra.contains(QStringLiteral("SoCo"));
    const std::optional<bool> fillFlag = stroke.isEmpty() ? std::nullopt : keyedBool(stroke, "fillEnabled");
    const std::optional<bool> strokeFlag = stroke.isEmpty() ? std::nullopt : keyedBool(stroke, "strokeEnabled");
    const bool fillEnabled = fillFlag.value_or(hasSolid);
    const bool strokeEnabled = strokeFlag.value_or(false);
    if (!fillEnabled) return std::nullopt;
    const auto fill = rgbOf(solid);
    if (!hasSolid || !fill) return std::nullopt;
    std::optional<Origination> origin = origination(extra.value(QStringLiteral("vogk")));
    if (!origin) {
        QByteArray mask = extra.value(QStringLiteral("vmsk"));
        if (mask.isEmpty()) mask = extra.value(QStringLiteral("vsms"));
        origin = sharpRect(mask, canvas);
    }
    if (!origin) return std::nullopt;
    const QRectF box = integral(origin->bounds);
    if (!std::isfinite(box.x()) || !std::isfinite(box.y())) return std::nullopt;
    const auto size = pixelSize(box.size(), remainingPixels, tooLarge);
    if (!size) return std::nullopt;
    PSDVectorLive out;
    out.style.kind = origin->kind;
    out.style.red = fill->r; out.style.green = fill->g; out.style.blue = fill->b;
    out.style.cornerRadius = origin->cornerRadius;
    out.bounds = QRectF(box.topLeft(), QSizeF(*size));
    out.image = shapeImage(out.style.kind, *size, QColor::fromRgbF(fill->r, fill->g, fill->b), out.style.cornerRadius);
    if (strokeEnabled) out.notes.append(QStringLiteral("The Photoshop stroke isn’t supported on shape layers and was omitted."));
    return out;
}

std::optional<PSDVectorRaster> raster(const QMap<QString, QByteArray> &extra, const QSizeF &canvas, qint64 remainingPixels, bool *tooLarge)
{
    QByteArray mask = extra.value(QStringLiteral("vmsk"));
    if (mask.isEmpty()) mask = extra.value(QStringLiteral("vsms"));
    if (mask.isEmpty()) return std::nullopt;
    const auto outline = path(mask, canvas);
    if (!outline) return std::nullopt;
    const QByteArray solid = extra.value(QStringLiteral("SoCo"));
    const QByteArray stroke = extra.value(QStringLiteral("vstk"));
    const auto fill = solid.isEmpty() ? std::nullopt : rgbOf(solid);
    const std::optional<bool> fillFlag = stroke.isEmpty() ? std::nullopt : keyedBool(stroke, "fillEnabled");
    const std::optional<bool> strokeFlag = stroke.isEmpty() ? std::nullopt : keyedBool(stroke, "strokeEnabled");
    const bool fillEnabled = fillFlag.value_or(fill.has_value());
    const bool strokeEnabled = strokeFlag.value_or(false);
    const auto strokeColor = stroke.isEmpty() ? std::nullopt : rgbOf(stroke);
    double strokeWidth = 1;
    if (!stroke.isEmpty()) { if (const auto w = keyedUnit(stroke, "strokeStyleLineWidth")) strokeWidth = *w; }
    if (!((fillEnabled && fill) || (strokeEnabled && strokeColor))) return std::nullopt;
    if (!std::isfinite(strokeWidth)) return std::nullopt;
    if (strokeEnabled && !(strokeWidth >= 0 && strokeWidth <= kMaxSide)) { if (tooLarge) *tooLarge = true; return std::nullopt; }
    QRectF box = outline->boundingRect();
    if (strokeEnabled) { const double inset = std::ceil(strokeWidth / 2 + 1); box.adjust(-inset, -inset, inset, inset); }
    box = integral(box);
    if (!std::isfinite(box.x()) || !std::isfinite(box.y())) return std::nullopt;
    const auto size = pixelSize(box.size(), remainingPixels, tooLarge);
    if (!size) return std::nullopt;
    QImage image(*size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(-box.left(), -box.top());
    QPainterPath shape = *outline;
    shape.setFillRule(Qt::WindingFill);
    if (fillEnabled && fill) painter.fillPath(shape, QColor::fromRgbF(fill->r, fill->g, fill->b));
    if (strokeEnabled && strokeColor) {
        QPen pen(QColor::fromRgbF(strokeColor->r, strokeColor->g, strokeColor->b), strokeWidth, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
        pen.setMiterLimit(10);
        painter.strokePath(shape, pen);
    }
    painter.end();
    return PSDVectorRaster{image, QRectF(box.left(), box.top(), size->width(), size->height())};
}

} // namespace PSDVector
} // namespace compositor
