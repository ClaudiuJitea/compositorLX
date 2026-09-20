#include "io/ProjectWriter.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

namespace compositor {
namespace {

QString uuid(const QUuid &value) { return value.toString(QUuid::WithoutBraces).toUpper(); }

QString samplingName(Sampling value)
{
    if (value == Sampling::Nearest) return QStringLiteral("Nearest");
    if (value == Sampling::Smooth) return QStringLiteral("Smooth");
    return QStringLiteral("High quality");
}

QString blendName(BlendMode value)
{
    static const QStringList names = {QStringLiteral("Normal"), QStringLiteral("Multiply"), QStringLiteral("Screen"),
        QStringLiteral("Overlay"), QStringLiteral("Darken"), QStringLiteral("Lighten"), QStringLiteral("Difference"),
        QStringLiteral("Color Dodge"), QStringLiteral("Color Burn"), QStringLiteral("Hue"), QStringLiteral("Saturation"),
        QStringLiteral("Color"), QStringLiteral("Luminosity")};
    const int index = int(value);
    return index >= 0 && index < names.size() ? names.at(index) : names.constFirst();
}

QJsonObject transformObject(const LayerTransform &value)
{
    return {{QStringLiteral("origin"), QJsonObject{{QStringLiteral("x"), value.origin.x()}, {QStringLiteral("y"), value.origin.y()}}},
            {QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), value.size.width()}, {QStringLiteral("height"), value.size.height()}}},
            {QStringLiteral("rotation"), value.rotation}, {QStringLiteral("flipX"), value.flipX},
            {QStringLiteral("flipY"), value.flipY}, {QStringLiteral("sampling"), samplingName(value.sampling)}};
}

void fail(const QString &detail) { throw ProjectWriteError(QStringLiteral("Could not save project: %1").arg(detail)); }

} // namespace

ProjectWriteError::ProjectWriteError(const QString &message)
    : std::runtime_error(message.toStdString()), message_(message)
{
}

void ProjectWriter::save(const Document &document, const QString &projectDirectory)
{
    if (document.id.isNull() || document.canvasSize.width() < 1 || document.canvasSize.width() > 30000
        || document.canvasSize.height() < 1 || document.canvasSize.height() > 30000
        || document.layers.size() > 10000) fail(QStringLiteral("the document is outside supported limits"));

    const QFileInfo destinationInfo(projectDirectory);
    QDir parent = destinationInfo.absoluteDir();
    if (!parent.exists() && !parent.mkpath(QStringLiteral("."))) fail(QStringLiteral("the destination folder cannot be created"));
    QTemporaryDir staging(parent.filePath(QStringLiteral(".compositor-save-XXXXXX")));
    if (!staging.isValid()) fail(QStringLiteral("a temporary package cannot be created"));
    QDir root(staging.path());
    if (!root.mkdir(QStringLiteral("images"))) fail(QStringLiteral("the image folder cannot be created"));

    QJsonArray layers;
    qint64 totalPixels = 0;
    for (const Layer &layer : document.layers) {
        if (layer.id.isNull() || layer.name.trimmed().isEmpty() || !layer.transform.isValid()) fail(QStringLiteral("a layer is invalid"));
        if (!layer.adjustment.isEmpty()) {
            static const QSet<QString> kinds{QStringLiteral("Hue/Saturation"), QStringLiteral("Levels"), QStringLiteral("Curves"),
                QStringLiteral("Exposure"), QStringLiteral("Gradient Map"), QStringLiteral("Grain")};
            if (layer.group || !layer.image.isNull() || !kinds.contains(layer.adjustment.value(QStringLiteral("kind")).toString()))
                fail(QStringLiteral("an adjustment layer is invalid"));
        }
        QJsonObject value{{QStringLiteral("id"), uuid(layer.id)}, {QStringLiteral("name"), layer.name},
            {QStringLiteral("isVisible"), layer.visible}, {QStringLiteral("transform"), transformObject(layer.transform)},
            {QStringLiteral("isGroup"), layer.group}, {QStringLiteral("opacity"), layer.opacity},
            {QStringLiteral("blendMode"), blendName(layer.blendMode)}};
        if (layer.parentId) value.insert(QStringLiteral("parentID"), uuid(*layer.parentId));
        if (!layer.adjustment.isEmpty()) value.insert(QStringLiteral("adjustment"), layer.adjustment);
        if (!layer.shape.isEmpty()) value.insert(QStringLiteral("shape"), layer.shape);
        for (const bool mask : {false, true}) {
            const QImage &image = mask ? layer.mask : layer.image;
            if (image.isNull()) continue;
            totalPixels += qint64(image.width()) * image.height();
            if (totalPixels > 100000000LL) fail(QStringLiteral("the image pixel limit is exceeded"));
            const QString fileName = uuid(layer.id) + (mask ? QStringLiteral(".mask.png") : QStringLiteral(".png"));
            if (!image.save(root.filePath(QStringLiteral("images/") + fileName), "PNG")) fail(QStringLiteral("a layer image could not be encoded"));
            value.insert(mask ? QStringLiteral("maskFile") : QStringLiteral("imageFile"), fileName);
            if (mask) { value.insert(QStringLiteral("maskEnabled"), layer.maskEnabled); value.insert(QStringLiteral("maskLinked"), layer.maskLinked); if (layer.maskPlacement) value.insert(QStringLiteral("maskPlacement"), transformObject(*layer.maskPlacement)); }
        }
        if (layer.maskSourceId) value.insert(QStringLiteral("maskSourceID"), uuid(*layer.maskSourceId));
        layers.append(value);
    }
    QJsonObject manifest{{QStringLiteral("format"), QStringLiteral("com.compositor.project")},
        {QStringLiteral("version"), 7}, {QStringLiteral("colorSpace"), QStringLiteral("sRGB")},
        {QStringLiteral("resolution"), document.resolution}, {QStringLiteral("documentID"), uuid(document.id)},
        {QStringLiteral("width"), document.canvasSize.width()}, {QStringLiteral("height"), document.canvasSize.height()},
        {QStringLiteral("activeLayerID"), document.activeLayerId ? QJsonValue(uuid(*document.activeLayerId)) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("layers"), layers}};
    const QByteArray metadata = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (metadata.size() > 4 * 1024 * 1024) fail(QStringLiteral("the manifest is too large"));
    QSaveFile manifestFile(root.filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(metadata) != metadata.size() || !manifestFile.commit())
        fail(QStringLiteral("the manifest could not be written"));

    const QString destination = destinationInfo.absoluteFilePath();
    const QString backup = destination + QStringLiteral(".previous-") + uuid(QUuid::createUuid());
    const bool existed = QFileInfo::exists(destination);
    if (existed && (!destinationInfo.isDir() || destinationInfo.isSymLink() || !parent.rename(destination, backup)))
        fail(QStringLiteral("the existing package cannot be replaced"));
    staging.setAutoRemove(false);
    if (!parent.rename(staging.path(), destination)) {
        if (existed) parent.rename(backup, destination);
        QDir(staging.path()).removeRecursively();
        fail(QStringLiteral("the completed package cannot be installed"));
    }
    if (existed) QDir(backup).removeRecursively();
}

} // namespace compositor
