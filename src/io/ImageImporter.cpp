#include "io/ImageImporter.h"
#include "io/SvgImporter.h"
#include "core/DocumentLimits.h"

#include <QColorSpace>
#include <QFileInfo>
#include <QFile>
#include <QImageReader>
#include <algorithm>

#ifdef COMPOSITOR_HAVE_LIBHEIF
#include <libheif/heif.h>
#endif

namespace compositor {

QImage ImageImporter::read(const QString &path, QString *error)
{
    if (SvgImporter::matches(path)) {
        return SvgImporter::read(path, std::nullopt, DocumentLimits::documentPixelBudget(), error);
    }

    QImageReader reader(path); reader.setAutoTransform(true);
    reader.setAllocationLimit(int(std::min<qint64>(2047, DocumentLimits::documentPixelBudget() * 4 / (1024 * 1024)) + 1));
    // The Mac importer takes JPEG, PNG, HEIC and TIFF (plus PSD, SVG and RAW through their own paths); Qt would also
    // decode GIF, BMP, WebP and more, which the Mac refuses.
    const QByteArray format = reader.format();
    if (!format.isEmpty() && !QList<QByteArray>{"png", "jpeg", "jpg", "tiff", "tif", "heic", "heif"}.contains(format)) {
        if (error) *error = QStringLiteral("Choose a JPEG, PNG, HEIC, TIFF, or Photoshop (PSD) file.");
        return {};
    }
    QImage image = reader.read();
#ifdef COMPOSITOR_HAVE_LIBHEIF
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (image.isNull() && (suffix == QStringLiteral("heic") || suffix == QStringLiteral("heif"))) {
        heif_context *context = heif_context_alloc();
        heif_image_handle *handle = nullptr; heif_image *decoded = nullptr;
        heif_error status = heif_context_read_from_file(context, QFile::encodeName(path).constData(), nullptr);
        if (status.code == heif_error_Ok) status = heif_context_get_primary_image_handle(context, &handle);
        if (status.code == heif_error_Ok) status = heif_decode_image(handle, &decoded, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
        if (status.code == heif_error_Ok) {
            int stride = 0; const uint8_t *pixels = heif_image_get_plane_readonly(decoded, heif_channel_interleaved, &stride);
            const int width = heif_image_handle_get_width(handle), height = heif_image_handle_get_height(handle);
            if (pixels && width > 0 && height > 0) image = QImage(pixels, width, height, stride, QImage::Format_RGBA8888).copy();
        }
        if (decoded) heif_image_release(decoded);
        if (handle) heif_image_handle_release(handle);
        heif_context_free(context);
        if (image.isNull() && error) *error = QString::fromUtf8(status.message ? status.message : "HEIC decoding failed");
    }
#endif
    if (image.isNull()) { if (error && error->isEmpty()) *error = reader.errorString(); return {}; }
    if (image.width() > 30000 || image.height() > 30000 || qint64(image.width()) * image.height() > DocumentLimits::documentPixelBudget()) {
        if (error) *error = QStringLiteral("unsupported dimensions");
        return {};
    }
    if (image.colorSpace().isValid()) image.convertToColorSpace(QColorSpace::SRgb); else image.setColorSpace(QColorSpace::SRgb);
    return image;
}

} // namespace compositor
