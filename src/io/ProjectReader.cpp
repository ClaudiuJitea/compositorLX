#include "io/ProjectReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace compositor {
namespace {

constexpr qint64 maxManifestBytes = 4LL * 1024LL * 1024LL;
constexpr qint64 maxAssetBytes = 512LL * 1024LL * 1024LL;
constexpr qint64 maxTotalPixels = 100000000LL; // 100 megapixels

[[noreturn]] void invalid(const QString &detail = {})
{
    const QString prefix = QStringLiteral("This is not a valid Compositor project");
    throw ProjectError(detail.isEmpty() ? prefix + QLatin1Char('.') : prefix + QStringLiteral(": ") + detail);
}

double number(const QJsonObject &object, const QString &key, double fallback, bool required = false)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined() && !required) return fallback;
    if (!value.isDouble() || !std::isfinite(value.toDouble())) invalid(QStringLiteral("invalid %1").arg(key));
    return value.toDouble();
}

bool boolean(const QJsonObject &object, const QString &key, bool fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined()) return fallback;
    if (!value.isBool()) invalid(QStringLiteral("invalid %1").arg(key));
    return value.toBool();
}

QUuid uuid(const QJsonValue &value, const QString &field)
{
    if (!value.isString()) invalid(QStringLiteral("invalid %1").arg(field));
    const QUuid result(value.toString());
    if (result.isNull()) invalid(QStringLiteral("invalid %1").arg(field));
    return result;
}

QPointF parsePoint(const QJsonValue &value, const QString &field)
{
    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        if (arr.size() != 2) invalid(QStringLiteral("invalid %1 array").arg(field));
        const double x = arr.at(0).toDouble(qQNaN());
        const double y = arr.at(1).toDouble(qQNaN());
        if (!std::isfinite(x) || !std::isfinite(y)) invalid(QStringLiteral("invalid %1 values").arg(field));
        return QPointF(x, y);
    }
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        return QPointF(number(obj, QStringLiteral("x"), 0.0, true),
                       number(obj, QStringLiteral("y"), 0.0, true));
    }
    invalid(QStringLiteral("invalid %1").arg(field));
}

QSizeF parseSize(const QJsonValue &value, const QString &field)
{
    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        if (arr.size() != 2) invalid(QStringLiteral("invalid %1 array").arg(field));
        const double w = arr.at(0).toDouble(qQNaN());
        const double h = arr.at(1).toDouble(qQNaN());
        if (!std::isfinite(w) || !std::isfinite(h)) invalid(QStringLiteral("invalid %1 values").arg(field));
        return QSizeF(w, h);
    }
    if (value.isObject()) {
        const QJsonObject obj = value.toObject();
        return QSizeF(number(obj, QStringLiteral("width"), 0.0, true),
                      number(obj, QStringLiteral("height"), 0.0, true));
    }
    invalid(QStringLiteral("invalid %1").arg(field));
}

LayerTransform transform(const QJsonValue &value)
{
    if (!value.isObject()) invalid(QStringLiteral("missing layer transform"));
    const QJsonObject object = value.toObject();
    LayerTransform result;
    result.origin = parsePoint(object.value(QStringLiteral("origin")), QStringLiteral("origin"));
    result.size = parseSize(object.value(QStringLiteral("size")), QStringLiteral("size"));
    result.rotation = number(object, QStringLiteral("rotation"), 0.0);
    result.flipX = boolean(object, QStringLiteral("flipX"), false);
    result.flipY = boolean(object, QStringLiteral("flipY"), false);
    const QJsonValue sampling = object.value(QStringLiteral("sampling"));
    if (!sampling.isUndefined() && !sampling.isString()) invalid(QStringLiteral("invalid sampling"));
    result.sampling = samplingFromString(sampling.toString(QStringLiteral("High quality")));
    if (!result.isValid()) invalid(QStringLiteral("layer transform is outside supported limits"));
    return result;
}

