#include "io/SvgImporter.h"

#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <cmath>
#include <zlib.h>

namespace compositor {

namespace {

QByteArray decompressGzip(const QByteArray &compressed, qint64 maxBytes = SvgImporter::MaxFileBytes)
{
    if (compressed.isEmpty()) return {};
    z_stream strm{};
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) return {};

    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.constData()));
    strm.avail_in = static_cast<uInt>(compressed.size());

    QByteArray out;
    char buffer[32768];

    int ret = Z_OK;
    while (ret == Z_OK) {
        strm.next_out = reinterpret_cast<Bytef *>(buffer);
        strm.avail_out = sizeof(buffer);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&strm);
            return {};
        }
        const int produced = sizeof(buffer) - static_cast<int>(strm.avail_out);
        if (produced > 0) {
            if (out.size() + produced > maxBytes) {
                inflateEnd(&strm);
                return {};
            }
            out.append(buffer, produced);
        }
    }
    inflateEnd(&strm);
    return ret == Z_STREAM_END ? out : QByteArray{};
}

} // namespace

QRectF SvgImporter::parseViewBox(QStringView str, bool *ok)
{
    if (ok) *ok = false;
    const QString trimmed = str.toString().trimmed();
    if (trimmed.isEmpty()) return {};

    static const QRegularExpression separator(QStringLiteral("[\\s,]+"));
    const QStringList tokens = trimmed.split(separator, Qt::SkipEmptyParts);
    if (tokens.size() != 4) return {};

    bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
    const double x = tokens[0].toDouble(&ok1);
    const double y = tokens[1].toDouble(&ok2);
    const double w = tokens[2].toDouble(&ok3);
    const double h = tokens[3].toDouble(&ok4);

    if (!ok1 || !ok2 || !ok3 || !ok4 || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(w) || !std::isfinite(h) || w <= 0.0 || h <= 0.0) {
        return {};
    }

    if (ok) *ok = true;
    return QRectF(x, y, w, h);
}

double SvgImporter::parseLength(QStringView str, double relativeTo, bool *ok)
{
    if (ok) *ok = false;
    const QString s = str.toString().trimmed();
    if (s.isEmpty()) return 0.0;

    if (s.endsWith(QLatin1Char('%'))) {
        bool numOk = false;
        const double pct = QStringView(s).chopped(1).trimmed().toDouble(&numOk);
        if (!numOk || !std::isfinite(pct) || pct < 0.0) return 0.0;
        if (ok) *ok = true;
        return (pct / 100.0) * relativeTo;
    }

    double multiplier = 1.0;
    QStringView numPart = s;
    if (s.endsWith(QLatin1String("px"), Qt::CaseInsensitive)) {
        numPart = QStringView(s).chopped(2).trimmed();
    } else if (s.endsWith(QLatin1String("pt"), Qt::CaseInsensitive)) {
        numPart = QStringView(s).chopped(2).trimmed();
        multiplier = 96.0 / 72.0;
    } else if (s.endsWith(QLatin1String("in"), Qt::CaseInsensitive)) {
        numPart = QStringView(s).chopped(2).trimmed();
        multiplier = 96.0;
    } else if (s.endsWith(QLatin1String("mm"), Qt::CaseInsensitive)) {
        numPart = QStringView(s).chopped(2).trimmed();
        multiplier = 96.0 / 25.4;
    } else if (s.endsWith(QLatin1String("cm"), Qt::CaseInsensitive)) {
        numPart = QStringView(s).chopped(2).trimmed();
        multiplier = 960.0 / 25.4;
    } else if (s.endsWith(QLatin1String("pc"), Qt::CaseInsensitive)) {
        numPart = QStringView(s).chopped(2).trimmed();
        multiplier = 16.0;
    }

    bool numOk = false;
    const double val = numPart.toDouble(&numOk);
    if (!numOk || !std::isfinite(val) || val < 0.0) return 0.0;
    if (ok) *ok = true;
    return val * multiplier;
}

