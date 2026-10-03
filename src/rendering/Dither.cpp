#include "rendering/Dither.h"

#include "rendering/RasterOperations.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <set>

extern "C" {
#include "DitherPixels.h"
}

namespace {

double clampTo(double value, double low, double high, double fallback)
{
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, low, high);
}

DitherColor clampedColor(const DitherColor &color)
{
    return {clampTo(color.red, 0, 1, 0), clampTo(color.green, 0, 1, 0), clampTo(color.blue, 0, 1, 0)};
}

uint8_t byteOf(double value) { return uint8_t(std::lround(std::clamp(value, 0.0, 1.0) * 255)); }

struct Glyphs {
    std::vector<uint8_t> maps;
    std::vector<float> coverage;
    int width = 1, height = 1;
};

// Each distinct character drawn into a monospaced cell lineHeight tall, on a shared baseline, sorted by ink.
Glyphs buildGlyphs(const QString &characters, int lineHeight)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setBold(true);
    font.setPixelSize(std::max(1, int(std::lround(lineHeight / 1.2))));
    font.setStyleStrategy(QFont::PreferAntialias);
    const QFontMetrics metrics(font);
    const int height = lineHeight;
    const int width = std::max(1, metrics.horizontalAdvance(QLatin1Char('M')));
    const int baseline = int(std::lround((height - (metrics.ascent() + metrics.descent())) / 2.0 + metrics.descent()));
    const int baselineY = height - baseline;

    struct Drawn { std::vector<uint8_t> map; float coverage; };
    std::vector<Drawn> drawn;
    std::set<char32_t> seen;
    const QList<uint> codes = characters.toUcs4();
    for (const uint code : codes) {
        if (!seen.insert(code).second) continue;
        QImage cell(width, height, QImage::Format_Grayscale8);
        cell.fill(0);
        {
            QPainter painter(&cell);
            painter.setRenderHint(QPainter::TextAntialiasing);
            painter.setFont(font);
            painter.setPen(Qt::white);
            const QString text = QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1);
            const int x = int(std::lround((width - metrics.horizontalAdvance(text)) / 2.0));
            painter.drawText(x, baselineY, text);
        }
        Drawn entry;
        entry.map.resize(size_t(width) * height);
        qint64 sum = 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const uint8_t value = cell.constScanLine(y)[x];
                entry.map[size_t(y) * width + x] = value;
                sum += value;
            }
        entry.coverage = float(double(sum) / (255.0 * width * height));
        drawn.push_back(std::move(entry));
    }
    std::stable_sort(drawn.begin(), drawn.end(), [](const Drawn &a, const Drawn &b) { return a.coverage < b.coverage; });
    Glyphs glyphs;
    glyphs.width = width;
    glyphs.height = height;
    for (const Drawn &entry : drawn) {
        glyphs.maps.insert(glyphs.maps.end(), entry.map.begin(), entry.map.end());
        glyphs.coverage.push_back(entry.coverage);
    }
    return glyphs;
}

// Averages block x block squares of premultiplied pixels (edge blocks average what they cover).
QImage boxDownsample(const QImage &image, int block)
{
    const int width = (image.width() + block - 1) / block, height = (image.height() + block - 1) / block;
    QImage small(width, height, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < height; ++y) {
        uchar *out = small.scanLine(y);
        for (int x = 0; x < width; ++x) {
            quint32 sums[4]{};
            int count = 0;
            for (int yy = y * block; yy < std::min(image.height(), (y + 1) * block); ++yy) {
                const uchar *in = image.constScanLine(yy);
                for (int xx = x * block; xx < std::min(image.width(), (x + 1) * block); ++xx, ++count)
                    for (int c = 0; c < 4; ++c) sums[c] += in[xx * 4 + c];
            }
            for (int c = 0; c < 4; ++c) out[x * 4 + c] = uchar((sums[c] + count / 2) / count);
        }
    }
    return small;
}

