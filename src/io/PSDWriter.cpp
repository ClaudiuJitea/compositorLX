#include "io/PSDWriter.h"
#include "rendering/DeepImage.h"
#include "rendering/LayerRenderer.h"
#include <QColorSpace>
#include <QHash>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace compositor::PSDWriter {
namespace {
// Big-endian bytes, as every number in a Photoshop file is.
struct Bytes {
    QByteArray bytes;
    void u8(int v) { bytes.append(char(v & 0xff)); }
    void u16(int v) { u8(v >> 8); u8(v); }
    void i16(int v) { u16(int(quint16(qint16(v)))); }
    void u32(qint64 v) { u16(int((v >> 16) & 0xffff)); u16(int(v & 0xffff)); }
    void i32(int v) { u32(qint64(quint32(v))); }
    void zeros(int n) { bytes.append(QByteArray(n, '\0')); }
    void ascii(const char *text) { bytes.append(text); }
    void data(const QByteArray &d, bool lengthPrefixed = false) { if (lengthPrefixed) u32(d.size()); bytes.append(d); }
    // An additional layer information block, padded to an even length.
    void block(const char *key, const QByteArray &body) {
        ascii("8BIM"); ascii(key);
        const int padded = body.size() + body.size() % 2;
        u32(padded); bytes.append(body); zeros(padded - body.size());
    }
};

struct Rect { int top = 0, left = 0, bottom = 0, right = 0; int width() const { return right - left; } int height() const { return bottom - top; } };
Rect rectOf(const QRect &r) { return Rect{r.top(), r.left(), r.top() + r.height(), r.left() + r.width()}; }

struct Channel { int id; QByteArray data; };
struct Record {
    QString name;
    Rect rect;
    QVector<Channel> channels;
    QString blend = QStringLiteral("norm");
    double opacity = 1;
    bool clipping = false, hidden = false;
    struct Mask { Rect rect; quint8 defaultValue; bool disabled; bool unlinked; };
    std::optional<Mask> mask;
    std::optional<int> section; // 1 an open group, 3 the divider closing it
};

// A plane's channel data: PackBits at 8 bits, raw at 16 (Photoshop's own 16-bit choice is zip).
QByteArray channel(const QByteArray &plane, int width, int height, bool deep)
{
    Bytes out;
    if (deep || width == 0 || height == 0) { out.u16(0); out.data(plane); return out.bytes; }
    QVector<QByteArray> rows;
    rows.reserve(height);
    for (int y = 0; y < height; ++y) rows.push_back(packBits(reinterpret_cast<const uchar *>(plane.constData()) + qsizetype(y) * width, width));
    out.u16(1);
    for (const QByteArray &row : rows) out.u16(row.size());
    for (const QByteArray &row : rows) out.data(row);
    return out.bytes;
}
QVector<Channel> emptyChannels()
{
    return {{0, QByteArray(2, '\0')}, {1, QByteArray(2, '\0')}, {2, QByteArray(2, '\0')}, {-1, QByteArray(2, '\0')}};
}

// Straight (not premultiplied) red, green, blue and alpha planes of `area` of `image`, big-endian at 16 bits.
QVector<QByteArray> planes(const QImage &image, const QRect &area, bool deep)
{
    const QImage straight = image.convertToFormat(deep ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    const int bytes = deep ? 2 : 1;
    QVector<QByteArray> result(4, QByteArray(qsizetype(area.width()) * area.height() * bytes, '\0'));
    for (int y = 0; y < area.height(); ++y) {
        if (deep) {
            const auto *line = reinterpret_cast<const quint16 *>(straight.constScanLine(area.top() + y)) + area.left() * 4;
            for (int c = 0; c < 4; ++c) {
                auto *out = reinterpret_cast<uchar *>(result[c].data()) + qsizetype(y) * area.width() * 2;
                for (int x = 0; x < area.width(); ++x) { const quint16 v = line[x * 4 + c]; out[x * 2] = uchar(v >> 8); out[x * 2 + 1] = uchar(v); }
            }
        } else {
            const uchar *line = straight.constScanLine(area.top() + y) + area.left() * 4;
            for (int c = 0; c < 4; ++c) {
                auto *out = reinterpret_cast<uchar *>(result[c].data()) + qsizetype(y) * area.width();
                for (int x = 0; x < area.width(); ++x) out[x] = line[x * 4 + c];
            }
        }
    }
    return result;
}
// A grayscale picture's plane over `area`, big-endian at 16 bits.
QByteArray grayPlane(const QImage &gray, const QRect &area, bool deep)
{
    QByteArray result(qsizetype(area.width()) * area.height() * (deep ? 2 : 1), '\0');
    for (int y = 0; y < area.height(); ++y) {
        const uchar *line = gray.constScanLine(area.top() + y) + area.left();
        auto *out = reinterpret_cast<uchar *>(result.data()) + qsizetype(y) * area.width() * (deep ? 2 : 1);
        for (int x = 0; x < area.width(); ++x) {
            if (deep) { out[x * 2] = line[x]; out[x * 2 + 1] = line[x]; } // v·257, big-endian
            else out[x] = line[x];
        }
    }
    return result;
}

// The smallest rectangle holding every pixel with any alpha; empty when there's none.
QRect trimmed(const QImage &image)
{
    const QImage alpha = image.convertToFormat(QImage::Format_RGBA64_Premultiplied);
    int minX = image.width(), minY = image.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < alpha.height(); ++y) {
        const auto *line = reinterpret_cast<const quint16 *>(alpha.constScanLine(y));
        for (int x = 0; x < alpha.width(); ++x) if (line[x * 4 + 3]) { minX = std::min(minX, x); maxX = std::max(maxX, x); minY = std::min(minY, y); maxY = std::max(maxY, y); }
    }
    return maxX < minX ? QRect() : QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

bool drawsEffects(const Layer &layer)
{
    if (!layer.effects) return false;
    const LayerEffects shown = layer.effects->visible();
    return !shown.isEmpty() && shown.isValid();
}

// A layer as it draws on its own, on the document: placed, with its effects, at full opacity and in Normal, trimmed to
// its pixels. Its mask goes alongside, unless effects drew it into the pixels.
Record layerRecord(const Document &document, const Layer &layer, bool deep)
{
    Record record;
    record.name = layer.name;
    record.blend = blendKey(layer.blendMode);
    record.opacity = layer.opacity;
    record.hidden = !layer.visible;
    record.clipping = layer.maskSourceId.has_value();
    if (layer.image.isNull()) { record.channels = emptyChannels(); return record; }
    const bool effects = drawsEffects(layer);
    const bool separateMask = !effects && !layer.mask.isNull();
    Document alone = document;
    Layer own = layer;
    own.parentId.reset(); own.maskSourceId.reset(); own.opacity = 1; own.blendMode = BlendMode::Normal; own.visible = true;
    if (separateMask) { own.mask = QImage(); own.maskPlacement.reset(); }
    alone.layers = {own};
    const QImage drawn = LayerRenderer::flattened(alone);
    const QRect area = drawn.isNull() ? QRect() : trimmed(drawn);
    if (area.isEmpty()) { record.channels = emptyChannels(); return record; }
    record.rect = rectOf(area);
    const QVector<QByteArray> rgba = planes(drawn, area, deep);
    const int ids[] = {0, 1, 2, -1};
    for (int c = 0; c < 4; ++c) record.channels.push_back({ids[c], channel(rgba[c], area.width(), area.height(), deep)});
    if (separateMask) {
        // On the layer's pixels, as the layer draws; everywhere else Photoshop's default, showing.
        QImage gray(document.canvasSize, QImage::Format_Grayscale8);
        gray.fill(255);
        const QImage grid = LayerRenderer::placedMask(layer, layer, layer.image.size());
        if (!grid.isNull()) {
            QPainter painter(&gray);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, layer.transform.sampling != Sampling::Nearest);
            painter.setTransform(LayerRenderer::pixelToDocument(layer.transform, grid.size()));
            painter.drawImage(0, 0, grid);
        }
        record.mask = Record::Mask{record.rect, 255, !layer.maskEnabled, !layer.maskLinked};
        record.channels.push_back({-2, channel(grayPlane(gray, area, deep), area.width(), area.height(), deep)});
    }
    return record;
}

// A folder's mask, over the whole canvas; outside it the folder hides.
void addFolderMask(const Document &document, const Layer &folder, Record &record, bool deep)
{
    if (folder.mask.isNull()) return;
    QImage gray(document.canvasSize, QImage::Format_Grayscale8);
    gray.fill(0);
    {
        const QImage mask = folder.mask.convertToFormat(QImage::Format_Grayscale8);
        QPainter painter(&gray);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setTransform(LayerRenderer::pixelToDocument(folder.maskPlacement.value_or(folder.transform), mask.size()));
        painter.drawImage(0, 0, mask);
    }
    const QRect canvas(QPoint(), document.canvasSize);
    record.mask = Record::Mask{rectOf(canvas), 0, !folder.maskEnabled, false};
    record.channels.push_back({-2, channel(grayPlane(gray, canvas, deep), canvas.width(), canvas.height(), deep)});
}

QByteArray layerSection(const Document &document, bool deep)
{
    QHash<QUuid, QVector<const Layer *>> children;
    for (const Layer &layer : document.layers) children[layer.parentId.value_or(QUuid())].push_back(&layer);
    QVector<Record> records;
    // Photoshop's order is bottom to top; a group is its divider, its contents, then the group itself.
    std::function<void(const QUuid &, int)> visit = [&](const QUuid &parent, int depth) {
        if (depth > 64) return;
        for (const Layer *layer : children.value(parent)) {
            if (layer->group) {
                Record divider; divider.name = QStringLiteral("</Layer group>"); divider.section = 3; divider.channels = emptyChannels();
                records.push_back(divider);
                visit(layer->id, depth + 1);
                Record group; group.name = layer->name; group.section = 1; group.blend = QStringLiteral("pass");
                group.opacity = layer->opacity; group.hidden = !layer->visible; group.channels = emptyChannels();
                addFolderMask(document, *layer, group, deep);
                records.push_back(group);
            } else if (layer->adjustment.isEmpty()) {
                records.push_back(layerRecord(document, *layer, deep));
            }
        }
    };
    visit(QUuid(), 0);

    Bytes info;
    info.i16(-int(records.size()));
    for (const Record &record : std::as_const(records)) {
        info.i32(record.rect.top); info.i32(record.rect.left); info.i32(record.rect.bottom); info.i32(record.rect.right);
        info.u16(record.channels.size());
        for (const Channel &c : record.channels) { info.i16(c.id); info.u32(c.data.size()); }
        info.ascii("8BIM"); info.ascii(record.blend.toLatin1().constData());
        info.u8(int(std::round(std::clamp(record.opacity, 0.0, 1.0) * 255)));
        info.u8(record.clipping ? 1 : 0);
        // Bit 1 hides the layer; bit 3 says bit 4 means something, and bit 4 that a group's pixels don't.
        info.u8((record.hidden ? 2 : 0) | (record.section ? 8 | 16 : 0));
        info.u8(0);
        Bytes extra;
        if (record.mask) {
            extra.u32(20);
            extra.i32(record.mask->rect.top); extra.i32(record.mask->rect.left); extra.i32(record.mask->rect.bottom); extra.i32(record.mask->rect.right);
            extra.u8(record.mask->defaultValue);
            extra.u8((record.mask->unlinked ? 1 : 0) | (record.mask->disabled ? 2 : 0));
            extra.zeros(2);
        } else {
            extra.u32(0);
        }
        extra.u32(0); // no blending ranges
        // A Pascal name in Mac Roman: ASCII kept, the rest as '?'; the full name follows as Unicode.
        QByteArray name;
        for (const QChar ch : record.name) { name.append(ch.unicode() < 128 ? char(ch.unicode()) : '?'); if (name.size() == 255) break; }
        extra.u8(name.size()); extra.data(name);
        extra.zeros((4 - (name.size() + 1) % 4) % 4);
        Bytes unicode;
        unicode.u32(record.name.size());
        for (const QChar ch : record.name) unicode.u16(ch.unicode());
        extra.block("luni", unicode.bytes);
        if (record.section) {
            Bytes divider;
            divider.u32(*record.section);
            if (*record.section != 3) { divider.ascii("8BIM"); divider.ascii(record.blend.toLatin1().constData()); }
            extra.block("lsct", divider.bytes);
        }
        info.data(extra.bytes, true);
    }
    for (const Record &record : std::as_const(records)) for (const Channel &c : record.channels) info.data(c.data);
    if (info.bytes.size() % 2 == 1) info.zeros(1);
    Bytes section;
    section.data(info.bytes, true);
    section.u32(0); // no global layer mask
    return section.bytes;
}

// Resolution, and the sRGB profile the pixels are in.
QByteArray resources(double resolution)
{
    Bytes out;
    const auto resource = [&](int id, const QByteArray &body) {
        out.ascii("8BIM"); out.u16(id); out.u16(0); // empty, padded name
        out.u32(body.size()); out.data(body);
        if (body.size() % 2 == 1) out.zeros(1);
    };
    Bytes resolutionInfo;
    const qint64 fixed = qint64(std::clamp(resolution, 1.0, 9600.0) * 65536);
    resolutionInfo.u32(fixed); resolutionInfo.u16(1); resolutionInfo.u16(1); // pixels per inch, shown in inches
    resolutionInfo.u32(fixed); resolutionInfo.u16(1); resolutionInfo.u16(1);
    resource(1005, resolutionInfo.bytes);
    const QByteArray profile = QColorSpace(QColorSpace::SRgb).iccProfile();
    if (!profile.isEmpty()) resource(1039, profile);
    return out.bytes;
}
} // namespace