bool SvgImporter::matchesData(const QByteArray &data, const QString &path)
{
    if (data.isEmpty() || data.size() > MaxFileBytes) return false;

    // Check GZip magic for SVGZ
    if (data.size() >= 2 && static_cast<uint8_t>(data[0]) == 0x1f && static_cast<uint8_t>(data[1]) == 0x8b) {
        if (!path.isEmpty()) {
            const QString suffix = QFileInfo(path).suffix().toLower();
            if (suffix == QStringLiteral("svgz") || suffix == QStringLiteral("svg")) return true;
        }
        // Try decompressing the header to inspect
        const QByteArray head = decompressGzip(data.left(4096), 65536);
        return head.contains("<svg");
    }

    // Inspect text header (skip BOM / whitespace)
    int offset = 0;
    if (data.startsWith("\xEF\xBB\xBF")) offset += 3;
    while (offset < data.size() && (data[offset] == ' ' || data[offset] == '\t' || data[offset] == '\r' || data[offset] == '\n')) {
        ++offset;
    }

    const QByteArray slice = data.mid(offset, 4096).toLower();
    if (slice.startsWith("<?xml") || slice.startsWith("<!doctype") || slice.startsWith("<svg")) {
        if (slice.contains("<svg")) return true;
    }

    if (!path.isEmpty()) {
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == QStringLiteral("svg") || suffix == QStringLiteral("svgz")) {
            return slice.contains("<svg");
        }
    }

    return false;
}

bool SvgImporter::matches(const QString &path)
{
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) return false;
    const qint64 sz = fi.size();
    if (sz <= 0 || sz > MaxFileBytes) return false;

    const QString suffix = fi.suffix().toLower();
    if (suffix == QStringLiteral("svg") || suffix == QStringLiteral("svgz")) {
        // Quick verification of header
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray header = file.read(4096);
            if (matchesData(header, path)) return true;
        }
    }

    // Secondary sniff via QImageReader
    const QByteArray fmt = QImageReader::imageFormat(path);
    return fmt == "svg" || fmt == "svgz";
}

