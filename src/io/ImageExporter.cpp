#include "io/ImageExporter.h"
#include <QFileInfo>
#include <QSet>
#include <QHash>
#include "core/ImageTrim.h"
#include "rendering/LayerRenderer.h"
#include "rendering/DeepImage.h"

#include <QBuffer>
#include <QColorSpace>
#include <QImageWriter>
#include <QPainter>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

namespace compositor {

std::optional<QByteArray> ImageExporter::png(const QImage &source, double resolution, QString *error)
{
    if (source.isNull() || !std::isfinite(resolution) || resolution < 1 || resolution > 9600) {
        if (error) *error = QStringLiteral("Invalid PNG export options.");
        return std::nullopt;
    }
    QImage image = source.convertToFormat(DeepImage::isDeep(source)?QImage::Format_RGBA64:QImage::Format_RGBA8888);
    image.setColorSpace(QColorSpace::SRgb);
    const int dotsPerMeter = qRound(resolution / .0254);
    image.setDotsPerMeterX(dotsPerMeter);
    image.setDotsPerMeterY(dotsPerMeter);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        if (error) *error = buffer.errorString();
        return std::nullopt;
    }
    QImageWriter writer(&buffer, "PNG");
    writer.setText(QStringLiteral("Software"), QStringLiteral("CompositorLX"));
    if (!writer.write(image)) {
        if (error) *error = writer.errorString();
        return std::nullopt;
    }
    return bytes;
}

bool ImageExporter::writeAtomically(const QByteArray &data, const QString &path, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

std::optional<JpegResult> ImageExporter::jpeg(const QImage &source, int quality, const QColor &matte,
                                               double resolution, QString *error)
{
    if (source.isNull() || !matte.isValid() || !std::isfinite(resolution) || resolution < 1 || resolution > 9600) {
        if (error) *error = QStringLiteral("Invalid JPEG export options.");
        return std::nullopt;
    }
    QImage opaque(source.size(), QImage::Format_RGB32);
    opaque.fill(matte.toRgb());
    QPainter painter(&opaque);
    painter.drawImage(0, 0, source);
    painter.end();
    opaque.setColorSpace(QColorSpace::SRgb);
    const int dotsPerMeter = qRound(resolution / .0254);
    opaque.setDotsPerMeterX(dotsPerMeter);
    opaque.setDotsPerMeterY(dotsPerMeter);

    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)) {
        if (error) *error = buffer.errorString();
        return std::nullopt;
    }
    QImageWriter writer(&buffer, "JPEG");
    writer.setQuality(std::clamp(quality, 0, 100));
    writer.setText(QStringLiteral("Software"), QStringLiteral("CompositorLX"));
    if (!writer.write(opaque)) {
        if (error) *error = writer.errorString();
        return std::nullopt;
    }
    QImage preview = QImage::fromData(bytes, "JPEG");
    if (preview.isNull()) {
        if (error) *error = QStringLiteral("The encoded JPEG preview could not be decoded.");
        return std::nullopt;
    }
    // Full size, so the dialog's 100% view shows the real artifacts; capped to keep memory in bounds (mac ac6f309).
    if (preview.width() > 8192 || preview.height() > 8192)
        preview = preview.scaled(8192, 8192, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return JpegResult{std::move(bytes), std::move(preview)};
}


Document ImageExporter::isolating(const Document &document, const QUuid &id)
{
    Document result = document;
    QHash<QUuid, std::optional<QUuid>> parents;
    for (const Layer &layer : document.layers) parents.insert(layer.id, layer.parentId);
    QSet<QUuid> folders;
    for (std::optional<QUuid> parent = parents.value(id); parent && !folders.contains(*parent); parent = parents.value(*parent)) folders.insert(*parent);
    const auto isInside = [&](const QUuid &layer) {
        QSet<QUuid> seen;
        for (std::optional<QUuid> parent = parents.value(layer); parent && !seen.contains(*parent); parent = parents.value(*parent)) {
            if (*parent == id) return true;
            seen.insert(*parent);
        }
        return false;
    };
    QSet<QUuid> kept;
    for (const Layer &layer : document.layers) if (layer.id == id || isInside(layer.id)) kept.insert(layer.id);
    for (Layer &layer : result.layers) {
        if (layer.id == id || folders.contains(layer.id)) layer.visible = true;
        else if (!kept.contains(layer.id) && !(layer.maskSourceId && kept.contains(*layer.maskSourceId))) layer.visible = false;
    }
    result.selection.reset();
    return result;
}

std::optional<QByteArray> ImageExporter::layerPng(const Document &document, const QUuid &id, QString *error)
{
    const QImage raster = LayerRenderer::flattened(isolating(document, id));
    const std::optional<QImage> trimmed = ImageTrim::trimImage(raster, TrimOptions());
    if (!trimmed || trimmed->isNull()) return std::nullopt;
    return png(*trimmed, document.resolution, error);
}

QString ImageExporter::layerFileName(const QString &name)
{
    QString cleaned = QString(name).replace(QLatin1Char('/'), QLatin1Char('-')).replace(QLatin1Char(':'), QLatin1Char('-')).trimmed();
    return cleaned.isEmpty() || cleaned.startsWith(QLatin1Char('.')) ? QStringLiteral("Layer") + cleaned : cleaned;
}

QString ImageExporter::startingFormat(const QString &sourceImage, const QString &projectPath, const QString &last)
{
    if (!projectPath.isEmpty() || sourceImage.isEmpty()) return last;
    const QString suffix = QFileInfo(sourceImage).suffix().toLower();
    if (suffix == QStringLiteral("png")) return QStringLiteral("png");
    if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg")) return QStringLiteral("jpg");
    if (suffix == QStringLiteral("tif") || suffix == QStringLiteral("tiff")) return QStringLiteral("tiff");
    if (suffix == QStringLiteral("gif")) return QStringLiteral("gif");
    if (suffix == QStringLiteral("exr")) return QStringLiteral("exr");
    if (suffix == QStringLiteral("psd")) return QStringLiteral("psd");
    return last;
}

QString ImageExporter::exportName(const QString &projectPath, const QString &sourceImage, const QString &extension)
{
    const QString from = !projectPath.isEmpty() ? projectPath : sourceImage;
    const QString base = from.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(from).completeBaseName();
    return base + QLatin1Char('.') + extension;
}

QString ImageExporter::exportFolder(const QString &projectPath, const QString &sourceImage)
{
    if (!projectPath.isEmpty()) return QFileInfo(projectPath).absolutePath();
    if (!sourceImage.isEmpty()) return QFileInfo(sourceImage).absolutePath();
    return {};
}

} // namespace compositor
