#include "io/ProjectReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace compositor {
namespace {

constexpr qint64 maxManifestBytes = 4LL * 1024LL * 1024LL;
constexpr qint64 maxAssetBytes = 512LL * 1024LL * 1024LL;
constexpr qint64 maxTotalPixels = 100000000LL;

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

LayerTransform transform(const QJsonValue &value)
{
    if (!value.isObject()) invalid(QStringLiteral("missing layer transform"));
    const QJsonObject object = value.toObject();
    const QJsonObject origin = object.value(QStringLiteral("origin")).toObject();
    const QJsonObject size = object.value(QStringLiteral("size")).toObject();
    LayerTransform result;
    result.origin = QPointF(number(origin, QStringLiteral("x"), 0.0, true),
                            number(origin, QStringLiteral("y"), 0.0, true));
    result.size = QSizeF(number(size, QStringLiteral("width"), 0.0, true),
                        number(size, QStringLiteral("height"), 0.0, true));
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
    const QString path = root.filePath(QStringLiteral("images/") + fileName);
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink() || info.size() > maxAssetBytes) {
        invalid(QStringLiteral("an image is missing, unsafe, or too large"));
    }
    return path;
}

QImage loadPng(const QDir &root, const QString &fileName, qint64 &pixelBudget)
{
    const QString path = checkedAssetPath(root, fileName);
    QImage image(path, "PNG");
    if (image.isNull() || image.width() < 1 || image.height() < 1
        || image.width() > 30000 || image.height() > 30000) {
        invalid(QStringLiteral("an image is damaged or has invalid dimensions"));
    }
    const qint64 pixels = qint64(image.width()) * qint64(image.height());
    if (pixels > maxTotalPixels - pixelBudget) invalid(QStringLiteral("image pixel limit exceeded"));
    pixelBudget += pixels;
    return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}

void validateHierarchy(const QVector<Layer> &layers)
{
    QSet<QUuid> ids;
    QSet<QUuid> groups;
    for (const Layer &layer : layers) {
        if (ids.contains(layer.id)) invalid(QStringLiteral("duplicate layer identifier"));
        ids.insert(layer.id);
        if (layer.group) groups.insert(layer.id);
    }
    for (const Layer &layer : layers) {
        if (layer.parentId && !groups.contains(*layer.parentId)) {
            invalid(QStringLiteral("layer parent is missing or is not a group"));
        }
        QSet<QUuid> visited{layer.id};
        std::optional<QUuid> parent = layer.parentId;
        int depth = 0;
        while (parent) {
            if (visited.contains(*parent) || ++depth > 64) invalid(QStringLiteral("cyclic or overly deep layer hierarchy"));
            visited.insert(*parent);
            const auto it = std::find_if(layers.cbegin(), layers.cend(), [&](const Layer &candidate) {
                return candidate.id == *parent;
            });
            if (it == layers.cend()) invalid(QStringLiteral("missing parent layer"));
            parent = it->parentId;
        }
    }
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
    if (document.formatVersion < 1 || document.formatVersion > 7) {
        throw ProjectError(QStringLiteral("This project uses unsupported format version %1.").arg(document.formatVersion));
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
        layer.opacity = number(object, QStringLiteral("opacity"), 1.0);
        if (layer.opacity < 0.0 || layer.opacity > 1.0) invalid(QStringLiteral("invalid layer opacity"));
        layer.blendMode = blendModeFromString(object.value(QStringLiteral("blendMode")).toString(QStringLiteral("Normal")));
        if (object.value(QStringLiteral("parentID")).isString()) {
            layer.parentId = uuid(object.value(QStringLiteral("parentID")), QStringLiteral("parentID"));
        }
        layer.imageFile = object.value(QStringLiteral("imageFile")).toString();
        if (!layer.imageFile.isEmpty()) {
            if (layer.imageFile != layer.id.toString(QUuid::WithoutBraces).toUpper() + QStringLiteral(".png")
                && layer.imageFile != layer.id.toString(QUuid::WithoutBraces).toLower() + QStringLiteral(".png")) {
                invalid(QStringLiteral("unexpected layer image filename"));
            }
            layer.image = loadPng(root, layer.imageFile, imagePixels);
        }
        layer.maskFile = object.value(QStringLiteral("maskFile")).toString();
        if (!layer.maskFile.isEmpty()) layer.mask = loadPng(root, layer.maskFile, maskPixels);
        layer.maskEnabled = boolean(object, QStringLiteral("maskEnabled"), true);
        layer.maskLinked = boolean(object, QStringLiteral("maskLinked"), true);
        if (!object.value(QStringLiteral("maskPlacement")).isUndefined()) { if (layer.mask.isNull()) invalid(QStringLiteral("mask placement without a mask")); layer.maskPlacement = transform(object.value(QStringLiteral("maskPlacement"))); }
        if (object.value(QStringLiteral("maskSourceID")).isString()) {
            layer.maskSourceId = uuid(object.value(QStringLiteral("maskSourceID")), QStringLiteral("maskSourceID"));
        }
        if (object.value(QStringLiteral("adjustment")).isObject()) {
            layer.adjustment = object.value(QStringLiteral("adjustment")).toObject();
            static const QSet<QString> kinds{QStringLiteral("Hue/Saturation"), QStringLiteral("Levels"), QStringLiteral("Curves"), QStringLiteral("Exposure"), QStringLiteral("Gradient Map"), QStringLiteral("Grain")};
            if (document.formatVersion < 7 || !layer.image.isNull() || layer.group || !kinds.contains(layer.adjustment.value(QStringLiteral("kind")).toString()))
                invalid(QStringLiteral("invalid adjustment layer"));
        }
        if (object.value(QStringLiteral("shape")).isObject()) layer.shape = object.value(QStringLiteral("shape")).toObject();
        if (layer.group && !layer.image.isNull()) invalid(QStringLiteral("a group cannot contain image pixels"));
        document.layers.push_back(std::move(layer));
    }

    validateHierarchy(document.layers);
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
