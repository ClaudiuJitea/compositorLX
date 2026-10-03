#include "io/PSDReader.h"
#include "rendering/RasterOperations.h"

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <climits>
#include <cmath>
#include <cstring>

namespace compositor {

QString psdErrorMessage(PSDErrorCode code)
{
    switch (code) {
    case PSDErrorCode::Truncated:
        return QStringLiteral("The Photoshop file could not be read. It may be damaged or incomplete.");
    case PSDErrorCode::UnsupportedVersion:
        return QStringLiteral("This Photoshop file uses a format version Compositor can’t read.");
    case PSDErrorCode::UnsupportedColorMode:
    case PSDErrorCode::UnsupportedDepth:
        return QStringLiteral("Only 8-bit RGB Photoshop files can be imported.");
    case PSDErrorCode::UnsupportedCompression:
        return QStringLiteral("This Photoshop file uses a layer compression method that isn’t supported.");
    case PSDErrorCode::TooLarge:
        return QStringLiteral("The image is too large to import.");
    case PSDErrorCode::Unreadable:
    default:
        return QStringLiteral("The file could not be read.");
    }
}

std::optional<BlendMode> PSDReader::blendModeFromPSD(const QString &key)
{
    if (key == QStringLiteral("norm")) return BlendMode::Normal;
    if (key == QStringLiteral("mul ")) return BlendMode::Multiply;
    if (key == QStringLiteral("scrn")) return BlendMode::Screen;
    if (key == QStringLiteral("over")) return BlendMode::Overlay;
    if (key == QStringLiteral("sLit")) return BlendMode::SoftLight;
    if (key == QStringLiteral("dark")) return BlendMode::Darken;
    if (key == QStringLiteral("lite")) return BlendMode::Lighten;
    if (key == QStringLiteral("diff")) return BlendMode::Difference;
    if (key == QStringLiteral("div ")) return BlendMode::ColorDodge;
    if (key == QStringLiteral("idiv")) return BlendMode::ColorBurn;
    if (key == QStringLiteral("hue ")) return BlendMode::Hue;
    if (key == QStringLiteral("sat ")) return BlendMode::Saturation;
    if (key == QStringLiteral("colr")) return BlendMode::Color;
    if (key == QStringLiteral("lum ")) return BlendMode::Luminosity;
    if (key == QStringLiteral("lbrn")) return BlendMode::LinearBurn;
    if (key == QStringLiteral("lddg")) return BlendMode::LinearDodge;
    if (key == QStringLiteral("hLit")) return BlendMode::HardLight;
    if (key == QStringLiteral("vLit")) return BlendMode::VividLight;
    if (key == QStringLiteral("lLit")) return BlendMode::LinearLight;
    if (key == QStringLiteral("pLit")) return BlendMode::PinLight;
    if (key == QStringLiteral("hMix")) return BlendMode::HardMix;
    if (key == QStringLiteral("smud")) return BlendMode::Exclusion;
    if (key == QStringLiteral("fsub")) return BlendMode::Subtract;
    if (key == QStringLiteral("fdiv")) return BlendMode::Divide;
    return std::nullopt;
}

namespace {

constexpr int MaxSide = 30000;
constexpr qint64 MaxSurfacePixels = DocumentLimits::maxSurfacePixels;
constexpr qint64 MaxFileBytes = 512LL * 1024LL * 1024LL; // 512 MB file size limit
constexpr qint32 MaxCoordinate = 10000000; // 10 million pixel coordinate limit

[[nodiscard]] inline bool checkedAdd(int a, int b, int &out) noexcept
{
    if (a < 0 || b < 0) return false;
    if (a > std::numeric_limits<int>::max() - b) return false;
    out = a + b;
    return true;
}

[[nodiscard]] inline bool checkedLength(quint64 value, int &out) noexcept
{
    if (value > static_cast<quint64>(std::numeric_limits<int>::max())) return false;
    out = static_cast<int>(value);
    return true;
}

[[nodiscard]] inline bool checkedMultiply(int w, int h, qint64 &out) noexcept
{
    if (w < 0 || h < 0) return false;
    out = static_cast<qint64>(w) * static_cast<qint64>(h);
    return true;
}

struct PSDCrop {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

class PSDCursor {
public:
    explicit PSDCursor(const QByteArray &data) : data_(data) {}

    [[nodiscard]] int offset() const noexcept { return offset_; }
    bool setOffset(int offset) {
        if (offset < 0 || offset > data_.size()) return false;
        offset_ = offset;
        return true;
    }
    [[nodiscard]] int remaining() const noexcept {
        if (offset_ < 0 || offset_ > data_.size()) return 0;
        return data_.size() - offset_;
    }
    [[nodiscard]] int size() const noexcept { return data_.size(); }

    [[nodiscard]] bool need(int count) const {
        return count >= 0 && offset_ >= 0 && offset_ <= data_.size() - count;
    }

    bool skip(int count) {
        if (!need(count)) return false;
        offset_ += count;
        return true;
    }

    bool readU8(quint8 &val) {
        if (!need(1)) return false;
        val = static_cast<quint8>(data_[offset_++]);
        return true;
    }

    bool readU16(quint16 &val) {
        if (!need(2)) return false;
        const auto *p = reinterpret_cast<const quint8 *>(data_.constData() + offset_);
        val = (static_cast<quint16>(p[0]) << 8) | static_cast<quint16>(p[1]);
        offset_ += 2;
        return true;
    }

    bool readI16(qint16 &val) {
        quint16 u = 0;
        if (!readU16(u)) return false;
        val = static_cast<qint16>(u);
        return true;
    }

    bool readU32(quint32 &val) {
        if (!need(4)) return false;
        const auto *p = reinterpret_cast<const quint8 *>(data_.constData() + offset_);
        val = (static_cast<quint32>(p[0]) << 24) |
              (static_cast<quint32>(p[1]) << 16) |
              (static_cast<quint32>(p[2]) << 8)  |
               static_cast<quint32>(p[3]);
        offset_ += 4;
        return true;
    }

    bool readI32(qint32 &val) {
        quint32 u = 0;
        if (!readU32(u)) return false;
        val = static_cast<qint32>(u);
        return true;
    }

    bool readU64(quint64 &val) {
        if (!need(8)) return false;
        const auto *p = reinterpret_cast<const quint8 *>(data_.constData() + offset_);
        val = (static_cast<quint64>(p[0]) << 56) |
              (static_cast<quint64>(p[1]) << 48) |
              (static_cast<quint64>(p[2]) << 40) |
              (static_cast<quint64>(p[3]) << 32) |
              (static_cast<quint64>(p[4]) << 24) |
              (static_cast<quint64>(p[5]) << 16) |
              (static_cast<quint64>(p[6]) << 8)  |
               static_cast<quint64>(p[7]);
        offset_ += 8;
        return true;
    }

    bool readBytes(int count, QByteArray &out) {
        if (!need(count)) return false;
        out = data_.mid(offset_, count);
        offset_ += count;
        return true;
    }