QByteArray packBits(const uchar *row, int count)
{
    QByteArray out;
    out.reserve(count + count / 128 + 1);
    int index = 0;
    while (index < count) {
        // A run of three or more of the same byte packs; anything shorter goes out as it is.
        int run = 1;
        while (index + run < count && run < 128 && row[index + run] == row[index]) ++run;
        if (run >= 3) { out.append(char(qint8(1 - run))); out.append(char(row[index])); index += run; continue; }
        int end = index;
        while (end < count && end - index < 128) {
            if (end + 2 < count && row[end] == row[end + 1] && row[end] == row[end + 2]) break;
            ++end;
        }
        out.append(char(end - index - 1));
        out.append(reinterpret_cast<const char *>(row + index), end - index);
        index = end;
    }
    return out;
}

QString blendKey(BlendMode mode)
{
    switch (mode) {
    case BlendMode::Normal: return QStringLiteral("norm");
    case BlendMode::Multiply: return QStringLiteral("mul ");
    case BlendMode::Screen: return QStringLiteral("scrn");
    case BlendMode::Overlay: return QStringLiteral("over");
    case BlendMode::SoftLight: return QStringLiteral("sLit");
    case BlendMode::Darken: return QStringLiteral("dark");
    case BlendMode::Lighten: return QStringLiteral("lite");
    case BlendMode::Difference: return QStringLiteral("diff");
    case BlendMode::ColorDodge: return QStringLiteral("div ");
    case BlendMode::ColorBurn: return QStringLiteral("idiv");
    case BlendMode::Hue: return QStringLiteral("hue ");
    case BlendMode::Saturation: return QStringLiteral("sat ");
    case BlendMode::Color: return QStringLiteral("colr");
    case BlendMode::Luminosity: return QStringLiteral("lum ");
    case BlendMode::LinearBurn: return QStringLiteral("lbrn");
    case BlendMode::LinearDodge: return QStringLiteral("lddg");
    case BlendMode::HardLight: return QStringLiteral("hLit");
    case BlendMode::VividLight: return QStringLiteral("vLit");
    case BlendMode::LinearLight: return QStringLiteral("lLit");
    case BlendMode::PinLight: return QStringLiteral("pLit");
    case BlendMode::HardMix: return QStringLiteral("hMix");
    case BlendMode::Exclusion: return QStringLiteral("smud");
    case BlendMode::Subtract: return QStringLiteral("fsub");
    case BlendMode::Divide: return QStringLiteral("fdiv");
    }
    return QStringLiteral("norm");
}