bool SvgImporter::probeData(const QByteArray &data, SvgInfo *info, QString *error)
{
    if (data.isEmpty()) {
        if (error) *error = QStringLiteral("SVG data is empty");
        return false;
    }
    if (data.size() > MaxFileBytes) {
        if (error) *error = QStringLiteral("SVG data exceeds maximum file limit of %1 bytes").arg(MaxFileBytes);
        return false;
    }

    const bool isGzip = (data.size() >= 2 && static_cast<uint8_t>(data[0]) == 0x1f && static_cast<uint8_t>(data[1]) == 0x8b);
    QByteArray uncompressed;
    if (isGzip) {
        uncompressed = decompressGzip(data);
        if (uncompressed.isEmpty()) {
            if (error) *error = QStringLiteral("Failed to decompress SVGZ data");
            return false;
        }
    }
    const QByteArray &xmlData = isGzip ? uncompressed : data;

    QXmlStreamReader xml(xmlData);
    xml.setEntityResolver(nullptr);
    xml.setEntityExpansionLimit(1000);

    bool foundSvg = false;
    SvgInfo parsed;
    double explicitWidth = 0.0;
    double explicitHeight = 0.0;

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            if (xml.name().compare(QLatin1String("svg"), Qt::CaseInsensitive) == 0) {
                foundSvg = true;
                const auto attrs = xml.attributes();
                if (attrs.hasAttribute(QLatin1String("viewBox"))) {
                    bool ok = false;
                    const QRectF vb = parseViewBox(attrs.value(QLatin1String("viewBox")), &ok);
                    if (ok) {
                        parsed.viewBox = vb;
                        parsed.hasViewBox = true;
                    }
                }
                if (attrs.hasAttribute(QLatin1String("width"))) {
                    bool ok = false;
                    const double w = parseLength(attrs.value(QLatin1String("width")),
                                                 parsed.hasViewBox ? parsed.viewBox.width() : 0.0, &ok);
                    if (ok && w > 0.0) {
                        parsed.hasWidth = true;
                        explicitWidth = w;
                    }
                }
                if (attrs.hasAttribute(QLatin1String("height"))) {
                    bool ok = false;
                    const double h = parseLength(attrs.value(QLatin1String("height")),
                                                 parsed.hasViewBox ? parsed.viewBox.height() : 0.0, &ok);
                    if (ok && h > 0.0) {
                        parsed.hasHeight = true;
                        explicitHeight = h;
                    }
                }
                break;
            } else {
                if (error) *error = QStringLiteral("Root XML element is <%1>, expected <svg>").arg(xml.name().toString());
                return false;
            }
        }
    }

    if (!foundSvg) {
        if (error) *error = xml.hasError() ? xml.errorString() : QStringLiteral("No <svg> root element found");
        return false;
    }

    // Probe with QImageReader
    QBuffer buf;
    buf.setData(data);
    if (!buf.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Failed to open memory buffer for SVG");
        return false;
    }
    QImageReader reader(&buf, isGzip ? "svgz" : "svg");
    const QSize readerSize = reader.size();

    QSize intrinsic;
    if (readerSize.isValid() && readerSize.width() > 0 && readerSize.height() > 0) {
        intrinsic = readerSize;
    } else if (parsed.hasWidth && parsed.hasHeight && explicitWidth > 0.0 && explicitHeight > 0.0) {
        intrinsic = QSize(std::max(1, static_cast<int>(std::round(explicitWidth))),
                          std::max(1, static_cast<int>(std::round(explicitHeight))));
    } else if (parsed.hasViewBox && parsed.viewBox.width() > 0 && parsed.viewBox.height() > 0) {
        intrinsic = QSize(std::max(1, static_cast<int>(std::round(parsed.viewBox.width()))),
                          std::max(1, static_cast<int>(std::round(parsed.viewBox.height()))));
    } else {
        // Fallback standard default size
        intrinsic = QSize(100, 100);
    }

    if (intrinsic.width() <= 0 || intrinsic.height() <= 0) {
        if (error) *error = QStringLiteral("SVG has invalid non-positive intrinsic dimensions: %1x%2")
                                .arg(intrinsic.width()).arg(intrinsic.height());
        return false;
    }

    parsed.intrinsicSize = intrinsic;
    if (info) *info = parsed;
    return true;
}

bool SvgImporter::probe(const QString &path, SvgInfo *info, QString *error)
{
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        if (error) *error = QStringLiteral("File not found: %1").arg(path);
        return false;
    }
    const qint64 sz = fi.size();
    if (sz == 0) {
        if (error) *error = QStringLiteral("SVG file is empty: %1").arg(path);
        return false;
    }
    if (sz > MaxFileBytes) {
        if (error) *error = QStringLiteral("SVG file exceeds maximum allowed size (%1 bytes): %2")
                                .arg(MaxFileBytes).arg(path);
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Failed to open file: %1").arg(path);
        return false;
    }
    const QByteArray data = file.read(MaxFileBytes + 1);
    file.close();

    if (data.size() > MaxFileBytes) {
        if (error) *error = QStringLiteral("SVG file exceeds maximum allowed size (%1 bytes): %2")
                                .arg(MaxFileBytes).arg(path);
        return false;
    }

    return probeData(data, info, error);
}

QSize SvgImporter::computeTargetSize(const QSize &intrinsicSize, const std::optional<QSize> &fitting)
{
    if (intrinsicSize.width() <= 0 || intrinsicSize.height() <= 0) return {};
    if (!fitting.has_value() || fitting->width() <= 0 || fitting->height() <= 0) {
        return intrinsicSize;
    }
    const double scale = std::min(double(fitting->width()) / double(intrinsicSize.width()),
                                  double(fitting->height()) / double(intrinsicSize.height()));
    const int w = std::max(1, static_cast<int>(std::round(intrinsicSize.width() * scale)));
    const int h = std::max(1, static_cast<int>(std::round(intrinsicSize.height() * scale)));
    return QSize(w, h);
}