QString checkedAssetPath(const QDir &root, const QString &fileName)
{
    if (fileName.isEmpty() || fileName != QFileInfo(fileName).fileName()) {
        invalid(QStringLiteral("unsafe image filename"));
    }
    const QDir imagesDir(root.filePath(QStringLiteral("images")));
    const QString imagesCanonical = imagesDir.canonicalPath();
    const QString path = imagesDir.filePath(fileName);
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink() || info.size() > maxAssetBytes) {
        invalid(QStringLiteral("an image is missing, unsafe, or too large"));
    }
    const QString canonical = info.canonicalFilePath();
    if (imagesCanonical.isEmpty() || !canonical.startsWith(imagesCanonical + QLatin1Char('/'))) {
        invalid(QStringLiteral("image path traversal detected"));
    }
    return path;
}

QImage loadPng(const QDir &root, const QString &fileName, qint64 &pixelBudget, bool isMask)
{
    const QString path = checkedAssetPath(root, fileName);
    QImageReader reader(path, "PNG");
    if (!reader.canRead()) {
        invalid(QStringLiteral("an image is damaged or cannot be read"));
    }
    const QSize size = reader.size();
    if (size.width() < 1 || size.height() < 1 || size.width() > 30000 || size.height() > 30000) {
        invalid(QStringLiteral("an image has invalid dimensions"));
    }
    const qint64 pixels = qint64(size.width()) * qint64(size.height());
    if (pixels > maxTotalPixels - pixelBudget) {
        invalid(QStringLiteral("image pixel limit exceeded"));
    }

    QImage image = reader.read();
    if (image.isNull()) {
        invalid(QStringLiteral("an image could not be decoded"));
    }
    pixelBudget += pixels;
    if (isMask) {
        return image.convertToFormat(QImage::Format_Grayscale8);
    }
    return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}

void validateHierarchy(const QVector<Layer> &layers)
{
    QSet<QUuid> ids;
    QSet<QUuid> groups;
    for (const Layer &layer : layers) {
        if (ids.contains(layer.id)) invalid(QStringLiteral("duplicate layer identifier"));
        ids.insert(layer.id);
        if (layer.group) {
            groups.insert(layer.id);
            if (!layer.image.isNull() || !layer.imageFile.isEmpty()) {
                invalid(QStringLiteral("a group cannot contain image pixels"));
            }
        }
    }
    for (const Layer &layer : layers) {
        if (layer.parentId && !groups.contains(*layer.parentId)) {
            invalid(QStringLiteral("layer parent is missing or is not a group"));
        }
        QSet<QUuid> visited{layer.id};
        std::optional<QUuid> parent = layer.parentId;
        int depth = 0;
        while (parent) {
            if (visited.contains(*parent) || ++depth > 64) {
                invalid(QStringLiteral("cyclic or overly deep layer hierarchy"));
            }
            visited.insert(*parent);
            const auto it = std::find_if(layers.cbegin(), layers.cend(), [&](const Layer &candidate) {
                return candidate.id == *parent;
            });
            if (it == layers.cend() || !it->group) invalid(QStringLiteral("missing parent group layer"));
            parent = it->parentId;
        }
        if (layer.group && visited.size() > 64) {
            invalid(QStringLiteral("overly deep group nesting"));
        }
    }
}

void validateLiveMaskGraph(const QVector<Layer> &layers)
{
    QHash<QUuid, const Layer *> byId;
    for (const Layer &layer : layers) {
        byId.insert(layer.id, &layer);
    }
    for (const Layer &layer : layers) {
        QSet<QUuid> visited;
        std::optional<QUuid> current = layer.id;
        while (current) {
            if (visited.size() >= 256 || visited.contains(*current)) {
                invalid(QStringLiteral("cyclic or overly deep clipping mask chain"));
            }
            visited.insert(*current);
            const Layer *record = byId.value(*current, nullptr);
            if (!record) invalid(QStringLiteral("referenced clipping mask layer missing"));
            if (record->maskSourceId) {
                if (record->group) invalid(QStringLiteral("a group cannot have a clipping mask source"));
                if (*record->maskSourceId == record->id) invalid(QStringLiteral("self-referencing clipping mask"));
                const Layer *source = byId.value(*record->maskSourceId, nullptr);
                if (!source) invalid(QStringLiteral("clipping mask source layer does not exist"));
                if (source->group) invalid(QStringLiteral("clipping mask source cannot be a group"));
                if (!source->adjustment.isEmpty()) invalid(QStringLiteral("clipping mask source cannot be an adjustment layer"));
            }
            current = record->maskSourceId;
        }
    }
}