QVector<Note> notes(const Document &document)
{
    QVector<Note> result;
    for (auto it = document.layers.crbegin(); it != document.layers.crend(); ++it) {
        const Layer &layer = *it;
        if (!layer.adjustment.isEmpty()) { result.push_back({layer.name, Note::Change::AdjustmentLeftOut}); continue; }
        if (layer.group) continue;
        if (drawsEffects(layer)) { result.push_back({layer.name, Note::Change::EffectsAsPixels}); continue; }
        if (layer.text) result.push_back({layer.name, Note::Change::TextAsPixels});
    }
    return result;
}

QString summary(const QVector<Note> &notes)
{
    const auto names = [&](Note::Change change) {
        QStringList matching;
        for (const Note &note : notes) if (note.change == change) matching << note.layerName;
        if (matching.isEmpty()) return QString();
        QStringList shown;
        for (int i = 0; i < std::min<int>(3, matching.size()); ++i) shown << QStringLiteral("“%1”").arg(matching.at(i));
        const QString list = shown.join(QStringLiteral(", "));
        return matching.size() > 3 ? QStringLiteral("%1 and %2 more").arg(list).arg(matching.size() - 3) : list;
    };
    QStringList lines;
    if (const QString n = names(Note::Change::TextAsPixels); !n.isEmpty()) lines << QStringLiteral("Text drawn as pixels: ") + n;
    if (const QString n = names(Note::Change::EffectsAsPixels); !n.isEmpty()) lines << QStringLiteral("Effects drawn into the pixels: ") + n;
    if (const QString n = names(Note::Change::AdjustmentLeftOut); !n.isEmpty()) lines << QStringLiteral("Adjustment layers left out: ") + n;
    return lines.join(QLatin1Char('\n'));
}