QImage nearestUpscale(const QImage &small, int block, const QSize &size)
{
    QImage full(size, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        const quint32 *in = reinterpret_cast<const quint32 *>(small.constScanLine(y / block));
        quint32 *out = reinterpret_cast<quint32 *>(full.scanLine(y));
        for (int x = 0; x < size.width(); ++x) out[x] = in[x / block];
    }
    return full;
}

bool runKernel(QImage &image, const DitherSettings &s, const Glyphs &glyphs)
{
    const bool twoColors = s.colors == DitherColors::TwoColors;
    DitherParams params{};
    params.style = int(s.style);
    params.levels = int(s.levels);
    params.diffusion = float(s.diffusion / 100);
    params.density = float(s.density / 100);
    params.contrast = float(s.contrast / 100);
    params.cell = int(s.style == DitherStyle::Scanlines ? s.lineSpacing : s.cellSize);
    params.angle = float(s.angle * M_PI / 180);
    params.lightOnDark = s.lightOnDark ? 1 : 0;
    params.originalColors = s.colors == DitherColors::Original ? 1 : 0;
    params.dark[0] = twoColors ? byteOf(s.dark.red) : 0;
    params.dark[1] = twoColors ? byteOf(s.dark.green) : 0;
    params.dark[2] = twoColors ? byteOf(s.dark.blue) : 0;
    params.light[0] = twoColors ? byteOf(s.light.red) : 255;
    params.light[1] = twoColors ? byteOf(s.light.green) : 255;
    params.light[2] = twoColors ? byteOf(s.light.blue) : 255;
    params.glyphWidth = glyphs.width;
    params.glyphHeight = glyphs.height;
    params.glyphs = glyphs.maps.empty() ? nullptr : glyphs.maps.data();
    params.glyphCoverage = glyphs.coverage.empty() ? nullptr : glyphs.coverage.data();
    params.glyphCount = int(glyphs.coverage.size());
    params.dots = float(s.dots / 100);
    params.wobble = float(s.wobble);
    return dither_apply(image.bits(), size_t(image.width()), size_t(image.height()), size_t(image.bytesPerLine()), &params) != 0;
}

