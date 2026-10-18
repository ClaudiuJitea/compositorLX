#include "io/HdrImage.h"
#include "core/DocumentLimits.h"
#include "DeepPixels.h"
#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QRegularExpression>
#include <cmath>
#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfHeader.h>
#include <stdexcept>
#include <vector>

namespace compositor::HdrImage
{
namespace
{
class Input final : public Imf::IStream
{
    QFile file_;

  public:
    explicit Input(const QString &path) : Imf::IStream("image.exr"), file_(path)
    {
        if (!file_.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read EXR");
    }
    bool read(char data[], int n) override
    {
        if (file_.read(data, n) != n)
            throw std::runtime_error("Truncated EXR");
        return !file_.atEnd();
    }
    uint64_t tellg() override
    {
        return uint64_t(file_.pos());
    }
    void seekg(uint64_t pos) override
    {
        if (!file_.seek(qint64(pos)))
            throw std::runtime_error("Invalid EXR offset");
    }
};
class Output final : public Imf::OStream
{
  public:
    QByteArray bytes;
    QBuffer buffer{&bytes};
    Output() : Imf::OStream("image.exr")
    {
        buffer.open(QIODevice::ReadWrite);
    }
    void write(const char data[], int n) override
    {
        if (buffer.write(data, n) != n)
            throw std::runtime_error("Cannot encode EXR");
    }
    uint64_t tellp() override
    {
        return uint64_t(buffer.pos());
    }
    void seekp(uint64_t pos) override
    {
        if (!buffer.seek(qint64(pos)))
            throw std::runtime_error("Invalid EXR offset");
    }
};
} // namespace
QImage read(const QString &path, QString *error)
{
    try {
        Input stream(path);
        Imf::RgbaInputFile file(stream);
        const auto box = file.dataWindow();
        const qint64 width = qint64(box.max.x) - box.min.x + 1, height = qint64(box.max.y) - box.min.y + 1;
        if (width < 1 || height < 1 || width > DocumentLimits::maxSide || height > DocumentLimits::maxSide ||
            width * height > DocumentLimits::documentPixelBudget())
            throw std::runtime_error("Unsupported EXR dimensions");
        // A scanline buffer with room for the data window's x offset avoids constructing an out-of-array
        // pointer.
        const qint64 left = std::max<qint64>(0, box.min.x), right = std::max<qint64>(0, -box.min.x);
        if (left + right + width > 1000000)
            throw std::runtime_error("Unsupported EXR data window");
        std::vector<Imf::Rgba> row(size_t(left + right + width));
        QImage image(int(width), int(height), QImage::Format_RGBA16FPx4_Premultiplied);
        if (image.isNull())
            throw std::bad_alloc();
        for (int y = 0; y < int(height); ++y) {
            file.setFrameBuffer(row.data() + right, 1, 0);
            file.readPixels(box.min.y + y);
            auto *out = reinterpret_cast<qfloat16 *>(image.scanLine(y));
            for (int x = 0; x < int(width); ++x) {
                const auto &p = row[size_t(left + x)];
                out[x * 4] = qfloat16(float(p.r));
                out[x * 4 + 1] = qfloat16(float(p.g));
                out[x * 4 + 2] = qfloat16(float(p.b));
                out[x * 4 + 3] = qfloat16(float(p.a));
            }
        }
        // EXR uses linear light; the editor keeps extended sRGB half floats.
        image.setColorSpace(QColorSpace::SRgbLinear);
        image.convertToColorSpace(QColorSpace::SRgb);
        if (deep_half_peak(reinterpret_cast<const uint16_t *>(image.constBits()), image.bytesPerLine(),
                           image.width(), image.height()) <= 1)
            return image.convertToFormat(QImage::Format_RGBA64_Premultiplied);
        return image;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return {};
    }
}
QImage readRadiance(const QString &path, QString *error)
{
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return QImage();
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    const QByteArray magic = file.readLine();
    if (!magic.startsWith("#?RADIANCE") && !magic.startsWith("#?RGBE"))
        return fail(QStringLiteral("Invalid Radiance HDR header."));
    bool format = false;
    int headerBytes = magic.size();
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        headerBytes += line.size();
        if (headerBytes > 65536)
            return fail(QStringLiteral("HDR header is too large."));
        if (line.trimmed().isEmpty())
            break;
        if (line.trimmed() == "FORMAT=32-bit_rle_rgbe")
            format = true;
    }
    if (!format)
        return fail(QStringLiteral("Unsupported Radiance HDR encoding."));
    const auto match = QRegularExpression(QStringLiteral("^([+-])Y (\\d+) ([+-])X (\\d+)$"))
                           .match(QString::fromLatin1(file.readLine().trimmed()));
    if (!match.hasMatch())
        return fail(QStringLiteral("Unsupported Radiance HDR orientation."));
    const int height = match.captured(2).toInt(), width = match.captured(4).toInt();
    if (width < 1 || height < 1 || width > DocumentLimits::maxSide || height > DocumentLimits::maxSide ||
        qint64(width) * height > DocumentLimits::documentPixelBudget())
        return fail(QStringLiteral("Unsupported HDR dimensions."));
    QImage image(width, height, QImage::Format_RGBA32FPx4_Premultiplied);
    if (image.isNull())
        return {};
    QByteArray row(width * 4, Qt::Uninitialized);
    for (int y = 0; y < height; ++y) {
        const QByteArray first = file.read(4);
        if (first.size() != 4)
            return fail(QStringLiteral("Truncated HDR scanline."));
        const bool rle = width >= 8 && uchar(first[0]) == 2 && uchar(first[1]) == 2 &&
                         ((uchar(first[2]) << 8) | uchar(first[3])) == width;
        if (rle) {
            for (int c = 0; c < 4; ++c) {
                int x = 0;
                while (x < width) {
                    char code = 0;
                    if (!file.getChar(&code) || uchar(code) == 0)
                        return fail(QStringLiteral("Invalid HDR run."));
                    const int count = uchar(code) > 128 ? uchar(code) - 128 : uchar(code);
                    if (x + count > width)
                        return fail(QStringLiteral("HDR run exceeds scanline."));
                    if (uchar(code) > 128) {
                        char value = 0;
                        if (!file.getChar(&value))
                            return fail(QStringLiteral("Truncated HDR run."));
                        for (int i = 0; i < count; ++i)
                            row[(x + i) * 4 + c] = value;
                    } else {
                        const QByteArray values = file.read(count);
                        if (values.size() != count)
                            return fail(QStringLiteral("Truncated HDR scanline."));
                        for (int i = 0; i < count; ++i)
                            row[(x + i) * 4 + c] = values[i];
                    }
                    x += count;
                }
            }
        } else {
            const QByteArray rest = file.read(width * 4 - 4);
            if (rest.size() != width * 4 - 4)
                return fail(QStringLiteral("Truncated HDR pixels."));
            row = first + rest;
        }
        const int yy = match.captured(1) == QStringLiteral("-") ? y : height - 1 - y;
        auto *out = reinterpret_cast<float *>(image.scanLine(yy));
        for (int x = 0; x < width; ++x) {
            const int xx = match.captured(3) == QStringLiteral("+") ? x : width - 1 - x;
            const int e = uchar(row[x * 4 + 3]);
            for (int c = 0; c < 3; ++c)
                out[xx * 4 + c] = e ? std::ldexp(float(uchar(row[x * 4 + c])), e - 136) : 0;
            out[xx * 4 + 3] = 1;
        }
    }
    image.setColorSpace(QColorSpace::SRgbLinear);
    image.convertToColorSpace(QColorSpace::SRgb);
    return image.convertToFormat(QImage::Format_RGBA16FPx4_Premultiplied);
}
std::optional<QByteArray> encode(const QImage &source, QString *error)
{
    if (source.isNull())
        return std::nullopt;
    try {
        QImage image = source.convertToFormat(QImage::Format_RGBA16FPx4_Premultiplied);
        image.setColorSpace(QColorSpace::SRgb);
        image.convertToColorSpace(QColorSpace::SRgbLinear);
        std::vector<Imf::Rgba> pixels(size_t(image.width()) * image.height());
        for (int y = 0; y < image.height(); ++y) {
            const auto *row = reinterpret_cast<const qfloat16 *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                auto &p = pixels[size_t(y) * image.width() + x];
                p.r = float(row[x * 4]);
                p.g = float(row[x * 4 + 1]);
                p.b = float(row[x * 4 + 2]);
                p.a = float(row[x * 4 + 3]);
            }
        }
        Output stream;
        {
            Imf::RgbaOutputFile file(stream, Imf::Header(image.width(), image.height()), Imf::WRITE_RGBA);
            file.setFrameBuffer(pixels.data(), 1, image.width());
            file.writePixels(image.height());
        }
        return stream.bytes;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return std::nullopt;
    }
}
} // namespace compositor::HdrImage