bool sixteenBit(const Document &document)
{
    return std::any_of(document.layers.cbegin(), document.layers.cend(), [](const Layer &layer) { return DeepImage::isDeep(layer.image); });
}

QByteArray write(const Document &document, QString *error)
{
    const int width = document.canvasSize.width(), height = document.canvasSize.height();
    if (width < 1 || height < 1 || width > DocumentLimits::maxSide || height > DocumentLimits::maxSide) {
        if (error) *error = QStringLiteral("The document is too large for a Photoshop file.");
        return {};
    }
    const bool deep = sixteenBit(document);
    const QImage composite = LayerRenderer::flattened(document);
    if (composite.isNull() || composite.size() != document.canvasSize) {
        if (error) *error = QStringLiteral("The canvas is too large to allocate for export.");
        return {};
    }
    Bytes file;
    // Header: RGB with an alpha channel, which the negative layer count makes the composite's transparency.
    file.ascii("8BPS"); file.u16(1); file.zeros(6);
    file.u16(4); file.u32(height); file.u32(width); file.u16(deep ? 16 : 8); file.u16(3);
    file.u32(0); // no color mode data
    file.data(resources(document.resolution), true);
    file.data(layerSection(document, deep), true);
    // The flattened image: every plane's rows counted first, then the planes, R, G, B and A.
    const QVector<QByteArray> rgba = planes(composite, QRect(0, 0, width, height), deep);
    if (deep) {
        file.u16(0);
        for (const QByteArray &plane : rgba) file.data(plane);
    } else {
        file.u16(1);
        QVector<QVector<QByteArray>> packed(4);
        for (int c = 0; c < 4; ++c)
            for (int y = 0; y < height; ++y) packed[c].push_back(packBits(reinterpret_cast<const uchar *>(rgba[c].constData()) + qsizetype(y) * width, width));
        for (const auto &plane : std::as_const(packed)) for (const QByteArray &row : plane) file.u16(row.size());
        for (const auto &plane : std::as_const(packed)) for (const QByteArray &row : plane) file.data(row);
    }
    return file.bytes;
}
} // namespace compositor::PSDWriter