    bool readString(int count, QString &out) {
        QByteArray bytes;
        if (!readBytes(count, bytes)) return false;
        out = QString::fromLatin1(bytes);
        return true;
    }

private:
    const QByteArray &data_;
    int offset_ = 0;
};

quint32 u32FromBytes(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    const auto *p = reinterpret_cast<const quint8 *>(data.constData() + offset);
    return (static_cast<quint32>(p[0]) << 24) |
           (static_cast<quint32>(p[1]) << 16) |
           (static_cast<quint32>(p[2]) << 8)  |
            static_cast<quint32>(p[3]);
}

quint16 u16FromBytes(const QByteArray &data, int offset)
{
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const auto *p = reinterpret_cast<const quint8 *>(data.constData() + offset);
    return (static_cast<quint16>(p[0]) << 8) | static_cast<quint16>(p[1]);
}

qint16 i16FromBytes(const QByteArray &data, int offset)
{
    return static_cast<qint16>(u16FromBytes(data, offset));
}

QString unicodeNameFromData(const QByteArray &data)
{
    if (data.size() < 4) return {};
    const int count = static_cast<int>(u32FromBytes(data, 0));
    if (count <= 0 || data.size() < 4 + count * 2) return {};
    QString result;
    result.reserve(count);
    for (int i = 0; i < count; ++i) {
        const quint8 hi = static_cast<quint8>(data[4 + i * 2]);
        const quint8 lo = static_cast<quint8>(data[5 + i * 2]);
        const char16_t ch = static_cast<char16_t>((static_cast<quint16>(hi) << 8) | lo);
        if (ch != 0) {
            result.append(QChar(ch));
        }
    }
    return result.trimmed();
}

const QSet<QString> psbLargeAdditionalInfoKeys = {
    QStringLiteral("LMsk"), QStringLiteral("Lr16"), QStringLiteral("Lr32"),
    QStringLiteral("Layr"), QStringLiteral("Mt16"), QStringLiteral("Mt32"),
    QStringLiteral("Mtrn"), QStringLiteral("Alph"), QStringLiteral("FMsk"),
    QStringLiteral("lnk2"), QStringLiteral("FEid"), QStringLiteral("FXid"),
    QStringLiteral("PxSD")
};

const QSet<QString> adjustmentKeys = {
    QStringLiteral("levl"), QStringLiteral("curv"), QStringLiteral("hue2"), QStringLiteral("hue "),
    QStringLiteral("expA"), QStringLiteral("grdm"), QStringLiteral("brit"), QStringLiteral("blnc"),
    QStringLiteral("nvrt"), QStringLiteral("thrs"), QStringLiteral("post"), QStringLiteral("mixr"),
    QStringLiteral("selc"), QStringLiteral("blwh"), QStringLiteral("phfl"), QStringLiteral("vibA")
};

enum class PSDLayerKind {
    Raster,
    Group,
    Adjustment,
    Text,
    SmartObject,
    Effects,
    Vector,
    Other
};

struct RawLayer {
    QString name;
    int top = 0;
    int left = 0;
    int bottom = 0;
    int right = 0;
    int sourceTop = 0;
    int sourceLeft = 0;
    int sourceBottom = 0;
    int sourceRight = 0;
    quint8 opacity = 255;
    quint8 fill = 255;
    bool clipping = false;
    bool hidden = false;
    QString blendKey = QStringLiteral("norm");
    QVector<QPair<int, int>> channels; // (id, length)
    QMap<QString, QByteArray> extra;
    int maskTop = 0;
    int maskLeft = 0;
    int maskBottom = 0;
    int maskRight = 0;
    int sourceMaskTop = 0;
    int sourceMaskLeft = 0;
    int sourceMaskBottom = 0;
    int sourceMaskRight = 0;
    quint8 maskDefault = 255;
    bool maskDisabled = false;
    bool maskLinked = true;
    bool maskFromRender = false;
    bool hasMask = false;
    std::optional<int> section;
    QImage image;
    QImage maskImage;
    std::optional<PSDCrop> imageCrop;
    std::optional<PSDCrop> maskCrop;
    bool cropped = false;
};

PSDCrop calculateCrop(int left, int top, int right, int bottom, int canvasWidth, int canvasHeight)
{
    const qint64 cLeft = std::min(static_cast<qint64>(canvasWidth), std::max(0LL, static_cast<qint64>(left)));
    const qint64 cTop = std::min(static_cast<qint64>(canvasHeight), std::max(0LL, static_cast<qint64>(top)));
    const qint64 cRight = std::max(cLeft, std::min(static_cast<qint64>(canvasWidth), static_cast<qint64>(right)));
    const qint64 cBottom = std::max(cTop, std::min(static_cast<qint64>(canvasHeight), static_cast<qint64>(bottom)));
    return PSDCrop{
        static_cast<int>(cLeft - static_cast<qint64>(left)),
        static_cast<int>(cTop - static_cast<qint64>(top)),
        static_cast<int>(cRight - cLeft),
        static_cast<int>(cBottom - cTop)
    };
}

void cropToCanvas(RawLayer &layer, int width, int height)
{
    const PSDCrop imageCrop = calculateCrop(layer.left, layer.top, layer.right, layer.bottom, width, height);
    const qint64 origW = static_cast<qint64>(layer.right) - static_cast<qint64>(layer.left);
    const qint64 origH = static_cast<qint64>(layer.bottom) - static_cast<qint64>(layer.top);
    if (imageCrop.x != 0 || imageCrop.y != 0 ||
        static_cast<qint64>(imageCrop.width) != origW ||
        static_cast<qint64>(imageCrop.height) != origH) {
        layer.left += imageCrop.x;
        layer.top += imageCrop.y;
        layer.right = layer.left + imageCrop.width;
        layer.bottom = layer.top + imageCrop.height;
        layer.imageCrop = imageCrop;
        layer.cropped = true;
    }
    if (layer.hasMask) {
        const PSDCrop maskCrop = calculateCrop(layer.maskLeft, layer.maskTop, layer.maskRight, layer.maskBottom, width, height);
        const qint64 origMaskW = static_cast<qint64>(layer.maskRight) - static_cast<qint64>(layer.maskLeft);
        const qint64 origMaskH = static_cast<qint64>(layer.maskBottom) - static_cast<qint64>(layer.maskTop);
        if (maskCrop.x != 0 || maskCrop.y != 0 ||
            static_cast<qint64>(maskCrop.width) != origMaskW ||
            static_cast<qint64>(maskCrop.height) != origMaskH) {
            layer.maskLeft += maskCrop.x;
            layer.maskTop += maskCrop.y;
            layer.maskRight = layer.maskLeft + maskCrop.width;
            layer.maskBottom = layer.maskTop + maskCrop.height;
            layer.maskCrop = maskCrop;
            layer.cropped = true;
        }
    }
}

bool fitsBudget(int width, int height, int maskWidth, int maskHeight, bool hasMask, qint64 remainingPixels)
{
    const qint64 budget = std::max(0LL, remainingPixels);
    qint64 layerPixels = 0;
    if (width > 0 && height > 0) {
        if (width > MaxSide || height > MaxSide) return false;
        layerPixels += (static_cast<qint64>(width) * height);
    }
    if (hasMask && maskWidth > 0 && maskHeight > 0) {
        if (maskWidth > MaxSide || maskHeight > MaxSide) return false;
        layerPixels += (static_cast<qint64>(maskWidth) * maskHeight);
    }
    return layerPixels <= budget;
}

bool fitsBudget(const QVector<RawLayer> &layers, qint64 remainingPixels)
{
    qint64 used = 0;
    const qint64 budget = std::max(0LL, remainingPixels);
    for (const auto &layer : layers) {
        const qint64 spanW = static_cast<qint64>(layer.right) - static_cast<qint64>(layer.left);
        const qint64 spanH = static_cast<qint64>(layer.bottom) - static_cast<qint64>(layer.top);
        const qint64 maskSpanW = static_cast<qint64>(layer.maskRight) - static_cast<qint64>(layer.maskLeft);
        const qint64 maskSpanH = static_cast<qint64>(layer.maskBottom) - static_cast<qint64>(layer.maskTop);
        const int width = static_cast<int>(std::max(0LL, spanW));
        const int height = static_cast<int>(std::max(0LL, spanH));
        const int maskWidth = static_cast<int>(std::max(0LL, maskSpanW));
        const int maskHeight = static_cast<int>(std::max(0LL, maskSpanH));
        if (!fitsBudget(width, height, maskWidth, maskHeight, layer.hasMask, budget - used)) return false;
        if (width > 0 && height > 0) used += (static_cast<qint64>(width) * height);
        if (layer.hasMask && maskWidth > 0 && maskHeight > 0) used += (static_cast<qint64>(maskWidth) * maskHeight);
    }
    return true;
}

PSDLayerKind layerKind(const RawLayer &layer, bool isGroup)
{
    if (isGroup) return PSDLayerKind::Group;
    for (const QString &k : {QStringLiteral("TySh"), QStringLiteral("tySh"), QStringLiteral("txt2")}) {
        if (layer.extra.contains(k)) return PSDLayerKind::Text;
    }
    for (const QString &k : {QStringLiteral("vmsk"), QStringLiteral("vsms"), QStringLiteral("vogk")}) {
        if (layer.extra.contains(k)) return PSDLayerKind::Vector;
    }
    for (const QString &k : {QStringLiteral("SoLd"), QStringLiteral("SoLE")}) {
        if (layer.extra.contains(k)) return PSDLayerKind::SmartObject;
    }
    for (const QString &k : {QStringLiteral("lfx2"), QStringLiteral("lrFX"), QStringLiteral("lmfx")}) {
        if (layer.extra.contains(k)) return PSDLayerKind::Effects;
    }
    for (const QString &k : adjustmentKeys) {
        if (layer.extra.contains(k)) return PSDLayerKind::Adjustment;
    }
    return PSDLayerKind::Raster;
}

bool unpackRLE(int width, int height, const QByteArray &data, bool largeDocument,
               const std::optional<PSDCrop> &crop, QVector<quint8> &outPlane, QString *error)
{
    if (width <= 0 || height <= 0) {
        outPlane.clear();
        return true;
    }
    PSDCursor cursor(data);
    QVector<int> counts(height);
    for (int r = 0; r < height; ++r) {
        if (largeDocument) {
            quint32 count = 0;
            if (!cursor.readU32(count)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            int c = 0;
            if (!checkedLength(count, c)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
                return false;
            }
            counts[r] = c;
        } else {
            quint16 count = 0;
            if (!cursor.readU16(count)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            counts[r] = static_cast<int>(count);
        }
    }

    if (!crop.has_value()) {
        const qint64 total64 = static_cast<qint64>(width) * height;
        if (total64 > INT_MAX) {
            if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
            return false;
        }
        outPlane.resize(static_cast<int>(total64));
        for (int row = 0; row < height; ++row) {
            int end = 0;
            if (!checkedAdd(cursor.offset(), counts[row], end) || end > cursor.size()) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            int written = 0;
            while (written < width) {
                if (cursor.offset() >= end) {
                    if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                    return false;
                }
                quint8 b = 0;
                cursor.readU8(b);
                const qint8 n = static_cast<qint8>(b);
                if (n >= 0) {
                    const int count = static_cast<int>(n) + 1;
                    int readEnd = 0;
                    if (written + count > width || !checkedAdd(cursor.offset(), count, readEnd) || readEnd > end) {
                        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                        return false;
                    }
                    for (int i = 0; i < count; ++i) {
                        quint8 val = 0;
                        cursor.readU8(val);
                        outPlane[row * width + written + i] = val;
                    }
                    written += count;
                } else if (n != -128) {
                    const int count = 1 - static_cast<int>(n);
                    if (written + count > width || cursor.offset() >= end) {
                        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                        return false;
                    }
                    quint8 val = 0;
                    cursor.readU8(val);
                    for (int i = 0; i < count; ++i) {
                        outPlane[row * width + written + i] = val;
                    }
                    written += count;
                }
            }
            if (!cursor.setOffset(end)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
        }
    } else {
        const PSDCrop &c = *crop;
        if (c.x < 0 || c.y < 0 || c.width < 0 || c.height < 0 ||
            c.x + c.width > width || c.y + c.height > height) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        if (c.width == 0 || c.height == 0) {
            outPlane.clear();
            return true;
        }
        const qint64 cropSize64 = static_cast<qint64>(c.width) * c.height;
        if (cropSize64 > INT_MAX) {
            if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
            return false;
        }
        outPlane.resize(static_cast<int>(cropSize64));
        QVector<quint8> rowBuffer(width);
        for (int row = 0; row < height; ++row) {
            int end = 0;
            if (!checkedAdd(cursor.offset(), counts[row], end) || end > cursor.size()) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            if (row < c.y || row >= c.y + c.height) {
                if (!cursor.setOffset(end)) {
                    if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                    return false;
                }
                continue;
            }
            int written = 0;
            while (written < width) {
                if (cursor.offset() >= end) {
                    if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                    return false;
                }
                quint8 b = 0;
                cursor.readU8(b);
                const qint8 n = static_cast<qint8>(b);
                if (n >= 0) {
                    const int count = static_cast<int>(n) + 1;
                    int readEnd = 0;
                    if (written + count > width || !checkedAdd(cursor.offset(), count, readEnd) || readEnd > end) {
                        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                        return false;
                    }
                    for (int i = 0; i < count; ++i) {
                        quint8 val = 0;
                        cursor.readU8(val);
                        rowBuffer[written + i] = val;
                    }
                    written += count;
                } else if (n != -128) {
                    const int count = 1 - static_cast<int>(n);
                    if (written + count > width || cursor.offset() >= end) {
                        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                        return false;
                    }
                    quint8 val = 0;
                    cursor.readU8(val);
                    for (int i = 0; i < count; ++i) {
                        rowBuffer[written + i] = val;
                    }
                    written += count;
                }
            }
            const int targetStart = (row - c.y) * c.width;
            for (int i = 0; i < c.width; ++i) {
                outPlane[targetStart + i] = rowBuffer[c.x + i];
            }
            if (!cursor.setOffset(end)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
        }
    }
    return true;
}

bool decodeRaw(int width, int height, const QByteArray &data,
               const std::optional<PSDCrop> &crop, QVector<quint8> &outPlane, QString *error)
{
    const qint64 expected64 = static_cast<qint64>(width) * height;
    if (expected64 > INT_MAX || data.size() < expected64) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }
    const int expected = static_cast<int>(expected64);
    if (!crop.has_value()) {
        outPlane.resize(expected);
        std::memcpy(outPlane.data(), data.constData(), expected);
    } else {
        const PSDCrop &c = *crop;
        if (c.x < 0 || c.y < 0 || c.width < 0 || c.height < 0 ||
            c.x + c.width > width || c.y + c.height > height) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        const qint64 cropSize64 = static_cast<qint64>(c.width) * c.height;
        if (cropSize64 > INT_MAX) {
            if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
            return false;
        }
        outPlane.resize(static_cast<int>(cropSize64));
        const auto *src = reinterpret_cast<const quint8 *>(data.constData());
        for (int r = 0; r < c.height; ++r) {
            const int srcStart = (c.y + r) * width + c.x;
            const int dstStart = r * c.width;
            std::memcpy(outPlane.data() + dstStart, src + srcStart, c.width);
        }
    }
    return true;
}

bool decodeChannelPlane(int compression, int width, int height, const QByteArray &payload,
                        bool largeDocument, const std::optional<PSDCrop> &crop,
                        QVector<quint8> &outPlane, QString *error)
{
    if (compression == 0) {
        return decodeRaw(width, height, payload, crop, outPlane, error);
    }
    if (compression == 1) {
        return unpackRLE(width, height, payload, largeDocument, crop, outPlane, error);
    }
    if (error) *error = psdErrorMessage(PSDErrorCode::UnsupportedCompression);
    return false;
}

QImage makeRgbaImage(int width, int height,
                     const QVector<quint8> &red,
                     const QVector<quint8> &green,
                     const QVector<quint8> &blue,
                     const QVector<quint8> &alpha)
{
    if (width <= 0 || height <= 0) return {};
    const qint64 count = static_cast<qint64>(width) * height;
    if (red.size() < count || green.size() < count || blue.size() < count || alpha.size() < count) return {};
    QImage img(width, height, QImage::Format_RGBA8888_Premultiplied);
    if (img.isNull()) return {};
    const qsizetype bpl = img.bytesPerLine();
    auto *dst = img.bits();
    for (int y = 0; y < height; ++y) {
        auto *dstRow = dst + y * bpl;
        const qint64 rowOffset = static_cast<qint64>(y) * width;
        for (int x = 0; x < width; ++x) {
            const qint64 i = rowOffset + x;
            const quint8 a = alpha[i];
            const quint8 r = static_cast<quint8>((static_cast<quint16>(red[i]) * a + 127) / 255);
            const quint8 g = static_cast<quint8>((static_cast<quint16>(green[i]) * a + 127) / 255);
            const quint8 b = static_cast<quint8>((static_cast<quint16>(blue[i]) * a + 127) / 255);
            dstRow[x * 4 + 0] = r;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = b;
            dstRow[x * 4 + 3] = a;
        }
    }
    return img;
}

QImage makeMaskImage(int width, int height, const QVector<quint8> &gray)
{
    if (width <= 0 || height <= 0) return {};
    const qint64 count = static_cast<qint64>(width) * height;
    if (gray.size() < count) return {};
    QImage img(width, height, QImage::Format_Grayscale8);
    if (img.isNull()) return {};
    const qsizetype bpl = img.bytesPerLine();
    uchar *dst = img.bits();
    const quint8 *src = gray.constData();
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst + y * bpl, src + static_cast<qint64>(y) * width, width);
    }
    return img;
}

std::optional<QJsonObject> parseLevelsAdjustment(const QByteArray &data)
{
    if (data.size() < 292) return std::nullopt;
    LevelsSettings settings;
    for (int ch = 0; ch < 4; ++ch) {
        const int base = 2 + ch * 10;
        const double inBlack = double(u16FromBytes(data, base));
        const double inWhite = double(u16FromBytes(data, base + 2));
        const double outBlack = double(u16FromBytes(data, base + 4));
        const double outWhite = double(u16FromBytes(data, base + 6));
        const double gamma = double(u16FromBytes(data, base + 8)) / 100.0;   // hundredths: 100 is 1.00 (mac 8c0417b)
        settings.ranges[size_t(ch)] = LevelRange{
            std::clamp(inBlack, 0.0, 255.0),
            std::clamp(gamma, 0.01, 9.99),
            std::clamp(inWhite, 0.0, 255.0),
            std::clamp(outBlack, 0.0, 255.0),
            std::clamp(outWhite, 0.0, 255.0)
        }.normalized();
    }
    return RasterOperations::levelsSettingsToJson(settings);
}

std::optional<QJsonObject> parseCurvesAdjustment(const QByteArray &data)
{
    if (data.size() < 5) return std::nullopt;
    int offset = 0;
    if (data[offset] == 0) offset += 1;
    if (offset + 4 > data.size()) return std::nullopt;
    const quint16 version = u16FromBytes(data, offset);
    offset += 2;
    if (version != 1 && version != 4) return std::nullopt;
    const int count = int(u16FromBytes(data, offset));
    offset += 2;
    CurvesSettings settings;
    for (int c = 0; c < std::min(4, count); ++c) {
        if (offset + 2 > data.size()) return std::nullopt;
        const int points = int(u16FromBytes(data, offset));
        offset += 2;
        QVector<CurvePoint> curve;
        for (int p = 0; p < points; ++p) {
            if (offset + 4 > data.size()) return std::nullopt;
            const double outVal = double(u16FromBytes(data, offset));
            const double inVal = double(u16FromBytes(data, offset + 2));
            offset += 4;
            curve.push_back({std::clamp(inVal, 0.0, 255.0), std::clamp(outVal, 0.0, 255.0)});
        }
        if (curve.size() >= 2) {
            std::sort(curve.begin(), curve.end(), [](const CurvePoint &a, const CurvePoint &b) {
                return a.x < b.x;
            });
            if (curve.first().x > 0.0) {
                curve.prepend({0.0, curve.first().y});
            }
            if (curve.last().x < 255.0) {
                curve.append({255.0, curve.last().y});
            }
            settings.channels[size_t(c)] = curve;
        }
    }
    if (!settings.isValid()) return std::nullopt;
    return RasterOperations::curvesSettingsToJson(settings);
}

// Photoshop's 'hue2': a version, the Colorize switch and a pad byte, the Colorize hue/saturation/lightness, the
// Master's, then for Reds through Magentas the band (where the range fades in, is full, and fades out, in degrees)
// and its hue, saturation and lightness (mac 8c0417b).
std::optional<QJsonObject> parseHueSaturationAdjustment(const QByteArray &data)
{
    if (data.size() < 16) return std::nullopt;
    const bool colorize = (data[2] != 0);
    HueSaturationSettings settings;
    settings.colorize = colorize;
    const auto values = [&](int offset) {
        return RangeAdjustment{double(i16FromBytes(data, offset)), double(i16FromBytes(data, offset + 2)), double(i16FromBytes(data, offset + 4))};
    };
    // Colorize has values of its own; the Master applies otherwise.
    settings.adjustments[size_t(ColorRange::Master)] = values(colorize ? 4 : 10);
    if (!colorize) {
        const auto degrees = [&](int at) {
            double value = std::fmod(double(i16FromBytes(data, at)), 360.0);
            return value < 0 ? value + 360.0 : value;
        };
        int offset = 16;
        for (size_t r = size_t(ColorRange::Reds); r < size_t(ColorRange::Count); ++r) {
            if (offset + 14 > data.size()) break;
            settings.bands[r] = HueBand{degrees(offset), degrees(offset + 2), degrees(offset + 4), degrees(offset + 6)};
            settings.adjustments[r] = values(offset + 8);
            offset += 14;
        }
    }
    return RasterOperations::hueSaturationSettingsToJson(settings);
}

std::optional<QJsonObject> parseAdjustment(const QMap<QString, QByteArray> &extra)
{
    if (extra.contains(QStringLiteral("levl"))) {
        return parseLevelsAdjustment(extra[QStringLiteral("levl")]);
    }
    if (extra.contains(QStringLiteral("curv"))) {
        return parseCurvesAdjustment(extra[QStringLiteral("curv")]);
    }
    if (extra.contains(QStringLiteral("hue2"))) {
        return parseHueSaturationAdjustment(extra[QStringLiteral("hue2")]);
    }
    if (extra.contains(QStringLiteral("hue "))) {
        return parseHueSaturationAdjustment(extra[QStringLiteral("hue ")]);
    }
    return std::nullopt;
}

} // namespace

std::optional<QJsonObject> PSDReader::levelsAdjustment(const QByteArray &data) { return parseLevelsAdjustment(data); }
std::optional<QJsonObject> PSDReader::hueSaturationAdjustment(const QByteArray &data) { return parseHueSaturationAdjustment(data); }

// A PSD layer mask on the layer's own pixel grid, as Compositor's masks are: the stored patch drawn where it sits on
// the document, and Photoshop's default value everywhere else. The patch alone, stretched over the layer, would put
// the mask in the wrong place. Adjustment layers and folders cover the canvas (mac 8c0417b).
QImage PSDReader::maskOnLayerGrid(const QImage &patch, const QRectF &maskBounds, quint8 maskDefault,
                                  const QRectF &layerPlacement, const QSize &layerGrid)
{
    if (layerGrid.width() < 1 || layerGrid.height() < 1 || layerPlacement.width() <= 0 || layerPlacement.height() <= 0
        || maskBounds.width() <= 0 || maskBounds.height() <= 0) return patch;
    const double sx = layerGrid.width() / layerPlacement.width(), sy = layerGrid.height() / layerPlacement.height();
    const QRectF rect((maskBounds.x() - layerPlacement.x()) * sx, (maskBounds.y() - layerPlacement.y()) * sy,
                      maskBounds.width() * sx, maskBounds.height() * sy);
    if (rect.toAlignedRect() == QRect(QPoint(), layerGrid) && patch.size() == layerGrid) return patch;
    QImage grid(layerGrid, QImage::Format_Grayscale8);
    grid.fill(maskDefault);
    QPainter painter(&grid);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(rect, patch.convertToFormat(QImage::Format_Grayscale8));
    return grid;
}

bool PSDReader::matches(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const qint64 sz = file.size();
    if (sz < 4 || sz > MaxFileBytes) return false;
    const QByteArray header = file.read(4);
    return header.size() == 4 && header.startsWith("8BPS");
}

bool PSDReader::matches(const QByteArray &data)
{
    return data.size() >= 4 && data.size() <= MaxFileBytes && data.startsWith("8BPS");
}

bool PSDReader::read(const QString &path, PSDImportResult &result, QString *error, qint64 remainingPixels)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Unreadable);
        return false;
    }
    const qint64 sz = file.size();
    if (sz < 26) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }
    if (sz > MaxFileBytes) {
        if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
        return false;
    }
    // Bound the actual read to the accepted file size plus one byte so concurrent
    // file growth cannot cause an unbounded allocation.
    const QByteArray data = file.read(sz + 1);
    if (data.size() != sz) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }
    return read(data, result, error, remainingPixels);
}