// The lines' light, blurred across a few line spacings and added back over them, as a CRT's phosphors bloom.
QImage glowing(const QImage &image, const DitherSettings &s)
{
    const double sigma = s.lineSpacing * 3 + 3;
    const int shrink = std::max(1, int(std::floor(sigma / 4)));
    QImage source = image;
    if (shrink > 1) {
        source = image.scaled(std::max(1, image.width() / shrink), std::max(1, image.height() / shrink),
                              Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    QImage bloom = compositor::RasterOperations::gaussianBlur(source, sigma / shrink);
    if (shrink > 1) bloom = bloom.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    bloom = bloom.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QImage result = image;
    result.detach();
    // Same stride is required by the kernel.
    if (bloom.bytesPerLine() != result.bytesPerLine()) return {};
    dither_glow(result.bits(), bloom.constBits(), size_t(result.width()), size_t(result.height()),
                size_t(result.bytesPerLine()), float(s.glow / 100 * 2.5));
    return result;
}

}

namespace DitherInfo {
QString styleName(DitherStyle style)
{
    switch (style) {
    case DitherStyle::Atkinson: return QStringLiteral("Atkinson (Classic Mac)");
    case DitherStyle::FloydSteinberg: return QString::fromUtf8("Floyd\xe2\x80\x93Steinberg");
    case DitherStyle::Bayer2: return QString::fromUtf8("Bayer 2 \xc3\x97 2");
    case DitherStyle::Bayer4: return QString::fromUtf8("Bayer 4 \xc3\x97 4");
    case DitherStyle::Bayer8: return QString::fromUtf8("Bayer 8 \xc3\x97 8");
    case DitherStyle::Dots: return QStringLiteral("Halftone Dots");
    case DitherStyle::Lines: return QStringLiteral("Halftone Lines");
    case DitherStyle::Diamonds: return QStringLiteral("Halftone Diamonds");
    case DitherStyle::Patterns: return QStringLiteral("Mac Patterns");
    case DitherStyle::Ascii: return QStringLiteral("ASCII");
    case DitherStyle::Scanlines: return QStringLiteral("Scanlines (CRT)");
    }
    return {};
}
bool diffuses(DitherStyle s) { return s == DitherStyle::Atkinson || s == DitherStyle::FloydSteinberg; }
bool hasTones(DitherStyle s)
{
    return diffuses(s) || s == DitherStyle::Bayer2 || s == DitherStyle::Bayer4 || s == DitherStyle::Bayer8;
}
bool isHalftone(DitherStyle s) { return s == DitherStyle::Dots || s == DitherStyle::Lines || s == DitherStyle::Diamonds; }
bool drawsMarks(DitherStyle s) { return !hasTones(s) && s != DitherStyle::Scanlines; }
bool usesPixelSize(DitherStyle s) { return s != DitherStyle::Ascii && s != DitherStyle::Scanlines; }
}

DitherSettings DitherSettings::normalized() const
{
    DitherSettings r = *this;
    r.pixelSize = std::round(clampTo(pixelSize, pixelSizeMin, pixelSizeMax, 2));
    r.cellSize = std::round(clampTo(cellSize, cellSizeMin, cellSizeMax, 8));
    r.textSize = std::round(clampTo(textSize, textSizeMin, textSizeMax, 14));
    r.lineSpacing = std::round(clampTo(lineSpacing, lineSpacingMin, lineSpacingMax, 4));
    r.glow = clampTo(glow, 0, 100, 35);
    r.dots = clampTo(dots, 0, 100, 0);
    r.wobble = clampTo(wobble, wobbleMin, wobbleMax, 0);
    r.angle = clampTo(angle, -90, 90, 45);
    r.levels = std::round(clampTo(levels, levelsMin, levelsMax, 2));
    r.diffusion = clampTo(diffusion, 0, 100, 100);
    r.density = clampTo(density, -100, 100, 0);
    r.contrast = clampTo(contrast, -100, 100, 0);
    r.dark = clampedColor(dark);
    r.light = clampedColor(light);
    QString filtered;
    for (const QChar c : characters) {
        if (c == QLatin1Char('\n') || c == QLatin1Char('\r') || c == QChar::LineSeparator || c == QChar::ParagraphSeparator
            || c == QChar(0x85) || c == QChar(0x0b) || c == QChar(0x0c)) continue;
        filtered.append(c);
    }
    // At most 64 characters (not UTF-16 units).
    const QList<uint> codes = filtered.toUcs4();
    r.characters = QString::fromUcs4(reinterpret_cast<const char32_t *>(codes.constData()), std::min<qsizetype>(codes.size(), 64));
    return r;
}

QImage DitherSettings::apply(const QImage &input) const
{
    if (input.isNull()) return {};
    const DitherSettings s = normalized();
    const QImage image = input.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    // ASCII and Scanlines draw at full resolution.
    const int block = DitherInfo::usesPixelSize(s.style) ? int(s.pixelSize) : 1;
    QImage working = block > 1 ? boxDownsample(image, block) : image;
    working.detach();

    Glyphs glyphs;
    if (s.style == DitherStyle::Ascii)
        glyphs = buildGlyphs(s.characters.isEmpty() ? defaultCharacters() : s.characters, int(s.textSize));
    if (!runKernel(working, s, glyphs)) return {};
    if (s.style == DitherStyle::Scanlines && s.glow > 0) return glowing(working, s);
    if (block <= 1) return working;

    QImage full = nearestUpscale(working, block, image.size());
    if (s.pixelShape == DitherPixelShape::Dot) {
        uint8_t gap[3] = {0, 0, 0};
        if (s.colors == DitherColors::TwoColors) { gap[0] = byteOf(s.dark.red); gap[1] = byteOf(s.dark.green); gap[2] = byteOf(s.dark.blue); }
        dither_dots(full.bits(), size_t(full.width()), size_t(full.height()), size_t(full.bytesPerLine()), block, gap);
    }
    return full;
}