QImage SvgImporter::readFromData(const QByteArray &data,
                                 const std::optional<QSize> &fitting,
                                 qint64 remainingPixels,
                                 QString *error)
{
    SvgInfo info;
    if (!probeData(data, &info, error)) {
        return {};
    }

    const QSize targetSize = computeTargetSize(info.intrinsicSize, fitting);
    if (targetSize.width() <= 0 || targetSize.height() <= 0) {
        if (error) *error = QStringLiteral("Computed target SVG dimensions are invalid: %1x%2")
                                .arg(targetSize.width()).arg(targetSize.height());
        return {};
    }

    if (targetSize.width() > MaxDimension || targetSize.height() > MaxDimension) {
        if (error) *error = QStringLiteral("SVG target dimensions (%1x%2) exceed maximum allowed side of %3 pixels")
                                .arg(targetSize.width()).arg(targetSize.height()).arg(MaxDimension);
        return {};
    }

    const qint64 surfacePixels = qint64(targetSize.width()) * targetSize.height();
    if (surfacePixels > remainingPixels) {
        if (error) *error = QStringLiteral("SVG target dimensions (%1x%2 = %3 pixels) exceed pixel budget of %4 pixels")
                                .arg(targetSize.width()).arg(targetSize.height()).arg(surfacePixels).arg(remainingPixels);
        return {};
    }

    const bool isGzip = (data.size() >= 2 && static_cast<uint8_t>(data[0]) == 0x1f && static_cast<uint8_t>(data[1]) == 0x8b);
    QBuffer buf;
    buf.setData(data);
    if (!buf.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Failed to open buffer for SVG reading");
        return {};
    }

    QImageReader reader(&buf, isGzip ? "svgz" : "svg");
    reader.setAutoTransform(true);
    reader.setScaledSize(targetSize);

    QImage image = reader.read();
    if (image.isNull()) {
        if (error) *error = reader.errorString().isEmpty() ? QStringLiteral("Failed to rasterize SVG image") : reader.errorString();
        return {};
    }

    if (image.width() > MaxDimension || image.height() > MaxDimension ||
        qint64(image.width()) * image.height() > remainingPixels) {
        if (error) *error = QStringLiteral("Rasterized SVG exceeds document limits");
        return {};
    }

    // Convert/assign sRGB color space
    if (image.colorSpace().isValid()) {
        image.convertToColorSpace(QColorSpace::SRgb);
    } else {
        image.setColorSpace(QColorSpace::SRgb);
    }

    // Ensure RGBA8888 premultiplied layer format
    if (image.format() != QImage::Format_RGBA8888_Premultiplied) {
        image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    }

    return image;
}

QImage SvgImporter::read(const QString &path,
                         const std::optional<QSize> &fitting,
                         qint64 remainingPixels,
                         QString *error)
{
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) {
        if (error) *error = QStringLiteral("File not found: %1").arg(path);
        return {};
    }
    const qint64 sz = fi.size();
    if (sz == 0) {
        if (error) *error = QStringLiteral("SVG file is empty: %1").arg(path);
        return {};
    }
    if (sz > MaxFileBytes) {
        if (error) *error = QStringLiteral("SVG file exceeds maximum allowed size (%1 bytes): %2")
                                .arg(MaxFileBytes).arg(path);
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Failed to open file: %1").arg(path);
        return {};
    }
    const QByteArray data = file.read(MaxFileBytes + 1);
    file.close();

    if (data.size() > MaxFileBytes) {
        if (error) *error = QStringLiteral("SVG file exceeds maximum allowed size (%1 bytes): %2")
                                .arg(MaxFileBytes).arg(path);
        return {};
    }

    return readFromData(data, fitting, remainingPixels, error);
}

} // namespace compositor