bool PSDReader::read(const QByteArray &data, PSDImportResult &result, QString *error, qint64 remainingPixels)
{
    if (data.size() < 26) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }
    if (data.size() > MaxFileBytes) {
        if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
        return false;
    }

    PSDCursor cursor(data);

    QString magic;
    if (!cursor.readString(4, magic) || magic != QStringLiteral("8BPS")) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Unreadable);
        return false;
    }

    quint16 version = 0;
    if (!cursor.readU16(version)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }
    if (version != 1 && version != 2) {
        if (error) *error = psdErrorMessage(PSDErrorCode::UnsupportedVersion);
        return false;
    }
    const bool isPSB = (version == 2);

    if (!cursor.skip(6)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }

    quint16 headerChannels = 0;
    if (!cursor.readU16(headerChannels)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }

    quint32 heightU32 = 0;
    quint32 widthU32 = 0;
    if (!cursor.readU32(heightU32) || !cursor.readU32(widthU32)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }
    int canvasHeight = 0;
    int canvasWidth = 0;
    if (!checkedLength(heightU32, canvasHeight) || !checkedLength(widthU32, canvasWidth)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
        return false;
    }

    quint16 depth = 0;
    quint16 mode = 0;
    if (!cursor.readU16(depth) || !cursor.readU16(mode)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }

    qint64 canvasSurface = 0;
    if (canvasWidth < 1 || canvasWidth > MaxSide || canvasHeight < 1 || canvasHeight > MaxSide ||
        !checkedMultiply(canvasWidth, canvasHeight, canvasSurface) || canvasSurface > MaxSurfacePixels) {
        if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
        return false;
    }

    if (depth != 8) {
        if (error) *error = psdErrorMessage(PSDErrorCode::UnsupportedDepth);
        return false;
    }

    if (mode != 3) { // 3 = RGB
        if (error) *error = psdErrorMessage(PSDErrorCode::UnsupportedColorMode);
        return false;
    }

    quint32 colorDataLen = 0;
    if (!cursor.readU32(colorDataLen)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }
    int colorDataLenInt = 0;
    if (!checkedLength(colorDataLen, colorDataLenInt) || !cursor.skip(colorDataLenInt)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }

    quint32 resLen = 0;
    if (!cursor.readU32(resLen)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }
    int resLenInt = 0;
    if (!checkedLength(resLen, resLenInt)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
        return false;
    }
    int resourcesEnd = 0;
    if (!checkedAdd(cursor.offset(), resLenInt, resourcesEnd) || resourcesEnd > cursor.size()) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }

    double resolution = 72.0;
    while (cursor.offset() + 12 <= resourcesEnd) {
        QString sig;
        if (!cursor.readString(4, sig) || sig != QStringLiteral("8BIM")) break;
        quint16 id = 0;
        cursor.readU16(id);
        quint8 nameLen = 0;
        cursor.readU8(nameLen);
        cursor.skip(static_cast<int>(nameLen));
        if ((static_cast<int>(nameLen) + 1) % 2 == 1) cursor.skip(1);
        quint32 length = 0;
        if (!cursor.readU32(length)) break;
        int lengthInt = 0;
        if (!checkedLength(length, lengthInt)) break;
        const int dataStart = cursor.offset();
        if (id == 1005 && length >= 4) {
            quint32 fixed = 0;
            cursor.readU32(fixed);
            double res = double(fixed) / 65536.0;
            if (!std::isfinite(res) || res < 1.0) res = 72.0;
            resolution = std::clamp(res, 1.0, 9600.0);
        }
        int nextResOffset = 0;
        if (!checkedAdd(dataStart, lengthInt, nextResOffset) || nextResOffset > resourcesEnd) break;
        if (!cursor.setOffset(nextResOffset)) break;
        if (length % 2 == 1) cursor.skip(1);
    }
    if (!cursor.setOffset(resourcesEnd)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }

    quint64 layerSectionLenU64 = 0;
    if (isPSB) {
        if (!cursor.readU64(layerSectionLenU64)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
    } else {
        quint32 len32 = 0;
        if (!cursor.readU32(len32)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        layerSectionLenU64 = len32;
    }
    int layerSection = 0;
    if (!checkedLength(layerSectionLenU64, layerSection)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
        return false;
    }
    int layerSectionEnd = 0;
    if (!checkedAdd(cursor.offset(), layerSection, layerSectionEnd) || layerSectionEnd > cursor.size()) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }

    Document tempDoc;
    tempDoc.formatVersion = 9;
    tempDoc.id = QUuid::createUuid();
    tempDoc.generation = 1;
    tempDoc.canvasSize = QSize(canvasWidth, canvasHeight);
    tempDoc.resolution = resolution;

    QVector<PSDConversion> conversions;

    // Flattened file fallback (no layer section records)
    if (layerSection < 4) {
        if (!cursor.setOffset(layerSectionEnd)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        if (cursor.remaining() < 2) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        quint16 comp = 0;
        if (!cursor.readU16(comp) || (comp != 0 && comp != 1)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::UnsupportedCompression);
            return false;
        }
        const int numChannels = std::clamp(static_cast<int>(headerChannels), 3, 4);
        QVector<quint8> redPlane, greenPlane, bluePlane, alphaPlane;
        bool ok = true;
        if (comp == 0) {
            qint64 planeSize64 = 0;
            if (!checkedMultiply(canvasWidth, canvasHeight, planeSize64) || planeSize64 > INT_MAX) {
                if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
                return false;
            }
            const int planeSize = static_cast<int>(planeSize64);
            QByteArray redBytes, greenBytes, blueBytes, alphaBytes;
            ok = cursor.readBytes(planeSize, redBytes) &&
                 cursor.readBytes(planeSize, greenBytes) &&
                 cursor.readBytes(planeSize, blueBytes);
            if (ok) {
                decodeRaw(canvasWidth, canvasHeight, redBytes, std::nullopt, redPlane, error);
                decodeRaw(canvasWidth, canvasHeight, greenBytes, std::nullopt, greenPlane, error);
                decodeRaw(canvasWidth, canvasHeight, blueBytes, std::nullopt, bluePlane, error);
                if (numChannels >= 4 && cursor.readBytes(planeSize, alphaBytes)) {
                    decodeRaw(canvasWidth, canvasHeight, alphaBytes, std::nullopt, alphaPlane, error);
                } else {
                    alphaPlane.resize(planeSize);
                    alphaPlane.fill(255);
                }
            }
        } else if (comp == 1) {
            // RLE compressed composite
            const int totalScanlines = numChannels * canvasHeight;
            QVector<int> scanlineCounts(totalScanlines);
            for (int s = 0; s < totalScanlines; ++s) {
                if (isPSB) {
                    quint32 count = 0;
                    if (!cursor.readU32(count)) { ok = false; break; }
                    int c = 0;
                    if (!checkedLength(count, c)) { ok = false; break; }
                    scanlineCounts[s] = c;
                } else {
                    quint16 count = 0;
                    if (!cursor.readU16(count)) { ok = false; break; }
                    scanlineCounts[s] = static_cast<int>(count);
                }
            }
            if (ok) {
                const auto unpackPlane = [&](QVector<quint8> &plane, int channelIndex) -> bool {
                    qint64 planeSize64 = 0;
                    if (!checkedMultiply(canvasWidth, canvasHeight, planeSize64) || planeSize64 > INT_MAX) return false;
                    plane.resize(static_cast<int>(planeSize64));
                    for (int row = 0; row < canvasHeight; ++row) {
                        const int count = scanlineCounts[channelIndex * canvasHeight + row];
                        int end = 0;
                        if (!checkedAdd(cursor.offset(), count, end) || end > cursor.size()) return false;
                        int written = 0;
                        while (written < canvasWidth) {
                            if (cursor.offset() >= end) return false;
                            quint8 b = 0;
                            cursor.readU8(b);
                            const qint8 n = static_cast<qint8>(b);
                            if (n >= 0) {
                                const int len = static_cast<int>(n) + 1;
                                int readEnd = 0;
                                if (written + len > canvasWidth || !checkedAdd(cursor.offset(), len, readEnd) || readEnd > end) return false;
                                for (int i = 0; i < len; ++i) {
                                    quint8 val = 0;
                                    cursor.readU8(val);
                                    plane[row * canvasWidth + written + i] = val;
                                }
                                written += len;
                            } else if (n != -128) {
                                const int len = 1 - static_cast<int>(n);
                                if (written + len > canvasWidth || cursor.offset() >= end) return false;
                                quint8 val = 0;
                                cursor.readU8(val);
                                for (int i = 0; i < len; ++i) {
                                    plane[row * canvasWidth + written + i] = val;
                                }
                                written += len;
                            }
                        }
                        if (!cursor.setOffset(end)) return false;
                    }
                    return true;
                };
                ok = unpackPlane(redPlane, 0) &&
                     unpackPlane(greenPlane, 1) &&
                     unpackPlane(bluePlane, 2);
                if (ok && numChannels >= 4) {
                    ok = unpackPlane(alphaPlane, 3);
                } else {
                    qint64 planeSize64 = static_cast<qint64>(canvasWidth) * canvasHeight;
                    alphaPlane.resize(static_cast<int>(planeSize64));
                    alphaPlane.fill(255);
                }
            }
        }
        if (!ok || redPlane.isEmpty()) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        QImage composite = makeRgbaImage(canvasWidth, canvasHeight, redPlane, greenPlane, bluePlane, alphaPlane);
        Layer bg;
        bg.id = QUuid::createUuid();
        bg.name = QStringLiteral("Background");
        bg.image = std::move(composite);
        bg.transform.origin = QPointF(0, 0);
        bg.transform.size = QSizeF(canvasWidth, canvasHeight);
        tempDoc.layers.push_back(std::move(bg));
        tempDoc.activeLayerId = tempDoc.layers.first().id;

        result.document = std::move(tempDoc);
        result.conversions = std::move(conversions);
        return true;
    }

    quint64 layerInfoLenU64 = 0;
    if (isPSB) {
        if (!cursor.readU64(layerInfoLenU64)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
    } else {
        quint32 len32 = 0;
        if (!cursor.readU32(len32)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        layerInfoLenU64 = len32;
    }
    int layerInfoLen = 0;
    if (!checkedLength(layerInfoLenU64, layerInfoLen)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
        return false;
    }
    int layerInfoEnd = 0;
    if (!checkedAdd(cursor.offset(), layerInfoLen, layerInfoEnd) || layerInfoEnd > layerSectionEnd) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }

    qint16 rawCount = 0;
    if (!cursor.readI16(rawCount)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }
    const int count = std::abs(static_cast<int>(rawCount));
    if (count > 10000) {
        if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
        return false;
    }

    QVector<RawLayer> raw;
    raw.reserve(count);

    for (int i = 0; i < count; ++i) {
        RawLayer layer;
        qint32 top = 0, left = 0, bottom = 0, right = 0;
        if (!cursor.readI32(top) || !cursor.readI32(left) ||
            !cursor.readI32(bottom) || !cursor.readI32(right)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }

        // Reject inverted or unrepresentable bounds before cropping or decoding
        if (top == std::numeric_limits<qint32>::min() || top == std::numeric_limits<qint32>::max() ||
            left == std::numeric_limits<qint32>::min() || left == std::numeric_limits<qint32>::max() ||
            bottom == std::numeric_limits<qint32>::min() || bottom == std::numeric_limits<qint32>::max() ||
            right == std::numeric_limits<qint32>::min() || right == std::numeric_limits<qint32>::max() ||
            top < -MaxCoordinate || top > MaxCoordinate ||
            left < -MaxCoordinate || left > MaxCoordinate ||
            bottom < -MaxCoordinate || bottom > MaxCoordinate ||
            right < -MaxCoordinate || right > MaxCoordinate) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        const qint64 layerSpanW = static_cast<qint64>(right) - static_cast<qint64>(left);
        const qint64 layerSpanH = static_cast<qint64>(bottom) - static_cast<qint64>(top);
        if (layerSpanW < 0 || layerSpanH < 0) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        if (layerSpanW > MaxSide || layerSpanH > MaxSide) {
            if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
            return false;
        }

        layer.top = top; layer.left = left; layer.bottom = bottom; layer.right = right;
        layer.sourceTop = top; layer.sourceLeft = left; layer.sourceBottom = bottom; layer.sourceRight = right;

        quint16 channelCount = 0;
        if (!cursor.readU16(channelCount)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        if (channelCount > 56) {
            if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
            return false;
        }

        for (int ch = 0; ch < static_cast<int>(channelCount); ++ch) {
            qint16 id = 0;
            if (!cursor.readI16(id)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            quint64 chLenU64 = 0;
            if (isPSB) {
                if (!cursor.readU64(chLenU64)) {
                    if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                    return false;
                }
            } else {
                quint32 len32 = 0;
                if (!cursor.readU32(len32)) {
                    if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                    return false;
                }
                chLenU64 = len32;
            }
            int chLen = 0;
            if (!checkedLength(chLenU64, chLen)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
                return false;
            }
            layer.channels.append({static_cast<int>(id), chLen});
        }

        QString blendSig;
        if (!cursor.readString(4, blendSig) || blendSig != QStringLiteral("8BIM")) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }

        if (!cursor.readString(4, layer.blendKey)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }

        quint8 opVal = 0;
        quint8 clipVal = 0;
        quint8 flags = 0;
        if (!cursor.readU8(opVal) || !cursor.readU8(clipVal) || !cursor.readU8(flags) || !cursor.skip(1)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        layer.opacity = opVal;
        layer.clipping = (clipVal != 0);
        layer.hidden = ((flags & 2) != 0);

        quint32 extraLen = 0;
        if (!cursor.readU32(extraLen)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        int extraLenInt = 0;
        if (!checkedLength(extraLen, extraLenInt)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
            return false;
        }
        int extraEnd = 0;
        if (!checkedAdd(cursor.offset(), extraLenInt, extraEnd) || extraEnd > layerSectionEnd) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }

        quint32 maskLen = 0;
        if (!cursor.readU32(maskLen)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        int maskLenInt = 0;
        if (!checkedLength(maskLen, maskLenInt)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
            return false;
        }
        int maskEnd = 0;
        if (!checkedAdd(cursor.offset(), maskLenInt, maskEnd) || maskEnd > extraEnd) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        if (maskLen >= 20) {
            layer.hasMask = true;
            qint32 mTop = 0, mLeft = 0, mBottom = 0, mRight = 0;
            quint8 mDefault = 0, mFlags = 0;
            if (!cursor.readI32(mTop) || !cursor.readI32(mLeft) ||
                !cursor.readI32(mBottom) || !cursor.readI32(mRight) ||
                !cursor.readU8(mDefault) || !cursor.readU8(mFlags)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }

            // Reject inverted or unrepresentable mask bounds before cropping or decoding
            if (mTop == std::numeric_limits<qint32>::min() || mTop == std::numeric_limits<qint32>::max() ||
                mLeft == std::numeric_limits<qint32>::min() || mLeft == std::numeric_limits<qint32>::max() ||
                mBottom == std::numeric_limits<qint32>::min() || mBottom == std::numeric_limits<qint32>::max() ||
                mRight == std::numeric_limits<qint32>::min() || mRight == std::numeric_limits<qint32>::max() ||
                mTop < -MaxCoordinate || mTop > MaxCoordinate ||
                mLeft < -MaxCoordinate || mLeft > MaxCoordinate ||
                mBottom < -MaxCoordinate || mBottom > MaxCoordinate ||
                mRight < -MaxCoordinate || mRight > MaxCoordinate) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            const qint64 maskSpanW = static_cast<qint64>(mRight) - static_cast<qint64>(mLeft);
            const qint64 maskSpanH = static_cast<qint64>(mBottom) - static_cast<qint64>(mTop);
            if (maskSpanW < 0 || maskSpanH < 0) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            if (maskSpanW > MaxSide || maskSpanH > MaxSide) {
                if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
                return false;
            }

            layer.maskTop = mTop; layer.maskLeft = mLeft; layer.maskBottom = mBottom; layer.maskRight = mRight;
            layer.sourceMaskTop = mTop; layer.sourceMaskLeft = mLeft; layer.sourceMaskBottom = mBottom; layer.sourceMaskRight = mRight;
            layer.maskDefault = mDefault;
            layer.maskDisabled = ((mFlags & 2) != 0);
            layer.maskLinked = ((mFlags & 1) == 0);
            layer.maskFromRender = ((mFlags & 8) != 0);
        }
        if (!cursor.setOffset(maskEnd)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }

        quint32 rangesLen = 0;
        if (!cursor.readU32(rangesLen)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        int rangesLenInt = 0;
        if (!checkedLength(rangesLen, rangesLenInt) || !cursor.skip(rangesLenInt)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }

        quint8 nameLen = 0;
        if (!cursor.readU8(nameLen)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        QByteArray nameBytes;
        if (!cursor.readBytes(static_cast<int>(nameLen), nameBytes)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        layer.name = QString::fromUtf8(nameBytes);
        if (layer.name.isEmpty()) layer.name = QString::fromLatin1(nameBytes);
        if (layer.name.isEmpty()) layer.name = QStringLiteral("Layer");

        const int namePad = (4 - ((static_cast<int>(nameLen) + 1) % 4)) % 4;
        cursor.skip(namePad);

        while (cursor.offset() + 12 <= extraEnd) {
            QString sig;
            if (!cursor.readString(4, sig) || (sig != QStringLiteral("8BIM") && sig != QStringLiteral("8B64"))) break;
            QString key;
            if (!cursor.readString(4, key)) break;
            quint64 payloadLenU64 = 0;
            if (sig == QStringLiteral("8B64") || (isPSB && psbLargeAdditionalInfoKeys.contains(key))) {
                if (!cursor.readU64(payloadLenU64)) break;
            } else {
                quint32 len32 = 0;
                if (!cursor.readU32(len32)) break;
                payloadLenU64 = len32;
            }
            int payloadLen = 0;
            if (!checkedLength(payloadLenU64, payloadLen)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
                return false;
            }
            int payloadEnd = 0;
            if (!checkedAdd(cursor.offset(), payloadLen, payloadEnd) || payloadEnd > extraEnd) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            QByteArray payload;
            if (!cursor.readBytes(payloadLen, payload)) break;
            if (payloadLen % 2 == 1) cursor.skip(1);
            layer.extra[key] = payload;

            if (key == QStringLiteral("luni")) {
                const QString uni = unicodeNameFromData(payload);
                if (!uni.isEmpty()) layer.name = uni;
            }
            if (key == QStringLiteral("iOpa") && !payload.isEmpty()) {
                layer.fill = static_cast<quint8>(payload[0]);
            }
            if ((key == QStringLiteral("lsct") || key == QStringLiteral("lsdk")) && payload.size() >= 4) {
                layer.section = static_cast<int>(u32FromBytes(payload, 0));
            }
        }
        if (!cursor.setOffset(extraEnd)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
            return false;
        }
        raw.append(std::move(layer));
    }

    // Budget check and canvas cropping
    if (!fitsBudget(raw, remainingPixels)) {
        for (auto &layer : raw) {
            cropToCanvas(layer, canvasWidth, canvasHeight);
        }
        if (!fitsBudget(raw, remainingPixels)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
            return false;
        }
    }

    // Channel decoding
    const QSet<int> unpackedChannelIDs = {-1, 0, 1, 2, -2};
    qint64 usedPixels = 0;

    for (auto &layer : raw) {
        const qint64 spanW = static_cast<qint64>(layer.right) - static_cast<qint64>(layer.left);
        const qint64 spanH = static_cast<qint64>(layer.bottom) - static_cast<qint64>(layer.top);
        const qint64 maskSpanW = static_cast<qint64>(layer.maskRight) - static_cast<qint64>(layer.maskLeft);
        const qint64 maskSpanH = static_cast<qint64>(layer.maskBottom) - static_cast<qint64>(layer.maskTop);
        const int width = static_cast<int>(std::max(0LL, spanW));
        const int height = static_cast<int>(std::max(0LL, spanH));
        const int maskWidth = static_cast<int>(std::max(0LL, maskSpanW));
        const int maskHeight = static_cast<int>(std::max(0LL, maskSpanH));

        if (!fitsBudget(width, height, maskWidth, maskHeight, layer.hasMask, remainingPixels - usedPixels)) {
            if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
            return false;
        }

        const qint64 srcSpanW = static_cast<qint64>(layer.sourceRight) - static_cast<qint64>(layer.sourceLeft);
        const qint64 srcSpanH = static_cast<qint64>(layer.sourceBottom) - static_cast<qint64>(layer.sourceTop);
        const qint64 srcMaskSpanW = static_cast<qint64>(layer.sourceMaskRight) - static_cast<qint64>(layer.sourceMaskLeft);
        const qint64 srcMaskSpanH = static_cast<qint64>(layer.sourceMaskBottom) - static_cast<qint64>(layer.sourceMaskTop);
        const int sourceWidth = static_cast<int>(std::max(0LL, srcSpanW));
        const int sourceHeight = static_cast<int>(std::max(0LL, srcSpanH));
        const int sourceMaskWidth = static_cast<int>(std::max(0LL, srcMaskSpanW));
        const int sourceMaskHeight = static_cast<int>(std::max(0LL, srcMaskSpanH));

        QMap<int, QVector<quint8>> planes;

        for (const auto &channel : layer.channels) {
            const int start = cursor.offset();
            const int chLen = std::max(0, channel.second);
            int chEnd = 0;
            if (!checkedAdd(start, chLen, chEnd) || chEnd > cursor.size()) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            if (!unpackedChannelIDs.contains(channel.first) || chLen < 2) {
                if (!cursor.setOffset(chEnd)) {
                    if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                    return false;
                }
                continue;
            }
            quint16 compression = 0;
            if (!cursor.readU16(compression)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            QByteArray payload;
            if (!cursor.readBytes(chLen - 2, payload)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            const bool isMask = (channel.first == -2);
            const int srcW = isMask ? sourceMaskWidth : sourceWidth;
            const int srcH = isMask ? sourceMaskHeight : sourceHeight;
            const int targetW = isMask ? maskWidth : width;
            const int targetH = isMask ? maskHeight : height;
            const auto &crop = isMask ? layer.maskCrop : layer.imageCrop;

            if (targetW > 0 && targetH > 0) {
                QVector<quint8> plane;
                if (!decodeChannelPlane(static_cast<int>(compression), srcW, srcH, payload, isPSB, crop, plane, error)) {
                    return false;
                }
                planes[channel.first] = std::move(plane);
            }
            if (!cursor.setOffset(chEnd)) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
        }

        if (layer.hasMask && maskWidth > 0 && maskHeight > 0 && planes.contains(-2) &&
            planes[-2].size() >= static_cast<qint64>(maskWidth) * maskHeight) {
            layer.maskImage = makeMaskImage(maskWidth, maskHeight, planes[-2]);
            if (!layer.maskImage.isNull()) {
                usedPixels += (static_cast<qint64>(maskWidth) * maskHeight);
            }
        }

        if (width > 0 && height > 0) {
            const qint64 total64 = static_cast<qint64>(width) * height;
            if (total64 > INT_MAX) {
                if (error) *error = psdErrorMessage(PSDErrorCode::TooLarge);
                return false;
            }
            const int total = static_cast<int>(total64);
            const QVector<quint8> black(total, 0);
            const QVector<quint8> opaque(total, 255);

            const QVector<quint8> &red = planes.contains(0) ? planes[0] : black;
            const QVector<quint8> &green = planes.contains(1) ? planes[1] : black;
            const QVector<quint8> &blue = planes.contains(2) ? planes[2] : black;
            const QVector<quint8> &alpha = planes.contains(-1) ? planes[-1] : opaque;

            if (red.size() < total || green.size() < total || blue.size() < total || alpha.size() < total) {
                if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
                return false;
            }
            layer.image = makeRgbaImage(width, height, red, green, blue, alpha);
            usedPixels += total;
        }
    }

    if (!cursor.setOffset(layerSectionEnd)) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }

    // Assembly
    QVector<QUuid> groups;
    struct LayerEntry {
        Layer layer;
        RawLayer raw;
        PSDLayerKind kind;
    };
    QVector<LayerEntry> entries;

    for (const auto &rawLayer : raw) {
        if (rawLayer.section == 3) {
            // Group closing divider (bottom)
            groups.push_back(QUuid::createUuid());
            continue;
        }

        const bool isGroup = (rawLayer.section == 1 || rawLayer.section == 2);
        const QUuid id = isGroup ? (groups.isEmpty() ? QUuid::createUuid() : groups.takeLast()) : QUuid::createUuid();
        const std::optional<QUuid> parentId = groups.isEmpty() ? std::nullopt : std::make_optional(groups.last());

        Layer layer;
        layer.id = id;
        layer.name = rawLayer.name.isEmpty() ? QStringLiteral("Layer") : rawLayer.name;
        layer.parentId = parentId;
        layer.group = isGroup;
        layer.visible = !rawLayer.hidden;

        const PSDLayerKind kind = layerKind(rawLayer, isGroup);

        if (rawLayer.cropped) {
            conversions.append(PSDConversion{
                QUuid::createUuid(), layer.name,
                QStringLiteral("Cropped to the canvas so the file fits in memory. Pixels outside the canvas weren't imported.")
            });
        }

        if (kind == PSDLayerKind::Text) {
            conversions.append(PSDConversion{
                QUuid::createUuid(), layer.name,
                QStringLiteral("Text layer was rasterized to pixels.")
            });
        } else if (kind == PSDLayerKind::Vector) {
            conversions.append(PSDConversion{
                QUuid::createUuid(), layer.name,
                QStringLiteral("Vector shape was rasterized to pixels.")
            });
        } else if (kind == PSDLayerKind::SmartObject) {
            conversions.append(PSDConversion{
                QUuid::createUuid(), layer.name,
                QStringLiteral("The smart object was rasterized. Linked contents can’t be edited.")
            });
        } else if (kind == PSDLayerKind::Effects) {
            conversions.append(PSDConversion{
                QUuid::createUuid(), layer.name,
                QStringLiteral("Layer effects were discarded, so the appearance may differ.")
            });
        } else if (kind == PSDLayerKind::Other) {
            conversions.append(PSDConversion{
                QUuid::createUuid(), layer.name,
                QStringLiteral("This Photoshop layer type isn’t supported and was imported as pixels.")
            });
        }

        if (isGroup) {
            if (rawLayer.blendKey != QStringLiteral("pass") && rawLayer.blendKey != QStringLiteral("norm")) {
                conversions.append(PSDConversion{
                    QUuid::createUuid(), layer.name,
                    QStringLiteral("Folder blend mode “%1” isn’t supported. The folder will be pass-through.").arg(rawLayer.blendKey)
                });
            }
            layer.blendMode = BlendMode::Normal;
        } else {
            const auto optBlend = blendModeFromPSD(rawLayer.blendKey);
            if (optBlend) {
                layer.blendMode = *optBlend;
            } else {
                if (rawLayer.blendKey != QStringLiteral("pass")) {
                    conversions.append(PSDConversion{
                        QUuid::createUuid(), layer.name,
                        QStringLiteral("Blend mode “%1” isn’t supported and will be applied as Normal.").arg(rawLayer.blendKey.trimmed())
                    });
                }
                layer.blendMode = BlendMode::Normal;
            }
        }

        const bool hasEffects = (kind == PSDLayerKind::Effects ||
                                 rawLayer.extra.contains(QStringLiteral("lfx2")) ||
                                 rawLayer.extra.contains(QStringLiteral("lrFX")) ||
                                 rawLayer.extra.contains(QStringLiteral("lmfx")));
        if (hasEffects && rawLayer.fill != 255) {
            layer.opacity = std::clamp(double(rawLayer.opacity) / 255.0, 0.0, 1.0);
        } else {
            layer.opacity = std::clamp((double(rawLayer.opacity) / 255.0) * (double(rawLayer.fill) / 255.0), 0.0, 1.0);
        }

        // Folders and adjustment layers cover the canvas, whatever bounds their record carries (as mac's builder does).
        if (isGroup || kind == PSDLayerKind::Adjustment) {
            layer.transform.origin = QPointF(0, 0);
            layer.transform.size = QSizeF(canvasWidth, canvasHeight);
        } else {
            const qint64 spanW = static_cast<qint64>(rawLayer.right) - static_cast<qint64>(rawLayer.left);
            const qint64 spanH = static_cast<qint64>(rawLayer.bottom) - static_cast<qint64>(rawLayer.top);
            const int w = static_cast<int>(std::max(0LL, spanW));
            const int h = static_cast<int>(std::max(0LL, spanH));
            layer.transform.origin = QPointF(rawLayer.left, rawLayer.top);
            layer.transform.size = QSizeF(w, h);
            layer.image = rawLayer.image;
        }

        if (!rawLayer.maskFromRender && !rawLayer.maskImage.isNull()) {
            const bool covers = isGroup || layer.image.isNull();
            layer.mask = PSDReader::maskOnLayerGrid(
                rawLayer.maskImage,
                QRectF(rawLayer.maskLeft, rawLayer.maskTop, double(rawLayer.maskRight) - rawLayer.maskLeft, double(rawLayer.maskBottom) - rawLayer.maskTop),
                rawLayer.maskDefault, QRectF(layer.transform.origin, layer.transform.size),
                covers ? QSize(canvasWidth, canvasHeight) : layer.image.size());
            layer.maskEnabled = !rawLayer.maskDisabled;
            layer.maskLinked = rawLayer.maskLinked;
        } else if (rawLayer.hasMask && rawLayer.maskImage.isNull()) {
            conversions.append(PSDConversion{
                QUuid::createUuid(), layer.name,
                QStringLiteral("The layer mask couldn’t be converted to 8-bit grayscale and was skipped.")
            });
        }

        if (!isGroup && kind == PSDLayerKind::Adjustment) {
            const auto adj = parseAdjustment(rawLayer.extra);
            if (adj) {
                layer.adjustment = *adj;
                conversions.append(PSDConversion{
                    QUuid::createUuid(), layer.name,
                    QStringLiteral("Adjustment parameters may not match Photoshop exactly.")
                });
            } else {
                conversions.append(PSDConversion{
                    QUuid::createUuid(), layer.name,
                    QStringLiteral("This adjustment type isn’t supported and was skipped.")
                });
                continue; // Skip unsupported adjustment layer
            }
        }

        entries.append(LayerEntry{std::move(layer), rawLayer, kind});
    }

    if (!groups.isEmpty()) {
        if (error) *error = psdErrorMessage(PSDErrorCode::Truncated);
        return false;
    }

    QVector<Layer> layers;
    layers.reserve(entries.size());
    for (auto &entry : entries) {
        layers.append(std::move(entry.layer));
    }

    // Clipping mask resolution
    QMap<QUuid, int> idToIndex;
    for (int i = 0; i < layers.size(); ++i) {
        idToIndex[layers[i].id] = i;
    }

    QMap<std::optional<QUuid>, QUuid> baseForParent;
    for (int i = 0; i < entries.size(); ++i) {
        const auto &rawLayer = entries[i].raw;
        const auto &layer = layers[i];
        if (rawLayer.clipping) {
            if (baseForParent.contains(layer.parentId)) {
                const QUuid sourceId = baseForParent[layer.parentId];
                const int srcIdx = idToIndex.value(sourceId, -1);
                if (srcIdx >= 0 && !layers[srcIdx].group && layers[srcIdx].adjustment.isEmpty()) {
                    layers[i].maskSourceId = sourceId;
                } else {
                    conversions.append(PSDConversion{
                        QUuid::createUuid(), layer.name,
                        QStringLiteral("This clipping mask’s base isn’t supported, so clipping was skipped.")
                    });
                }
            } else {
                conversions.append(PSDConversion{
                    QUuid::createUuid(), layer.name,
                    QStringLiteral("This clipping mask’s base isn’t supported, so clipping was skipped.")
                });
            }
        } else if (!layer.group && layer.adjustment.isEmpty()) {
            baseForParent[layer.parentId] = layer.id;
        } else {
            baseForParent.remove(layer.parentId);
        }
    }

    tempDoc.layers = std::move(layers);
    if (!tempDoc.layers.isEmpty()) {
        QUuid activeId;
        for (int i = tempDoc.layers.size() - 1; i >= 0; --i) {
            if (!tempDoc.layers[i].parentId) {
                activeId = tempDoc.layers[i].id;
                break;
            }
        }
        if (activeId.isNull()) activeId = tempDoc.layers.last().id;
        tempDoc.activeLayerId = activeId;
    }

    result.document = std::move(tempDoc);
    result.conversions = std::move(conversions);
    return true;
}

} // namespace compositor
