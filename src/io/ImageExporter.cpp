#include "io/ImageExporter.h"

#include <QBuffer>
#include <QColorSpace>
#include <QImageWriter>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace compositor {

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
    if (preview.width() > 1000 || preview.height() > 1000)
        preview = preview.scaled(1000, 1000, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return JpegResult{std::move(bytes), std::move(preview)};
}

} // namespace compositor