void validateGuides(const QVector<CanvasGuide> &guides, int version)
{
    if (version < 8) {
        if (!guides.isEmpty()) invalid(QStringLiteral("guides require format version 8 or later"));
        return;
    }
    if (guides.size() > 1000) {
        invalid(QStringLiteral("guide count exceeds 1,000 limit"));
    }
    QSet<QUuid> ids;
    for (const CanvasGuide &guide : guides) {
        if (!guide.isValid()) invalid(QStringLiteral("invalid guide coordinates or identifier"));
        if (ids.contains(guide.id)) invalid(QStringLiteral("duplicate guide identifier"));
        ids.insert(guide.id);
    }
}

void validateAdjustmentSettings(const QJsonObject &adj, int version)
{
    const QString kindStr = adj.value(QStringLiteral("kind")).toString();
    const auto kind = adjustmentKindFromString(kindStr);
    if (!kind) invalid(QStringLiteral("unknown adjustment kind: %1").arg(kindStr));

    if (isVersion9Adjustment(*kind) && version < 9) {
        invalid(QStringLiteral("%1 adjustment requires format version 9").arg(kindStr));
    }

    const double hue = number(adj, QStringLiteral("hue"), 0.0);
    const double saturation = number(adj, QStringLiteral("saturation"), 0.0);
    const double lightness = number(adj, QStringLiteral("lightness"), 0.0);
    if (std::abs(hue) > 360.0 || std::abs(saturation) > 100.0 || std::abs(lightness) > 100.0) {
        invalid(QStringLiteral("adjustment HSV parameters out of range"));
    }

    if (*kind == AdjustmentKind::GaussianBlur) {
        const double radius = number(adj, QStringLiteral("blurRadius"), 10.0);
        if (radius < 0.1 || radius > 250.0) invalid(QStringLiteral("blurRadius out of range (0.1..250)"));
    } else if (*kind == AdjustmentKind::MotionBlur) {
        const double angle = number(adj, QStringLiteral("motionAngle"), 0.0);
        const double dist = number(adj, QStringLiteral("motionDistance"), 10.0);
        if (angle < -90.0 || angle > 90.0 || dist < 1.0 || dist > 2000.0) {
            invalid(QStringLiteral("motion blur parameters out of range"));
        }
    } else if (*kind == AdjustmentKind::AddNoise) {
        const double amount = number(adj, QStringLiteral("noiseAmount"), 10.0);
        if (amount < 0.1 || amount > 400.0) invalid(QStringLiteral("noiseAmount out of range (0.1..400)"));
    } else if (*kind == AdjustmentKind::BlackWhite) {
        if (adj.contains(QStringLiteral("blackWhiteSettings"))) {
            const QJsonObject bw = adj.value(QStringLiteral("blackWhiteSettings")).toObject();
            for (const QString &colorKey : {QStringLiteral("reds"), QStringLiteral("yellows"), QStringLiteral("greens"),
                                            QStringLiteral("cyans"), QStringLiteral("blues"), QStringLiteral("magentas")}) {
                const double val = bw.value(colorKey).toDouble(0.0);
                if (!std::isfinite(val) || val < -200.0 || val > 300.0) {
                    invalid(QStringLiteral("Black & White parameter %1 out of range (-200..300)").arg(colorKey));
                }
            }
            if (bw.contains(QStringLiteral("tintHue"))) {
                const double hue = bw.value(QStringLiteral("tintHue")).toDouble(0.0);
                if (!std::isfinite(hue) || hue < 0.0 || hue > 360.0) invalid(QStringLiteral("Black & White tintHue out of range (0..360)"));
            }
            if (bw.contains(QStringLiteral("tintSaturation"))) {
                const double sat = bw.value(QStringLiteral("tintSaturation")).toDouble(0.0);
                if (!std::isfinite(sat) || sat < 0.0 || sat > 100.0) invalid(QStringLiteral("Black & White tintSaturation out of range (0..100)"));
            }
        }
    } else if (*kind == AdjustmentKind::ColorBalance) {
        if (adj.contains(QStringLiteral("colorBalanceSettings"))) {
            const QJsonObject cb = adj.value(QStringLiteral("colorBalanceSettings")).toObject();
            for (const QString &cbKey : {QStringLiteral("shadowCyanRed"), QStringLiteral("shadowMagentaGreen"), QStringLiteral("shadowYellowBlue"),
                                         QStringLiteral("midCyanRed"), QStringLiteral("midMagentaGreen"), QStringLiteral("midYellowBlue"),
                                         QStringLiteral("highlightCyanRed"), QStringLiteral("highlightMagentaGreen"), QStringLiteral("highlightYellowBlue")}) {
                const double val = cb.value(cbKey).toDouble(0.0);
                if (!std::isfinite(val) || val < -100.0 || val > 100.0) {
                    invalid(QStringLiteral("Color Balance parameter %1 out of range (-100..100)").arg(cbKey));
                }
            }
        }
    }
}

