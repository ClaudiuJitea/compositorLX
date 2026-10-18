#include "io/ImageImporter.h"
#include "io/HdrImage.h"
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

    if (QFileInfo(path).suffix().compare(QStringLiteral("exr"),Qt::CaseInsensitive)==0) return HdrImage::read(path,error);
    if (QFileInfo(path).suffix().compare(QStringLiteral("hdr"),Qt::CaseInsensitive)==0) return HdrImage::readRadiance(path,error);
    QImageReader reader(path); reader.setAutoTransform(true);
    reader.setAllocationLimit(int(std::min<qint64>(2047, DocumentLimits::documentPixelBudget() * 4 / (1024 * 1024)) + 1));
    // The Mac importer takes JPEG, PNG, HEIC and TIFF (plus PSD, SVG and RAW through their own paths); Qt would also
    // decode GIF, BMP and other formats outside the supported import list.
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool heifFile = suffix == QStringLiteral("heic") || suffix == QStringLiteral("heif") || suffix == QStringLiteral("hif");
    const QByteArray format = reader.format();
    if (!heifFile && !format.isEmpty() && !QList<QByteArray>{"png", "jpeg", "jpg", "tiff", "tif", "heic", "heif", "webp"}.contains(format)) {
        if (error) *error = QStringLiteral("Choose a JPEG, PNG, WebP, HEIF, TIFF, EXR, or Photoshop (PSD) file.");
        return {};
    }
    QImage image;
#ifdef COMPOSITOR_HAVE_LIBHEIF
    if (!heifFile) image = reader.read();
    if (image.isNull() && heifFile) {
        heif_context *context = heif_context_alloc();
        heif_image_handle *handle = nullptr; heif_image *decoded = nullptr;
        heif_error status = heif_context_read_from_file(context, QFile::encodeName(path).constData(), nullptr);
        if (status.code == heif_error_Ok) status = heif_context_get_primary_image_handle(context, &handle);
        if (status.code == heif_error_Ok) {
            const int width = heif_image_handle_get_width(handle), height = heif_image_handle_get_height(handle);
            if (width <= 0 || height <= 0 || width > 30000 || height > 30000
                || qint64(width)*height > DocumentLimits::documentPixelBudget()) {
                heif_image_handle_release(handle); heif_context_free(context);
                if (error) *error = QStringLiteral("unsupported dimensions");
                return {};
            }
            const bool deep = heif_image_handle_get_luma_bits_per_pixel(handle) > 8;
            status = heif_decode_image(handle, &decoded, heif_colorspace_RGB,
                deep ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RGBA, nullptr);
        }
        if (status.code == heif_error_Ok) {
            int stride = 0; const uint8_t *pixels = heif_image_get_plane_readonly(decoded, heif_channel_interleaved, &stride);
            const int width = heif_image_handle_get_width(handle), height = heif_image_handle_get_height(handle);
            const int bits = heif_image_get_bits_per_pixel_range(decoded,heif_channel_interleaved);
            if (pixels && width > 0 && height > 0) {
                if (bits <= 8) image = QImage(pixels,width,height,stride,QImage::Format_RGBA8888).copy();
                else if (bits <= 16) {
                    image = QImage(width,height,QImage::Format_RGBA64);
                    const quint32 max = (1u << bits)-1;
                    for (int y=0;y<height;++y) {
                        const auto *source = pixels+qsizetype(y)*stride;
                        auto *out = reinterpret_cast<QRgba64 *>(image.scanLine(y));
                        for (int x=0;x<width;++x) {
                            quint16 channels[4];
                            for(int c=0;c<4;++c) {
                                const quint32 value = source[x*8+c*2] | (quint32(source[x*8+c*2+1])<<8);
                                channels[c] = quint16(std::min(65535u,(value*65535+max/2)/max));
                            }
                            out[x] = QRgba64::fromRgba64(channels[0],channels[1],channels[2],channels[3]);
                        }
                    }
                }
                const size_t profileSize = heif_image_handle_get_raw_color_profile_size(handle);
                if (profileSize > 0 && profileSize <= 16*1024*1024) {
                    QByteArray profile(qsizetype(profileSize),Qt::Uninitialized);
                    if (heif_image_handle_get_raw_color_profile(handle,profile.data()).code == heif_error_Ok)
                        image.setColorSpace(QColorSpace::fromIccProfile(profile));
                }
                if (!image.colorSpace().isValid()) {
                    heif_color_profile_nclx *nclx = nullptr;
                    if (heif_image_handle_get_nclx_color_profile(handle,&nclx).code == heif_error_Ok && nclx) {
                        auto transfer = QColorSpace::TransferFunction::SRgb;
                        float gamma = 0;
                        const int tc = int(nclx->transfer_characteristics);
                        if (tc == 8) transfer = QColorSpace::TransferFunction::Linear;
                        else if (tc == 4 || tc == 5) { transfer = QColorSpace::TransferFunction::Gamma; gamma = tc == 4 ? 2.2f : 2.8f; }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
                        else if (tc == 1 || tc == 6 || tc == 14 || tc == 15) transfer = QColorSpace::TransferFunction::Bt2020;
                        else if (tc == 16) transfer = QColorSpace::TransferFunction::St2084;
                        else if (tc == 18) transfer = QColorSpace::TransferFunction::Hlg;
                        if (tc == 16 || tc == 18) image = image.convertToFormat(QImage::Format_RGBA32FPx4);
#endif
                        const QColorSpace space({nclx->color_primary_white_x,nclx->color_primary_white_y},
                            {nclx->color_primary_red_x,nclx->color_primary_red_y},
                            {nclx->color_primary_green_x,nclx->color_primary_green_y},
                            {nclx->color_primary_blue_x,nclx->color_primary_blue_y},transfer,gamma);
                        if (space.isValid()) image.setColorSpace(space);
                        heif_nclx_color_profile_free(nclx);
                    }
                }
            }
        }
        if (decoded) heif_image_release(decoded);
        if (handle) heif_image_handle_release(handle);
        heif_context_free(context);
        if (image.isNull() && error) *error = QString::fromUtf8(status.message ? status.message : "HEIC decoding failed");
    }
#else
    image = reader.read();
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
