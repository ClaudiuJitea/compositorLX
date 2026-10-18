#include "io/PrintExporter.h"
#include "rendering/DeepImage.h"
#include <QBuffer>
#include <QColorSpace>
#include <QPainter>
#include <QTemporaryFile>
#include <jpeglib.h>
#include <tiffio.h>
#include <setjmp.h>
#include <cmath>
#include <cstdlib>

namespace compositor::PrintExporter
{
namespace
{
bool valid(const QImage &i, double dpi)
{
    return !i.isNull() && std::isfinite(dpi) && dpi >= 1 && dpi <= 9600;
}
QByteArray compressed(const QByteArray &data)
{
    return qCompress(data, 6).sliced(4);
}
struct JpegError {
    jpeg_error_mgr manager;
    jmp_buf jump;
};
void jpegError(j_common_ptr c)
{
    auto *error = reinterpret_cast<JpegError *>(c->err);
    longjmp(error->jump, 1);
}
} // namespace
std::optional<QByteArray> pdf(const QImage &image, double resolution,
                              const std::optional<CMYKProfile> &profile, QString *error)
{
    if (!valid(image, resolution)) {
        if (error)
            *error = QStringLiteral("Invalid PDF export options.");
        return std::nullopt;
    }
    QImage opaque(image.size(), QImage::Format_RGB888);
    opaque.fill(Qt::white);
    {
        QPainter p(&opaque);
        p.drawImage(0, 0, image);
    }
    QByteArray pixels;
    if (profile)
        pixels = CMYK::inks(opaque, *profile);
    else {
        pixels.reserve(image.width() * image.height() * 3);
        for (int y = 0; y < image.height(); ++y)
            pixels.append(reinterpret_cast<const char *>(opaque.constScanLine(y)), image.width() * 3);
    }
    if (pixels.isEmpty())
        return std::nullopt;
    const QByteArray width = QByteArray::number(image.width() * 72.0 / resolution, 'f', 6),
                     height = QByteArray::number(image.height() * 72.0 / resolution, 'f', 6);
    const auto stream = [](const QByteArray &data, const QByteArray &attrs = QByteArray()) {
        return "<< /Length " + QByteArray::number(data.size()) + " " + attrs + " >>\nstream\n" + data +
               "\nendstream";
    };
    QList<QByteArray> objects{
        "<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + width + " " + height +
            "] /Resources << /XObject << /Image 5 0 R >> >> /Contents 4 0 R >>",
        stream("q " + width + " 0 0 " + height + " 0 0 cm /Image Do Q"),
        stream(compressed(pixels),
               "/Type /XObject /Subtype /Image /Width " + QByteArray::number(image.width()) + " /Height " +
                   QByteArray::number(image.height()) + " /BitsPerComponent 8 /ColorSpace " +
                   (profile ? QByteArray("[/ICCBased 6 0 R]") : QByteArray("/DeviceRGB")) +
                   " /Filter /FlateDecode")};
    if (profile)
        objects.append(stream(compressed(profile->data), "/N 4 /Alternate /DeviceCMYK /Filter /FlateDecode"));
    QByteArray out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    QList<qint64> offsets{0};
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(out.size());
        out += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const qint64 xref = out.size();
    out += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (int i = 1; i < offsets.size(); ++i)
        out += QByteArray::number(offsets[i]).rightJustified(10, '0') + " 00000 n \n";
    out += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
           QByteArray::number(xref) + "\n%%EOF\n";
    return out;
}
std::optional<QByteArray> tiff(const QImage &image, double resolution,
                               const std::optional<CMYKProfile> &profile, QString *error)
{
    if (!valid(image, resolution))
        return std::nullopt;
    QTemporaryFile temp;
    if (!temp.open())
        return std::nullopt;
    TIFF *file = TIFFOpen(temp.fileName().toUtf8().constData(), "w");
    if (!file)
        return std::nullopt;
    const int bits = !profile && DeepImage::isDeep(image) ? 16 : 8;
    const QImage rgba = image.convertToFormat(bits == 16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    const QByteArray inks = profile ? CMYK::inks(image, *profile) : QByteArray();
    const auto icc = profile ? profile->data : QColorSpace(QColorSpace::SRgb).iccProfile();
    TIFFSetField(file, TIFFTAG_IMAGEWIDTH, image.width());
    TIFFSetField(file, TIFFTAG_IMAGELENGTH, image.height());
    TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, bits);
    TIFFSetField(file, TIFFTAG_PHOTOMETRIC, profile ? PHOTOMETRIC_SEPARATED : PHOTOMETRIC_RGB);
    TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(file, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(file, TIFFTAG_XRESOLUTION, float(resolution));
    TIFFSetField(file, TIFFTAG_YRESOLUTION, float(resolution));
    TIFFSetField(file, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
    TIFFSetField(file, TIFFTAG_ICCPROFILE, uint32_t(icc.size()), icc.constData());
    if (profile)
        TIFFSetField(file, TIFFTAG_INKSET, INKSET_CMYK);
    else {
        uint16_t alpha = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(file, TIFFTAG_EXTRASAMPLES, 1, &alpha);
    }
    bool ok = !profile || !inks.isEmpty();
    for (int y = 0; ok && y < image.height(); ++y) {
        const void *row = profile
                              ? static_cast<const void *>(inks.constData() + qsizetype(y) * image.width() * 4)
                              : rgba.constScanLine(y);
        ok = TIFFWriteScanline(file, const_cast<void *>(row), y, 0) >= 0;
    }
    TIFFClose(file);
    if (!ok) {
        if (error)
            *error = QStringLiteral("Could not encode TIFF.");
        return std::nullopt;
    }
    temp.seek(0);
    return temp.readAll();
}
std::optional<QByteArray> jpeg(const QImage &image, double resolution, int quality,
                               const CMYKProfile &profile, QString *error)
{
    if (!valid(image, resolution))
        return std::nullopt;
    QByteArray ink = CMYK::inks(image, profile);
    if (ink.isEmpty())
        return std::nullopt;
    // Adobe CMYK JPEG stores inverted inks and announces the inversion in its APP14 marker.
    for (char &c : ink)
        c = char(255 - uchar(c));
    jpeg_compress_struct c{};
    JpegError err{};
    c.err = jpeg_std_error(&err.manager);
    err.manager.error_exit = jpegError;
    unsigned char *data = nullptr;
    unsigned long size = 0;
    if (setjmp(err.jump)) {
        jpeg_destroy_compress(&c);
        std::free(data);
        if (error)
            *error = QStringLiteral("Could not encode CMYK JPEG.");
        return std::nullopt;
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &data, &size);
    c.image_width = image.width();
    c.image_height = image.height();
    c.input_components = 4;
    c.in_color_space = JCS_CMYK;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, std::clamp(quality, 0, 100), TRUE);
    c.write_Adobe_marker = TRUE;
    c.density_unit = 1;
    c.X_density = c.Y_density = UINT16(std::clamp(qRound(resolution), 1, 65535));
    jpeg_start_compress(&c, TRUE);
    constexpr int maxChunk = 65519;
    const int chunks = int((profile.data.size() + maxChunk - 1) / maxChunk);
    for (int i = 0; i < chunks; ++i) {
        QByteArray marker("ICC_PROFILE\0", 12);
        marker.append(char(i + 1));
        marker.append(char(chunks));
        marker += profile.data.mid(i * maxChunk, maxChunk);
        jpeg_write_marker(&c, JPEG_APP0 + 2, reinterpret_cast<const JOCTET *>(marker.constData()),
                          unsigned(marker.size()));
    }
    while (c.next_scanline < c.image_height) {
        JSAMPROW row =
            reinterpret_cast<JSAMPROW>(ink.data() + qsizetype(c.next_scanline) * image.width() * 4);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    QByteArray out(reinterpret_cast<const char *>(data), qsizetype(size));
    jpeg_destroy_compress(&c);
    std::free(data);
    return out;
}
} // namespace compositor::PrintExporter