std::optional<LayerEffects> parseEffects(const QJsonObject &obj)
{
    if (obj.isEmpty()) return std::nullopt;
    LayerEffects effects;
    if (obj.contains(QStringLiteral("stroke")) && obj.value(QStringLiteral("stroke")).isObject()) {
        const QJsonObject s = obj.value(QStringLiteral("stroke")).toObject();
        StrokeEffect stroke;
        if (s.contains(QStringLiteral("enabled"))) stroke.enabled = boolean(s, QStringLiteral("enabled"), true);
        stroke.size = number(s, QStringLiteral("size"), 4.0);
        stroke.red = number(s, QStringLiteral("red"), 0.0);
        stroke.green = number(s, QStringLiteral("green"), 0.0);
        stroke.blue = number(s, QStringLiteral("blue"), 0.0);
        stroke.opacity = number(s, QStringLiteral("opacity"), 1.0);
        stroke.inside = boolean(s, QStringLiteral("inside"), false);
        if (!stroke.isValid()) invalid(QStringLiteral("invalid stroke effect"));
        effects.stroke = stroke;
    }
    if (obj.contains(QStringLiteral("shadow")) && obj.value(QStringLiteral("shadow")).isObject()) {
        const QJsonObject s = obj.value(QStringLiteral("shadow")).toObject();
        ShadowEffect shadow;
        if (s.contains(QStringLiteral("enabled"))) shadow.enabled = boolean(s, QStringLiteral("enabled"), true);
        shadow.angle = number(s, QStringLiteral("angle"), 90.0);
        shadow.distance = number(s, QStringLiteral("distance"), 20.0);
        shadow.blur = number(s, QStringLiteral("blur"), 20.0);
        shadow.red = number(s, QStringLiteral("red"), 0.0);
        shadow.green = number(s, QStringLiteral("green"), 0.0);
        shadow.blue = number(s, QStringLiteral("blue"), 0.0);
        shadow.opacity = number(s, QStringLiteral("opacity"), 0.5);
        if (!shadow.isValid()) invalid(QStringLiteral("invalid shadow effect"));
        effects.shadow = shadow;
    }
    if (obj.contains(QStringLiteral("colorOverlay")) && obj.value(QStringLiteral("colorOverlay")).isObject()) {
        const QJsonObject c = obj.value(QStringLiteral("colorOverlay")).toObject();
        ColorOverlayEffect overlay;
        if (c.contains(QStringLiteral("enabled"))) overlay.enabled = boolean(c, QStringLiteral("enabled"), true);
        overlay.red = number(c, QStringLiteral("red"), 0.0);
        overlay.green = number(c, QStringLiteral("green"), 0.0);
        overlay.blue = number(c, QStringLiteral("blue"), 0.0);
        overlay.opacity = number(c, QStringLiteral("opacity"), 1.0);
        if (!overlay.isValid()) invalid(QStringLiteral("invalid color overlay effect"));
        effects.colorOverlay = overlay;
    }
    if (obj.contains(QStringLiteral("innerShadow")) && obj.value(QStringLiteral("innerShadow")).isObject()) {
        const QJsonObject s = obj.value(QStringLiteral("innerShadow")).toObject();
        InnerShadowEffect innerShadow;
        if (s.contains(QStringLiteral("enabled"))) innerShadow.enabled = boolean(s, QStringLiteral("enabled"), true);
        innerShadow.angle = number(s, QStringLiteral("angle"), 90.0);
        innerShadow.distance = number(s, QStringLiteral("distance"), 10.0);
        innerShadow.blur = number(s, QStringLiteral("blur"), 10.0);
        innerShadow.red = number(s, QStringLiteral("red"), 0.0);
        innerShadow.green = number(s, QStringLiteral("green"), 0.0);
        innerShadow.blue = number(s, QStringLiteral("blue"), 0.0);
        innerShadow.opacity = number(s, QStringLiteral("opacity"), 0.5);
        if (!innerShadow.isValid()) invalid(QStringLiteral("invalid inner shadow effect"));
        effects.innerShadow = innerShadow;
    }
    if (obj.contains(QStringLiteral("outerGlow")) && obj.value(QStringLiteral("outerGlow")).isObject()) {
        const QJsonObject g = obj.value(QStringLiteral("outerGlow")).toObject();
        OuterGlowEffect glow;
        if (g.contains(QStringLiteral("enabled"))) glow.enabled = boolean(g, QStringLiteral("enabled"), true);
        glow.size = number(g, QStringLiteral("size"), 20.0);
        glow.red = number(g, QStringLiteral("red"), 1.0);
        glow.green = number(g, QStringLiteral("green"), 1.0);
        glow.blue = number(g, QStringLiteral("blue"), 1.0);
        glow.opacity = number(g, QStringLiteral("opacity"), 0.75);
        if (!glow.isValid()) invalid(QStringLiteral("invalid outer glow effect"));
        effects.outerGlow = glow;
    }
    if (obj.contains(QStringLiteral("innerGlow")) && obj.value(QStringLiteral("innerGlow")).isObject()) {
        const QJsonObject g = obj.value(QStringLiteral("innerGlow")).toObject();
        InnerGlowEffect glow;
        if (g.contains(QStringLiteral("enabled"))) glow.enabled = boolean(g, QStringLiteral("enabled"), true);
        glow.size = number(g, QStringLiteral("size"), 10.0);
        glow.red = number(g, QStringLiteral("red"), 1.0);
        glow.green = number(g, QStringLiteral("green"), 1.0);
        glow.blue = number(g, QStringLiteral("blue"), 1.0);
        glow.opacity = number(g, QStringLiteral("opacity"), 0.75);
        if (!glow.isValid()) invalid(QStringLiteral("invalid inner glow effect"));
        effects.innerGlow = glow;
    }
    if (effects.isEmpty()) return std::nullopt;
    return effects;
}

std::optional<TextStyle> parseText(const QJsonObject &obj)
{
    if (obj.isEmpty()) return std::nullopt;
    TextStyle text;
    text.content = obj.value(QStringLiteral("content")).toString(QStringLiteral("Text"));
    text.fontName = obj.value(QStringLiteral("fontName")).toString(QStringLiteral("Helvetica"));
    text.fontSize = number(obj, QStringLiteral("fontSize"), 72.0);
    text.red = number(obj, QStringLiteral("red"), 0.0);
    text.green = number(obj, QStringLiteral("green"), 0.0);
    text.blue = number(obj, QStringLiteral("blue"), 0.0);
    const QString alignStr = obj.value(QStringLiteral("alignment")).toString(QStringLiteral("Left"));
    const auto align = textAlignmentFromString(alignStr);
    text.alignment = align.value_or(TextAlignment::Left);
    text.tracking = number(obj, QStringLiteral("tracking"), 0.0);
    text.leading = number(obj, QStringLiteral("leading"), 0.0);
    if (obj.contains(QStringLiteral("boxSize"))) {
        text.boxSize = parseSize(obj.value(QStringLiteral("boxSize")), QStringLiteral("boxSize"));
    }
    if (!text.isValid()) invalid(QStringLiteral("invalid layer text style"));
    return text;
}

std::optional<LayerShapeStyle> parseShape(const QJsonObject &obj)
{
    if (obj.isEmpty() || obj.value(QStringLiteral("kind")).toString() == QStringLiteral("Text")) {
        return std::nullopt;
    }
    const QString kindStr = obj.value(QStringLiteral("kind")).toString();
    const auto kind = shapeKindFromString(kindStr);
    if (!kind) return std::nullopt;
    LayerShapeStyle style;
    style.kind = *kind;
    style.red = number(obj, QStringLiteral("red"), 0.0);
    style.green = number(obj, QStringLiteral("green"), 0.0);
    style.blue = number(obj, QStringLiteral("blue"), 0.0);
    style.cornerRadius = number(obj, QStringLiteral("cornerRadius"), 0.0);
    if (obj.contains(QStringLiteral("lineWidth"))) {
        style.lineWidth = number(obj, QStringLiteral("lineWidth"), 1.0);
    }
    if (obj.contains(QStringLiteral("start"))) {
        style.start = parsePoint(obj.value(QStringLiteral("start")), QStringLiteral("start"));
    }
    if (obj.contains(QStringLiteral("end"))) {
        style.end = parsePoint(obj.value(QStringLiteral("end")), QStringLiteral("end"));
    }
    if (!style.isValid()) invalid(QStringLiteral("invalid shape style"));
    return style;
}

void migrateLegacyLxText(Layer &layer)
{
    if (layer.text.has_value() || layer.shape.isEmpty()) return;
    if (layer.shape.value(QStringLiteral("kind")).toString() != QStringLiteral("Text")) return;

    TextStyle text;
    text.content = layer.shape.value(QStringLiteral("text")).toString();
    text.fontName = layer.shape.value(QStringLiteral("fontFamily")).toString(QStringLiteral("Helvetica"));
    text.fontSize = layer.shape.value(QStringLiteral("pixelSize")).toDouble(32.0);
    const QColor color(layer.shape.value(QStringLiteral("fill")).toString());
    if (color.isValid()) {
        text.red = color.redF();
        text.green = color.greenF();
        text.blue = color.blueF();
    }
    const int alignInt = layer.shape.value(QStringLiteral("alignment")).toInt(0);
    if (alignInt == 1) text.alignment = TextAlignment::Center;
    else if (alignInt == 2) text.alignment = TextAlignment::Right;
    else text.alignment = TextAlignment::Left;

    if (layer.shape.value(QStringLiteral("areaText")).toBool()) {
        const double bw = layer.shape.value(QStringLiteral("baseWidth")).toDouble(layer.transform.size.width());
        const double bh = layer.shape.value(QStringLiteral("baseHeight")).toDouble(layer.transform.size.height());
        if (bw >= 16.0 && bh >= 16.0) text.boxSize = QSizeF(bw, bh);
    }
    layer.text = text;
}

} // namespace

ProjectError::ProjectError(const QString &message)
    : std::runtime_error(message.toStdString()), message_(message)
{
}

QString ProjectError::message() const
{
    return message_;
}

Document ProjectReader::load(const QString &projectDirectory)
{
    const QFileInfo rootInfo(projectDirectory);
    if (!rootInfo.exists() || !rootInfo.isDir() || rootInfo.isSymLink()) invalid();
    const QDir root(rootInfo.absoluteFilePath());
    const QFileInfo manifestInfo(root.filePath(QStringLiteral("manifest.json")));
    if (!manifestInfo.exists() || !manifestInfo.isFile() || manifestInfo.isSymLink()
        || manifestInfo.size() > maxManifestBytes) {
        invalid(QStringLiteral("manifest.json is missing or too large"));
    }

    QFile manifestFile(manifestInfo.absoluteFilePath());
    if (!manifestFile.open(QIODevice::ReadOnly)) invalid(QStringLiteral("manifest.json cannot be read"));
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
    if (json.isNull() || !json.isObject()) invalid(QStringLiteral("manifest JSON is damaged"));
    const QJsonObject manifest = json.object();
    if (manifest.value(QStringLiteral("format")).toString() != QStringLiteral("com.compositor.project")) invalid();

    Document document;
    document.formatVersion = manifest.value(QStringLiteral("version")).toInt(-1);
    if (document.formatVersion < 1 || document.formatVersion > 9) {
        throw ProjectError(QStringLiteral("This project uses format version %1. This app supports versions 1–9.").arg(document.formatVersion));
    }
    if (manifest.value(QStringLiteral("colorSpace")).toString() != QStringLiteral("sRGB")) {
        invalid(QStringLiteral("only the sRGB working space is supported"));
    }
    document.id = uuid(manifest.value(QStringLiteral("documentID")), QStringLiteral("documentID"));
    const int width = manifest.value(QStringLiteral("width")).toInt(-1);
    const int height = manifest.value(QStringLiteral("height")).toInt(-1);
    if (width < 1 || width > 30000 || height < 1 || height > 30000) invalid(QStringLiteral("invalid canvas size"));
    document.canvasSize = QSize(width, height);
    document.resolution = number(manifest, QStringLiteral("resolution"), 72.0);
    if (document.resolution < 1.0 || document.resolution > 9600.0) invalid(QStringLiteral("invalid resolution"));
    if (!manifest.value(QStringLiteral("activeLayerID")).isNull()
        && !manifest.value(QStringLiteral("activeLayerID")).isUndefined()) {
        document.activeLayerId = uuid(manifest.value(QStringLiteral("activeLayerID")), QStringLiteral("activeLayerID"));
    }

    // Top-level guides
    if (manifest.contains(QStringLiteral("guides"))) {
        const QJsonValue guidesValue = manifest.value(QStringLiteral("guides"));
        if (!guidesValue.isNull() && !guidesValue.isArray()) invalid(QStringLiteral("invalid guides structure"));
        if (guidesValue.isArray()) {
            for (const QJsonValue &gVal : guidesValue.toArray()) {
                if (!gVal.isObject()) invalid(QStringLiteral("invalid guide entry"));
                const QJsonObject gObj = gVal.toObject();
                CanvasGuide guide;
                guide.id = uuid(gObj.value(QStringLiteral("id")), QStringLiteral("guide id"));
                const QString axisStr = gObj.value(QStringLiteral("axis")).toString();
                const auto axis = guideAxisFromString(axisStr);
                if (!axis) invalid(QStringLiteral("invalid guide axis: %1").arg(axisStr));
                guide.axis = *axis;
                guide.position = number(gObj, QStringLiteral("position"), 0.0, true);
                document.guides.append(guide);
            }
        }
    }
    validateGuides(document.guides, document.formatVersion);

    const QJsonValue layerValue = manifest.value(QStringLiteral("layers"));
    if (!layerValue.isArray() || layerValue.toArray().size() > 10000) invalid(QStringLiteral("invalid layer list"));
    qint64 imagePixels = 0;
    qint64 maskPixels = 0;

    for (const QJsonValue &value : layerValue.toArray()) {
        if (!value.isObject()) invalid(QStringLiteral("invalid layer record"));
        const QJsonObject object = value.toObject();
        Layer layer;
        layer.id = uuid(object.value(QStringLiteral("id")), QStringLiteral("layer id"));
        layer.name = object.value(QStringLiteral("name")).toString().trimmed();
        if (layer.name.isEmpty() || layer.name.toUtf8().size() > 16384) invalid(QStringLiteral("invalid layer name"));
        layer.visible = boolean(object, QStringLiteral("isVisible"), true);
        layer.transform = transform(object.value(QStringLiteral("transform")));
        layer.group = boolean(object, QStringLiteral("isGroup"), false);

        if (document.formatVersion == 1 && layer.group) {
            invalid(QStringLiteral("groups require format version 2 or later"));
        }

        layer.opacity = number(object, QStringLiteral("opacity"), 1.0);
        if (layer.opacity < 0.0 || layer.opacity > 1.0) invalid(QStringLiteral("invalid layer opacity"));

        const QString blendStr = object.value(QStringLiteral("blendMode")).toString(QStringLiteral("Normal"));
        const auto blend = blendModeFromString(blendStr);
        if (!blend) invalid(QStringLiteral("unknown or unsupported blend mode: %1").arg(blendStr));
        layer.blendMode = *blend;

        // Version-gated appearance rules
        if (document.formatVersion < 3) {
            if (layer.opacity != 1.0 || layer.blendMode != BlendMode::Normal) {
                invalid(QStringLiteral("opacity and blendMode require format version 3 or later"));
            }
        }
        if (layer.group) {
            if (layer.blendMode != BlendMode::Normal) {
                invalid(QStringLiteral("folder blend mode must be Normal"));
            }
            if (document.formatVersion < 8 && layer.opacity != 1.0) {
                invalid(QStringLiteral("folder opacity requires format version 8 or later"));
            }
        }

        if (object.contains(QStringLiteral("parentID"))) {
            if (document.formatVersion == 1) invalid(QStringLiteral("parentID requires format version 2 or later"));
            layer.parentId = uuid(object.value(QStringLiteral("parentID")), QStringLiteral("parentID"));
        }

        layer.imageFile = object.value(QStringLiteral("imageFile")).toString();
        if (!layer.imageFile.isEmpty()) {
            if (layer.group) invalid(QStringLiteral("a group cannot contain an image file"));
            const QString expectedId = layer.id.toString(QUuid::WithoutBraces);
            if (layer.imageFile.compare(expectedId + QStringLiteral(".png"), Qt::CaseInsensitive) != 0) {
                invalid(QStringLiteral("unexpected layer image filename"));
            }
            layer.image = loadPng(root, layer.imageFile, imagePixels, false);
        }

        layer.maskFile = object.value(QStringLiteral("maskFile")).toString();
        if (!layer.maskFile.isEmpty()) {
            if (document.formatVersion < 4) invalid(QStringLiteral("masks require format version 4 or later"));
            if (layer.group && document.formatVersion < 6) invalid(QStringLiteral("folder masks require format version 6 or later"));
            const QString expectedId = layer.id.toString(QUuid::WithoutBraces);
            if (layer.maskFile.compare(expectedId + QStringLiteral(".mask.png"), Qt::CaseInsensitive) != 0) {
                invalid(QStringLiteral("unexpected layer mask filename"));
            }
            layer.mask = loadPng(root, layer.maskFile, maskPixels, true);
        }

        layer.maskEnabled = boolean(object, QStringLiteral("maskEnabled"), true);
        layer.maskLinked = boolean(object, QStringLiteral("maskLinked"), true);
        if (object.contains(QStringLiteral("maskPlacement")) && !object.value(QStringLiteral("maskPlacement")).isUndefined()) {
            if (layer.mask.isNull()) invalid(QStringLiteral("mask placement without a mask"));
            layer.maskPlacement = transform(object.value(QStringLiteral("maskPlacement")));
        }

        if (object.contains(QStringLiteral("maskSourceID"))) {
            if (document.formatVersion < 5) invalid(QStringLiteral("maskSourceID requires format version 5 or later"));
            if (layer.group) invalid(QStringLiteral("groups cannot have clipping mask sources"));
            layer.maskSourceId = uuid(object.value(QStringLiteral("maskSourceID")), QStringLiteral("maskSourceID"));
        }

        if (object.contains(QStringLiteral("adjustment")) && object.value(QStringLiteral("adjustment")).isObject()) {
            if (document.formatVersion < 7) invalid(QStringLiteral("adjustments require format version 7 or later"));
            if (layer.group) invalid(QStringLiteral("groups cannot be adjustments"));
            if (!layer.image.isNull() || !layer.imageFile.isEmpty()) invalid(QStringLiteral("adjustment layer cannot carry an image"));
            layer.adjustment = object.value(QStringLiteral("adjustment")).toObject();
            validateAdjustmentSettings(layer.adjustment, document.formatVersion);
        }

        if (object.contains(QStringLiteral("effects")) && object.value(QStringLiteral("effects")).isObject()) {
            if (layer.group) invalid(QStringLiteral("groups cannot have layer effects"));
            if (!layer.adjustment.isEmpty()) invalid(QStringLiteral("adjustments cannot have layer effects"));
            layer.effects = parseEffects(object.value(QStringLiteral("effects")).toObject());
        }

        if (object.contains(QStringLiteral("text")) && object.value(QStringLiteral("text")).isObject()) {
            if (layer.group) invalid(QStringLiteral("groups cannot carry text"));
            if (!layer.adjustment.isEmpty()) invalid(QStringLiteral("adjustments cannot carry text"));
            if (layer.image.isNull()) invalid(QStringLiteral("text layer requires fallback image"));
            layer.text = parseText(object.value(QStringLiteral("text")).toObject());
        }

        if (object.contains(QStringLiteral("shape")) && object.value(QStringLiteral("shape")).isObject()) {
            layer.shape = object.value(QStringLiteral("shape")).toObject();
            layer.shapeStyle = parseShape(layer.shape);
        }

        // Migrate LX legacy shape.kind == "Text"
        migrateLegacyLxText(layer);

        document.layers.push_back(std::move(layer));
    }

    validateHierarchy(document.layers);
    validateLiveMaskGraph(document.layers);

    if (document.activeLayerId) {
        const bool found = std::any_of(document.layers.cbegin(), document.layers.cend(), [&](const Layer &layer) {
            return layer.id == *document.activeLayerId;
        });
        if (!found) invalid(QStringLiteral("active layer is missing"));
    }

    document.projectPath = root.absolutePath();
    return document;
}

} // namespace compositor
