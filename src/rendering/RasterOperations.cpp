#include "rendering/RasterOperations.h"
#include "rendering/GpuOperations.h"
#include "rendering/RgbSplit.h"
#include "core/CameraRawTables.h"
#include "rendering/DeepImage.h"
#include "PatchPixels.h"
#include "DeepPixels.h"
#include "core/CameraRaw.h"

extern "C" {
#include "LensPixels.h"
#include "NoisePixels.h"
#include "WandPixels.h"

#include <cstring>
#include "LevelsPixels.h"
#include "AdjustPixels.h"
#include "ContentFill.h"
#include "HealPixels.h"
}

#include <algorithm>
#include <QJsonArray>
#include <QtConcurrent/QtConcurrentMap>
#include <vector>
#include <functional>
#include <cmath>

namespace compositor {

// Runs `rows(first, end)` over bands of rows on the global thread pool (the calling thread works too). Every pixel is
// computed exactly as in a serial loop, so results are identical.
static void parallelRows(int height, const std::function<void(int, int)> &rows)
{
    constexpr int band = 32;
    if (height <= band) { rows(0, height); return; }
    std::vector<int> starts;
    for (int y = 0; y < height; y += band) starts.push_back(y);
    QtConcurrent::blockingMap(starts, [&](int y) { rows(y, std::min(height, y + band)); });
}

namespace {
// Photoshop's Saturation (mac fe7a83d): below 0 it scales toward gray (-100 is gray); above 0 it divides by what is
// left, so +50 doubles it and +100 takes any color all the way. Multiplicative both ways, so neutral grays stay neutral.
double adjustedSaturation(double saturation, double amount)
{
    amount = std::clamp(amount / 100.0, -1.0, 1.0);
    if (amount <= 0.0) return std::max(0.0, saturation * (1.0 + amount));
    if (amount >= 1.0) return saturation > 0.0 ? 1.0 : 0.0;
    return std::min(1.0, saturation / (1.0 - amount));
}
double wrapHue(double value)
{
    value = std::fmod(value, 360.0);
    return value < 0 ? value + 360 : value;
}
}

double HueBand::forward(double from, double to) { return wrapHue(to - from); }

double HueBand::weight(double hue) const
{
    const double span = forward(falloffStart, falloffEnd);
    if (span <= 0) return 1;
    const double position = forward(falloffStart, hue);
    if (position > span) return 0;
    const double rampIn = forward(falloffStart, rangeStart);
    const double plateauEnd = forward(falloffStart, rangeEnd);
    if (position < rampIn) return rampIn > 0 ? position / rampIn : 1;
    if (position <= plateauEnd) return 1;
    const double rampOut = span - plateauEnd;
    return rampOut > 0 ? (span - position) / rampOut : 1;
}

HueBand HueBand::defaultFor(ColorRange range)
{
    switch (range) {
    case ColorRange::Master: return {0, 0, 360, 360};
    case ColorRange::Reds: return {315, 345, 15, 45};
    case ColorRange::Yellows: return {15, 45, 75, 105};
    case ColorRange::Greens: return {75, 105, 135, 165};
    case ColorRange::Cyans: return {135, 165, 195, 225};
    case ColorRange::Blues: return {195, 225, 255, 285};
    case ColorRange::Magentas: return {255, 285, 315, 345};
    case ColorRange::Count: break;
    }
    return {};
}

HueBand HueBand::centered(double hue) const
{
    const double core = forward(rangeStart, rangeEnd), leading = forward(falloffStart, rangeStart), trailing = forward(rangeEnd, falloffEnd);
    const double start = wrapHue(hue - core / 2);
    return {wrapHue(start - leading), start, wrapHue(start + core), wrapHue(start + core + trailing)};
}

void HueBand::include(double hue)
{
    hue = wrapHue(hue); if (weight(hue) >= 1) return;
    const double shoulderIn = forward(falloffStart, rangeStart), shoulderOut = forward(rangeEnd, falloffEnd);
    if (forward(hue, rangeStart) <= forward(rangeEnd, hue)) { rangeStart = hue; falloffStart = wrapHue(hue - shoulderIn); }
    else { rangeEnd = hue; falloffEnd = wrapHue(hue + shoulderOut); }
}

void HueBand::exclude(double hue)
{
    hue = wrapHue(hue); if (weight(hue) <= 0) return;
    const double shoulderIn = forward(falloffStart, rangeStart), shoulderOut = forward(rangeEnd, falloffEnd);
    if (forward(falloffStart, hue) <= forward(hue, falloffEnd)) { falloffStart = wrapHue(hue + 1); rangeStart = wrapHue(hue + 1 + shoulderIn); }
    else { falloffEnd = wrapHue(hue - 1); rangeEnd = wrapHue(hue - 1 - shoulderOut); }
}

bool HueBand::setHandle(int index, double degrees)
{
    HueBand changed = *this; const double value = wrapHue(degrees);
    if (index == 0) changed.falloffStart = value; else if (index == 1) changed.rangeStart = value;
    else if (index == 2) changed.rangeEnd = value; else changed.falloffEnd = value;
    const double span = forward(changed.falloffStart, changed.falloffEnd);
    const double start = forward(changed.falloffStart, changed.rangeStart), end = forward(changed.falloffStart, changed.rangeEnd);
    if (span <= 1 || span > 350 || start > end || end > span) return false;
    *this = changed; return true;
}

HueSaturationSettings::HueSaturationSettings()
{
    for (size_t i = 0; i < bands.size(); ++i) bands[i] = HueBand::defaultFor(ColorRange(i));
}

HueSaturationSettings::HueSaturationSettings(double hue, double saturation, double lightness, bool colorizeValue, ColorRange selected)
    : HueSaturationSettings()
{
    range = selected; colorize = colorizeValue; adjustments[size_t(selected)] = {hue, saturation, lightness};
}

bool HueSaturationSettings::isIdentity() const
{
    return !colorize && std::all_of(adjustments.cbegin(), adjustments.cend(), [](const RangeAdjustment &a) { return a == RangeAdjustment{}; });
}

double HueSaturationSettings::weight(ColorRange colorRange, double hue) const
{
    if (colorRange == ColorRange::Master) return 1;
    double value = bands[size_t(colorRange)].weight(hue);
    if (invertRange && colorRange == range) value = 1 - value;
    return value;
}

CurvesSettings::CurvesSettings()
{
    for (auto &points : channels) points = {{0, 0}, {255, 255}};
}

bool CurvesSettings::isValid() const
{
    if (channel < 0 || channel >= 4) return false;
    for (const auto &points : channels) {
        if (points.size() < 2 || points.size() > 32 || points.constFirst().x != 0 || points.constLast().x != 255) return false;
        for (int i = 0; i < points.size(); ++i) {
            if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y) || points[i].x < 0 || points[i].x > 255 || points[i].y < 0 || points[i].y > 255) return false;
            if (i && points[i - 1].x >= points[i].x) return false;
        }
    }
    return true;
}

double CurvesSettings::value(double x, int channelIndex) const
{
    if (channelIndex < 0 || channelIndex >= 4 || !isValid()) return std::clamp(x, 0.0, 255.0);
    const auto &points = channels[size_t(channelIndex)];
    int index = 0; while (index + 2 < points.size() && points[index + 1].x <= x) ++index;
    QVector<double> differences; differences.reserve(points.size() - 1);
    for (int i = 0; i + 1 < points.size(); ++i) differences.push_back((points[i + 1].y - points[i].y) / (points[i + 1].x - points[i].x));
    const auto slope = [&](int i) {
        if (i == 0) return differences.constFirst();
        if (i == points.size() - 1) return differences.constLast();
        if (differences[i - 1] * differences[i] <= 0) return 0.0;
        return 2 / (1 / differences[i - 1] + 1 / differences[i]);
    };
    const double h = points[index + 1].x - points[index].x;
    const double t = std::clamp((x - points[index].x) / h, 0.0, 1.0);
    const double y = (2*t*t*t - 3*t*t + 1) * points[index].y + (t*t*t - 2*t*t + t) * h * slope(index)
        + (-2*t*t*t + 3*t*t) * points[index + 1].y + (t*t*t - t*t) * h * slope(index + 1);
    return std::clamp(y, 0.0, 255.0);
}

std::optional<QImage> RasterOperations::magicWandMask(const QImage &image, const QPoint &seed,
                                                       int tolerance, int radius, bool contiguous)
{
    if (image.isNull() || !QRect(QPoint(), image.size()).contains(seed)) return std::nullopt;
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QImage mask(source.size(), QImage::Format_Grayscale8);
    mask.fill(0);
    std::vector<uint8_t> packed(size_t(source.width()) * size_t(source.height()));
    const long count = wand_mask(source.constBits(), size_t(source.width()), size_t(source.height()),
                                 size_t(source.bytesPerLine()), size_t(seed.x()), size_t(seed.y()),
                                 size_t(std::clamp(radius, 0, 2)), std::clamp(tolerance, 0, 255),
                                 contiguous ? 1 : 0, packed.data());
    if (count < 0) return std::nullopt;
    for (int y = 0; y < mask.height(); ++y)
        std::copy_n(packed.data() + size_t(y) * size_t(mask.width()), size_t(mask.width()), mask.scanLine(y));
    return mask;
}

QImage RasterOperations::inverted(const QImage &image)
{
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return inverted(low); });
    if (auto gpu = GpuOperations::simple(image.convertToFormat(QImage::Format_RGBA8888_Premultiplied),GpuOp::Invert); !gpu.isNull()) return gpu;
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < result.height(); ++y) {
        uchar *p = result.scanLine(y);
        for (int x = 0; x < result.width(); ++x, p += 4) {
            const int alpha = p[3];
            p[0] = uchar(alpha - p[0]); p[1] = uchar(alpha - p[1]); p[2] = uchar(alpha - p[2]);
        }
    }
    return result;
}

QImage RasterOperations::addNoise(const QImage &image, float amount, bool gaussian, bool monochromatic, quint32 seed)
{
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return addNoise(low, amount, gaussian, monochromatic, seed); });
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    noise_add(result.bits(), size_t(result.width()), size_t(result.height()), size_t(result.bytesPerLine()),
              amount, gaussian ? 1 : 0, monochromatic ? 1 : 0, seed);
    return result;
}

QImage RasterOperations::lensDistorted(const QImage &image, double amount)
{
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return lensDistorted(low, amount); });
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QImage result(source.size(), source.format());
    result.fill(Qt::transparent);
    lens_distort(source.constBits(), result.bits(), size_t(source.width()), size_t(source.height()),
                 size_t(source.bytesPerLine()), amount);
    return result;
}

static double applyLevel(double input, const LevelRange &raw)
{
    const LevelRange value = raw.normalized();
    const double clipped = std::clamp((input * 255 - value.black) / (value.white - value.black), 0.0, 1.0);
    return (value.outputBlack + std::pow(clipped, 1 / value.gamma) * (value.outputWhite - value.outputBlack)) / 255;
}

std::array<float, 768> RasterOperations::levelsTables(const LevelsSettings &settings)
{
    std::array<float, 3 * 256> tables{};
    for (int channel = 0; channel < 3; ++channel) for (int value = 0; value < 256; ++value)
        tables[channel * 256 + value] = float(applyLevel(applyLevel(value / 255.0, settings.ranges[channel + 1]), settings.ranges[0]));
    return tables;
}

std::array<float, 768> RasterOperations::curvesTables(const CurvesSettings &settings)
{
    std::array<float, 3 * 256> tables{};
    for (int channel = 0; channel < 3; ++channel) for (int input = 0; input < 256; ++input)
        tables[size_t(channel * 256 + input)] = float(settings.value(settings.value(input, channel + 1), 0) / 255);
    return tables;
}

LevelsSettings RasterOperations::levelsFromJson(const QJsonObject &settings)
{
    LevelsSettings value; const QJsonArray ranges = settings.value(QStringLiteral("levels")).toObject().value(QStringLiteral("ranges")).toArray();
    for (int i = 0; i < std::min(4, int(ranges.size())); ++i) { const QJsonObject r = ranges[i].toObject(); value.ranges[size_t(i)] = {
        r.value(QStringLiteral("black")).toDouble(), r.value(QStringLiteral("gamma")).toDouble(1), r.value(QStringLiteral("white")).toDouble(255),
        r.value(QStringLiteral("outputBlack")).toDouble(), r.value(QStringLiteral("outputWhite")).toDouble(255)}; }
    return value;
}

CurvesSettings RasterOperations::curvesFromJson(const QJsonObject &settings)
{
    CurvesSettings value; const QJsonArray channels = settings.value(QStringLiteral("curves")).toObject().value(QStringLiteral("channels")).toArray();
    for (int c = 0; c < std::min(4, int(channels.size())); ++c) { QVector<CurvePoint> points; for (const QJsonValue &entry : channels[c].toArray()) {
        const QJsonObject point = entry.toObject(); points.push_back({point.value(QStringLiteral("x")).toDouble(), point.value(QStringLiteral("y")).toDouble()}); }
        if (!points.isEmpty()) value.channels[size_t(c)] = points;
    }
    return value;
}

RasterOperations::ExposureSettings RasterOperations::exposureFromJson(const QJsonObject &settings)
{
    QJsonObject value = settings.value(QStringLiteral("exposureSettings")).toObject(); if (value.isEmpty()) value = settings.value(QStringLiteral("exposure")).toObject();
    return {value.value(QStringLiteral("exposure")).toDouble(), value.value(QStringLiteral("offset")).toDouble(), value.value(QStringLiteral("gamma")).toDouble(1)};
}

QImage RasterOperations::levels(const QImage &image, const LevelsSettings &settings)
{
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return levels(low, settings); });
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const auto tables = levelsTables(settings);
    if (auto gpu = GpuOperations::lookup(result,tables.data()); !gpu.isNull()) return gpu;
    levels_apply(result.bits(), size_t(result.width()) * result.height(), tables.data());
    return result;
}

LevelsHistogram RasterOperations::levelsHistogram(const QImage &image, const QImage &coverage)
{
    LevelsHistogram bins{}; const QImage source=image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage mask=coverage.isNull()?QImage():coverage.convertToFormat(QImage::Format_Grayscale8);
    if(!mask.isNull()&&mask.size()!=source.size())return bins;
    for(int y=0;y<source.height();++y){const uchar *row=source.constScanLine(y),*clip=mask.isNull()?nullptr:mask.constScanLine(y);for(int x=0;x<source.width();++x){const uchar *p=row+x*4;if(!p[3])continue;const double weight=p[3]/255.0*(clip?clip[x]/255.0:1.0);if(weight<=0)continue;int values[3];for(int c=0;c<3;++c)values[c]=std::clamp((int(p[c])*255+p[3]/2)/p[3],0,255);for(int c=0;c<3;++c)bins[size_t(c+1)][size_t(values[c])]+=weight;for(int c=0;c<3;++c)bins[0][size_t(values[c])]+=weight/3.0;}}
    return bins;
}

LevelsSettings RasterOperations::automaticLevels(const LevelsHistogram &histogram, int mode)
{
    LevelsSettings result;const auto endpoints=[](const std::array<double,256>&bins)->std::optional<QPair<double,double>>{double total=0;for(double v:bins)total+=v;if(total<=0)return std::nullopt;double sum=0;int low=0,high=255;for(int i=0;i<256;++i){sum+=bins[size_t(i)];if(sum>total*.001){low=i;break;}}sum=0;for(int i=255;i>=0;--i){sum+=bins[size_t(i)];if(sum>total*.001){high=i;break;}}return low<high?std::optional<QPair<double,double>>(qMakePair(double(low),double(high))):std::nullopt;};
    if(mode==0){double low=255,high=0;bool found=false;for(int c=1;c<=3;++c)if(const auto e=endpoints(histogram[size_t(c)])){low=std::min(low,e->first);high=std::max(high,e->second);found=true;}if(found&&low<high)result.ranges[0]={low,1,high,0,255};}
    else for(int c=1;c<=3;++c)if(const auto e=endpoints(histogram[size_t(c)])){LevelRange range{e->first,1,e->second,0,255};if(mode==2){double total=0,mean=0;for(int i=0;i<256;++i){const double w=histogram[size_t(c)][size_t(i)];total+=w;mean+=applyLevel(i/255.0,range)*w;}if(total>0){mean/=total;if(mean>0&&mean<1)range.gamma=std::clamp(std::log(mean)/std::log(.5),.1,9.99);}}result.ranges[size_t(c)]=range;}
    return result;
}

LevelsSettings RasterOperations::sampledLevels(const LevelsSettings &settings, const QColor &sample, int mode)
{
    LevelsSettings result = settings;
    if (!sample.isValid() || sample.alpha() == 0 || mode < 0 || mode > 2) return result;
    result.ranges[0] = LevelRange{};
    const std::array<double, 3> rgb{sample.redF(), sample.greenF(), sample.blueF()};
    for (int channel = 1; channel <= 3; ++channel) {
        LevelRange range = result.ranges[size_t(channel)];
        const double value = rgb[size_t(channel - 1)] * 255;
        if (mode == 0) range.black = std::min(range.white - 1, std::max(0.0, value));
        else if (mode == 2) range.white = std::max(range.black + 1, std::min(255.0, value));
        else {
            const double fraction = (value - range.black) / (range.white - range.black);
            if (fraction > 0 && fraction < 1) range.gamma = std::log(fraction) / std::log(.5);
        }
        range.outputBlack = 0; range.outputWhite = 255;
        result.ranges[size_t(channel)] = range.normalized();
    }
    return result;
}

QImage RasterOperations::exposure(const QImage &image, double stops, double offset, double gamma)
{
    if(DeepImage::isHDR(image)) {
        QImage out=DeepImage::canonical(image);deep_hdr_exposure(reinterpret_cast<uint16_t *>(out.bits()),out.bytesPerLine(),out.width(),out.height(),float(std::pow(2.0,stops)),float(offset),float(1/gamma));return out;
    }
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return exposure(low, stops, offset, gamma); });
    if (!std::isfinite(stops) || !std::isfinite(offset) || !std::isfinite(gamma)) return image;
    stops = std::clamp(stops, -20.0, 20.0); offset = std::clamp(offset, -.5, .5); gamma = std::clamp(gamma, .01, 9.99);
    const double scale = std::pow(2, stops);
    GpuParameters gpuParams{};gpuParams[GpuParam::Exposure::multiplier]=scale;gpuParams[GpuParam::Exposure::offset]=offset;gpuParams[GpuParam::Exposure::inverseGamma]=1/gamma;
    if (auto gpu = GpuOperations::simple(image.convertToFormat(QImage::Format_RGBA8888_Premultiplied),GpuOp::Exposure,gpuParams); !gpu.isNull()) return gpu;
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const auto channel = [&](int value, int alpha) {
        double encoded = std::clamp(value * 255.0 / alpha, 0.0, 255.0) / 255;
        double linear = encoded <= .04045 ? encoded / 12.92 : std::pow((encoded + .055) / 1.055, 2.4);
        linear = std::pow(std::max(0.0, linear * scale + offset), 1 / gamma);
        const double output = linear <= .0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1 / 2.4) - .055;
        return uchar(std::clamp(qRound(output * alpha), 0, alpha));
    };
    // The result depends only on (value, alpha): above 65536 channels a table of every pair is cheaper than three
    // pow() calls per channel, and it holds exactly the values the per-pixel formula gives.
    std::vector<uchar> table;
    if (qint64(result.width()) * result.height() * 3 > 65536) {
        table.resize(256 * 256);
        for (int alpha = 1; alpha < 256; ++alpha) for (int value = 0; value < 256; ++value) table[size_t(alpha) * 256 + value] = channel(value, alpha);
    }
    result.detach(); // Rows are written from several threads.
    parallelRows(result.height(), [&](int first, int end) {
        for (int y = first; y < end; ++y) { uchar *row = result.scanLine(y); for (int x = 0; x < result.width(); ++x) {
            uchar *pixel = row + x * 4; const int alpha = pixel[3]; if (!alpha) continue;
            for (int c = 0; c < 3; ++c) pixel[c] = table.empty() ? channel(pixel[c], alpha) : table[size_t(alpha) * 256 + pixel[c]];
        } }
    });
    return result;
}

QImage RasterOperations::colorRangeMask(const QImage &image, const QVector<quint8> &include, const QVector<quint8> &exclude,
                                        int fuzziness, bool invert)
{
    QImage mask(image.size(), QImage::Format_Grayscale8);
    mask.fill(0);
    if (image.isNull() || include.size() < 3) return mask;
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    std::vector<uint8_t> packed(size_t(source.width()) * size_t(source.height()));
    color_range_mask(source.constBits(), size_t(source.width()), size_t(source.height()), size_t(source.bytesPerLine()),
                     include.constData(), int(include.size() / 3), exclude.constData(), int(exclude.size() / 3),
                     std::clamp(fuzziness, 0, 200), invert ? 1 : 0, packed.data());
    for (int y = 0; y < mask.height(); ++y) std::memcpy(mask.scanLine(y), packed.data() + size_t(y) * size_t(mask.width()), size_t(mask.width()));
    return mask;
}

std::optional<std::array<quint8, 3>> RasterOperations::colorRangeSample(const QImage &image, const QPoint &point)
{
    if (image.isNull() || !QRect(QPoint(), image.size()).contains(point)) return std::nullopt;
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    long sums[4] = {0, 0, 0, 0};
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        const int x = point.x() + dx, y = point.y() + dy;
        if (x < 0 || y < 0 || x >= source.width() || y >= source.height()) continue;   // outside counts as transparent
        const uchar *p = source.constScanLine(y) + x * 4;
        for (int c = 0; c < 4; ++c) sums[c] += p[c];
    }
    if (sums[3] <= 0) return std::nullopt;
    std::array<quint8, 3> color{};
    for (int c = 0; c < 3; ++c) color[size_t(c)] = quint8(std::min<long>(255, (sums[c] * 255 + sums[3] / 2) / sums[3]));
    return color;
}

std::array<std::array<double, 3>, 361> RasterOperations::hueSaturationResponses(const HueSaturationSettings &settings)
{
    std::array<std::array<double, 3>, 361> responses{};
    for (int degree = 0; degree <= 360; ++degree) for (size_t i = 0; i < settings.adjustments.size(); ++i) {
        const auto &adjustment = settings.adjustments[i]; const double weight = settings.weight(ColorRange(i), degree);
        responses[size_t(degree)][0] += adjustment.hue * weight;
        responses[size_t(degree)][1] += adjustment.saturation * weight;
        responses[size_t(degree)][2] += adjustment.lightness * weight;
    }
    return responses;
}

QImage RasterOperations::hueSaturation(const QImage &image, const HueSaturationSettings &settings)
{
    if (DeepImage::isDeep(image)) {
        // Building the deep lookup cube requires QColor's exact HSL quantization.
        GpuRenderer::CpuScope cpu;
        return DeepImage::lookup(image, [&](const QImage &low) { return hueSaturation(low, settings); });
    }
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (settings.isIdentity()) return result;
    struct Response { double hue = 0, saturation = 0, lightness = 0; };
    std::array<Response, 361> responses{};
    const auto changes = hueSaturationResponses(settings);
    for (int degree = 0; degree <= 360; ++degree)
        responses[size_t(degree)] = {changes[size_t(degree)][0], changes[size_t(degree)][1], changes[size_t(degree)][2]};
    if (GpuRenderer::shouldAttempt(result.size(), 16)) {
        GpuGraph graph;
        const auto gpu = graph.execute(GpuOperations::appendHueSaturation(graph, graph.source(result), settings, 1), result.format());
        if (!gpu.isNull()) return gpu;
    }
    result.detach(); // Rows are written from several threads.
    parallelRows(result.height(), [&](int first, int end) {
    for (int y = first; y < end; ++y) {
        uchar *row = result.scanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            uchar *p = row + x * 4; const int alpha = p[3]; if (!alpha) continue;
            const QColor source = QColor::fromRgbF(std::clamp(p[0] / double(alpha), 0.0, 1.0), std::clamp(p[1] / double(alpha), 0.0, 1.0), std::clamp(p[2] / double(alpha), 0.0, 1.0));
            float hue = 0, saturation = 0, lightness = 0; source.getHslF(&hue, &saturation, &lightness);
            double hueDegrees = hue < 0 ? 0 : hue * 360, lightnessAmount = 0;
            if (settings.colorize) {
                const auto &master = settings.adjustments[size_t(settings.range)];
                hueDegrees = wrapHue(master.hue); saturation = std::clamp(master.saturation / 100, 0.0, 1.0); lightnessAmount = master.lightness / 100;
            } else {
                const Response &response = responses[size_t(std::clamp(qRound(hueDegrees), 0, 360))];
                hueDegrees = wrapHue(hueDegrees + response.hue);
                saturation = adjustedSaturation(saturation, response.saturation);
                lightnessAmount = response.lightness / 100;
            }
            lightnessAmount = std::clamp(lightnessAmount, -1.0, 1.0);
            lightness = lightnessAmount >= 0 ? lightness + (1 - lightness) * lightnessAmount : lightness * (1 + lightnessAmount);
            const QColor adjusted = QColor::fromHslF(hueDegrees / 360, saturation, std::clamp(double(lightness), 0.0, 1.0));
            p[0] = uchar(std::clamp(qRound(adjusted.redF() * alpha), 0, alpha));
            p[1] = uchar(std::clamp(qRound(adjusted.greenF() * alpha), 0, alpha));
            p[2] = uchar(std::clamp(qRound(adjusted.blueF() * alpha), 0, alpha));
        }
    }
    });
    return result;
}

QImage RasterOperations::curves(const QImage &image, const CurvesSettings &settings)
{
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return curves(low, settings); });
    if (!settings.isValid()) return image;
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const auto tables = curvesTables(settings);
    if (auto gpu = GpuOperations::lookup(result,tables.data()); !gpu.isNull()) return gpu;
    levels_apply(result.bits(), size_t(result.width()) * result.height(), tables.data());
    return result;
}

HueSaturationSettings RasterOperations::hueSaturationFromJson(const QJsonObject &settings)
{
    const QJsonObject hsv = settings.value(QStringLiteral("hsvSettings")).toObject();
    HueSaturationSettings value(hsv.value(QStringLiteral("hue")).toDouble(settings.value(QStringLiteral("hue")).toDouble()),
        hsv.value(QStringLiteral("saturation")).toDouble(settings.value(QStringLiteral("saturation")).toDouble()),
        hsv.value(QStringLiteral("lightness")).toDouble(settings.value(QStringLiteral("lightness")).toDouble()),
        hsv.value(QStringLiteral("colorize")).toBool(settings.value(QStringLiteral("colorize")).toBool()));
    static const QStringList names{QStringLiteral("Master"),QStringLiteral("Reds"),QStringLiteral("Yellows"),QStringLiteral("Greens"),QStringLiteral("Cyans"),QStringLiteral("Blues"),QStringLiteral("Magentas")};
    const int selected=names.indexOf(hsv.value(QStringLiteral("range")).toString());if(selected>=0)value.range=ColorRange(selected);value.invertRange=hsv.value(QStringLiteral("invertRange")).toBool();
    const QJsonArray adjustments=hsv.value(QStringLiteral("adjustments")).toArray();for(int i=0;i+1<adjustments.size();i+=2){const int range=names.indexOf(adjustments[i].toString());const QJsonObject a=adjustments[i+1].toObject();if(range>=0)value.adjustments[size_t(range)]={a.value(QStringLiteral("hue")).toDouble(),a.value(QStringLiteral("saturation")).toDouble(),a.value(QStringLiteral("lightness")).toDouble()};}
    const QJsonArray bands=hsv.value(QStringLiteral("bands")).toArray();for(int i=0;i+1<bands.size();i+=2){const int range=names.indexOf(bands[i].toString());const QJsonObject b=bands[i+1].toObject();if(range>=0)value.bands[size_t(range)]={b.value(QStringLiteral("falloffStart")).toDouble(),b.value(QStringLiteral("rangeStart")).toDouble(),b.value(QStringLiteral("rangeEnd")).toDouble(),b.value(QStringLiteral("falloffEnd")).toDouble()};}
    return value;
}

RasterOperations::GradientMapSettings RasterOperations::gradientMapFromJson(const QJsonObject &settings)
{
    QJsonObject value = settings.value(QStringLiteral("gradientMapSettings")).toObject(); if (value.isEmpty()) value = settings.value(QStringLiteral("gradientMap")).toObject();
    const auto color = [](const QJsonValue &entry, const QColor &fallback) { const QJsonObject c = entry.toObject(); return c.isEmpty() ? fallback : QColor::fromRgbF(c.value(QStringLiteral("red")).toDouble(), c.value(QStringLiteral("green")).toDouble(), c.value(QStringLiteral("blue")).toDouble()); };
    return {color(value.value(QStringLiteral("shadows")), Qt::black), color(value.value(QStringLiteral("highlights")), Qt::white), value.value(QStringLiteral("reversed")).toBool()};
}

RasterOperations::GrainSettings RasterOperations::grainFromJson(const QJsonObject &settings)
{
    QJsonObject value = settings.value(QStringLiteral("grainSettings")).toObject(); if (value.isEmpty()) value = settings.value(QStringLiteral("grain")).toObject();
    return {value.value(QStringLiteral("amount")).toDouble(25), value.value(QStringLiteral("size")).toDouble(1.5),
            value.value(QStringLiteral("roughness")).toDouble(50), quint32(value.value(QStringLiteral("seed")).toDouble())};
}

RasterOperations::MotionSettings RasterOperations::motionFromJson(const QJsonObject &settings)
{
    return {settings.value(QStringLiteral("motionAngle")).toDouble(0.0), settings.value(QStringLiteral("motionDistance")).toDouble(10.0)};
}

RasterOperations::NoiseSettings RasterOperations::noiseFromJson(const QJsonObject &settings)
{
    return {float(settings.value(QStringLiteral("noiseAmount")).toDouble(10.0)), settings.value(QStringLiteral("noiseGaussian")).toBool(false),
            settings.value(QStringLiteral("noiseMonochromatic")).toBool(false), quint32(settings.value(QStringLiteral("noiseSeed")).toDouble(0.0))};
}

RasterOperations::BlackWhiteSettings RasterOperations::blackWhiteFromJson(const QJsonObject &settings)
{
    QJsonObject bw = settings.value(QStringLiteral("blackWhiteSettings")).toObject();
    if (bw.isEmpty()) bw = settings.value(QStringLiteral("blackWhite")).toObject();
    BlackWhiteSettings value;
    value.weights = {float(bw.value(QStringLiteral("reds")).toDouble(40.0) / 100.0), float(bw.value(QStringLiteral("yellows")).toDouble(60.0) / 100.0),
                     float(bw.value(QStringLiteral("greens")).toDouble(40.0) / 100.0), float(bw.value(QStringLiteral("cyans")).toDouble(60.0) / 100.0),
                     float(bw.value(QStringLiteral("blues")).toDouble(20.0) / 100.0), float(bw.value(QStringLiteral("magentas")).toDouble(80.0) / 100.0)};
    value.tint = bw.value(QStringLiteral("tint")).toBool(false);
    value.tintHue = bw.value(QStringLiteral("tintHue")).toDouble(40.0);
    value.tintSaturation = bw.value(QStringLiteral("tintSaturation")).toDouble(20.0) / 100.0;
    return value;
}

RasterOperations::ColorBalanceSettings RasterOperations::colorBalanceFromJson(const QJsonObject &settings)
{
    QJsonObject cb = settings.value(QStringLiteral("colorBalanceSettings")).toObject();
    if (cb.isEmpty()) cb = settings.value(QStringLiteral("colorBalance")).toObject();
    const auto read = [&](const char *name) { return float(cb.value(QLatin1String(name)).toDouble(0.0) / 100.0); };
    ColorBalanceSettings value;
    value.shadows = {read("shadowCyanRed"), read("shadowMagentaGreen"), read("shadowYellowBlue")};
    value.midtones = {read("midCyanRed"), read("midMagentaGreen"), read("midYellowBlue")};
    value.highlights = {read("highlightCyanRed"), read("highlightMagentaGreen"), read("highlightYellowBlue")};
    value.preserveLuminosity = cb.value(QStringLiteral("preserveLuminosity")).toBool(true);
    return value;
}

QImage RasterOperations::adjustment(const QImage &image, const QJsonObject &settings,
                                    const QPointF &origin, double unitsPerPixel)
{
    const QString kind = settings.value(QStringLiteral("kind")).toString();
    if (kind == "Progressive Blur" || kind == "Bloom / Glow" || kind == "RGB Split")
        return applyFilterLayer(image, settings, origin, unitsPerPixel);
    if (kind == QStringLiteral("Hue/Saturation")) return hueSaturation(image, hueSaturationFromJson(settings));
    if (kind == QStringLiteral("Levels")) return levels(image, levelsFromJson(settings));
    if (kind == QStringLiteral("Curves")) return curves(image, curvesFromJson(settings));
    if (kind == QStringLiteral("Exposure")) {
        const auto value = exposureFromJson(settings);
        return exposure(image, value.stops, value.offset, value.gamma);
    }
    if (kind == QStringLiteral("Gradient Map")) {
        const auto value = gradientMapFromJson(settings);
        return gradientMap(image, value.shadows, value.highlights, value.reversed);
    }
    if (kind == QStringLiteral("Grain")) {
        const auto value = grainFromJson(settings);
        return grain(image, value.amount, value.size, value.roughness, value.seed, origin, unitsPerPixel);
    }
    if (kind == QStringLiteral("Invert")) {
        return inverted(image);
    }
    if (kind == QStringLiteral("Gaussian Blur")) {
        const double radius = settings.value(QStringLiteral("blurRadius")).toDouble(10.0);
        return gaussianBlur(image, radius);
    }
    if (kind == QStringLiteral("Motion Blur")) {
        const auto value = motionFromJson(settings);
        return motionBlur(image, value.angle, value.distance);
    }
    if (kind == QStringLiteral("Add Noise")) {
        const auto value = noiseFromJson(settings);
        return addNoise(image, value.amount, value.gaussian, value.monochromatic, value.seed);
    }
    if (kind == QStringLiteral("Black & White")) {
        const auto value = blackWhiteFromJson(settings);
        return blackWhite(image, value.weights.data(), value.tint, value.tintHue, value.tintSaturation);
    }
    if (kind == QStringLiteral("Color Balance")) {
        const auto value = colorBalanceFromJson(settings);
        return colorBalance(image, value.shadows.data(), value.midtones.data(), value.highlights.data(), value.preserveLuminosity);
    }
    throw std::runtime_error("Unsupported adjustment kind: " + kind.toStdString());
}

QImage RasterOperations::blackWhite(const QImage &image, const float *weights, bool tint, double tintHue, double tintSaturation)
{
    if (DeepImage::isDeep(image)) return DeepImage::lookup(image, [&](const QImage &low) { return blackWhite(low, weights, tint, tintHue, tintSaturation); });
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    GpuParameters p{};for(int i=0;i<6;++i)p[GpuParam::BlackWhite::red+i]=weights[i];p[GpuParam::BlackWhite::tint]=tint?1:0;p[GpuParam::BlackWhite::tintHue]=tintHue/360;p[GpuParam::BlackWhite::tintSaturation]=tintSaturation;
    if(auto gpu=GpuOperations::simple(result,GpuOp::BlackWhite,p);!gpu.isNull())return gpu;
    adjust_black_white(result.bits(), size_t(result.width()), size_t(result.height()),
                       size_t(result.bytesPerLine()), weights, tint ? 1 : 0, tintHue, tintSaturation);
    return result;
}

QImage RasterOperations::colorBalance(const QImage &image, const float *shadows, const float *midtones, const float *highlights, bool preserveLuminosity)
{
    if (DeepImage::isDeep(image)) return DeepImage::lookup(image, [&](const QImage &low) { return colorBalance(low, shadows, midtones, highlights, preserveLuminosity); });
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    GpuParameters p{};for(int i=0;i<3;++i){p[GpuParam::ColorBalance::shadowRed+i]=shadows[i];p[GpuParam::ColorBalance::midRed+i]=midtones[i];p[GpuParam::ColorBalance::highlightRed+i]=highlights[i];}p[GpuParam::ColorBalance::preserveLuminosity]=preserveLuminosity?1:0;
    if(auto gpu=GpuOperations::simple(result,GpuOp::ColorBalance,p);!gpu.isNull())return gpu;
    adjust_color_balance(result.bits(), size_t(result.width()), size_t(result.height()),
                         size_t(result.bytesPerLine()), shadows, midtones, highlights, preserveLuminosity ? 1 : 0);
    return result;
}

QImage RasterOperations::vignette(const QImage &image, double amount, const QColor &color,
                                  double midpoint, double roundness, double feather,
                                  double highlights, const std::optional<QRectF> &canvasFrame)
{
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return vignette(low, amount, color, midpoint, roundness, feather, highlights, canvasFrame); }, false);
    if (image.isNull()) return image;
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (!std::isfinite(amount) || amount <= 0) return result;

    const double frameX = canvasFrame ? canvasFrame->x() : 0.0;
    const double frameY = canvasFrame ? canvasFrame->y() : 0.0;
    const double frameW = canvasFrame ? canvasFrame->width() : double(result.width());
    const double frameH = canvasFrame ? canvasFrame->height() : double(result.height());
    const int fillsClear = canvasFrame.has_value() ? 1 : 0;

    adjust_colored_vignette(result.bits(), size_t(result.width()), size_t(result.height()),
                            size_t(result.bytesPerLine()),
                            frameX, frameY, frameW, frameH, fillsClear,
                            std::clamp(amount, 0.0, 100.0),
                            std::clamp(midpoint, 0.0, 100.0),
                            std::clamp(roundness, -100.0, 100.0),
                            std::clamp(feather, 0.0, 100.0),
                            std::clamp(highlights, 0.0, 100.0),
                            std::clamp(double(color.redF()), 0.0, 1.0),
                            std::clamp(double(color.greenF()), 0.0, 1.0),
                            std::clamp(double(color.blueF()), 0.0, 1.0));
    return result;
}

QImage RasterOperations::bloomGlow(const QImage &image, double amount, double radius)
{
    if (auto gpu = GpuOperations::bloom(image,amount,radius,DeepImage::isDeep(image)); !gpu.isNull()) return gpu;
    if (DeepImage::isDeep(image)) {
        if (!std::isfinite(amount) || amount <= 0 || !std::isfinite(radius) || radius <= 0) return image;
        const QImage source = DeepImage::floating(image);
        const QImage soft = DeepImage::gaussianBlur(source,std::clamp(radius,1.0,150.0));
        QImage out = source;
        const float gain = float(std::clamp(amount,0.0,100.0)/50);
        for (int y=0;y<out.height();++y) {
            const auto *a = reinterpret_cast<const float *>(source.constScanLine(y));
            const auto *b = reinterpret_cast<const float *>(soft.constScanLine(y));
            auto *p = reinterpret_cast<float *>(out.scanLine(y));
            for (int x=0;x<out.width();++x) {
                const float alpha = a[x*4+3], glowAlpha = std::clamp(b[x*4+3]*gain,0.f,1.f);
                p[x*4+3] = alpha+glowAlpha*(1-alpha);
                for(int c=0;c<3;++c) p[x*4+c] = a[x*4+c]+std::max(0.f,b[x*4+c]*gain)*(1-std::clamp(a[x*4+c],0.f,1.f));
            }
        }
        return out.convertToFormat(image.format());
    }
    if (image.isNull()) return image;
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (!std::isfinite(amount) || amount <= 0 || !std::isfinite(radius) || radius <= 0) return source;

    const double rad = std::clamp(radius, 1.0, 150.0);
    const QImage blurred = gaussianBlur(source, rad);
    const double k = std::clamp(amount, 0.0, 100.0) / 50.0;

    QImage result(source.size(), source.format());
    for (int y = 0; y < source.height(); ++y) {
        const uchar *srcLine = source.constScanLine(y);
        const uchar *blurLine = blurred.constScanLine(y);
        uchar *dstLine = result.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            const int idx = x * 4;
            const double sR = srcLine[idx];
            const double sG = srcLine[idx + 1];
            const double sB = srcLine[idx + 2];
            const double sA = srcLine[idx + 3];

            const double bR = std::min(255.0, blurLine[idx] * k);
            const double bG = std::min(255.0, blurLine[idx + 1] * k);
            const double bB = std::min(255.0, blurLine[idx + 2] * k);
            const double bA = std::min(255.0, blurLine[idx + 3] * k);

            // Screen blend: C_out = C_0 + C_bloom - (C_0 * C_bloom) / 255
            const double outA = std::clamp(sA + bA - (sA * bA) / 255.0, 0.0, 255.0);
            const double outR = std::clamp(sR + bR - (sR * bR) / 255.0, 0.0, outA);
            const double outG = std::clamp(sG + bG - (sG * bG) / 255.0, 0.0, outA);
            const double outB = std::clamp(sB + bB - (sB * bB) / 255.0, 0.0, outA);

            dstLine[idx] = static_cast<uchar>(std::round(outR));
            dstLine[idx + 1] = static_cast<uchar>(std::round(outG));
            dstLine[idx + 2] = static_cast<uchar>(std::round(outB));
            dstLine[idx + 3] = static_cast<uchar>(std::round(outA));
        }
    }
    return result;
}

QImage RasterOperations::tonalContrast(const QImage &image, double amount, double radius,
                                       double shadows, double midtones, double highlights)
{
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return tonalContrast(low, amount, radius, shadows, midtones, highlights); }, true);
    if (image.isNull()) return image;
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (!std::isfinite(amount) || amount <= 0 || (shadows == 0 && midtones == 0 && highlights == 0)) return result;

    const double rad = std::clamp(radius, 1.0, 100.0);
    const QImage blurred = gaussianBlur(result, rad);

    adjust_tonal_contrast(result.bits(), blurred.constBits(),
                          size_t(result.width()), size_t(result.height()),
                          size_t(result.bytesPerLine()), size_t(blurred.bytesPerLine()),
                          std::clamp(amount, 0.0, 100.0),
                          std::clamp(shadows, -100.0, 100.0),
                          std::clamp(midtones, -100.0, 100.0),
                          std::clamp(highlights, -100.0, 100.0));
    return result;
}

std::array<uint8_t, 768> RasterOperations::gradientMapTable(const GradientMapSettings &settings)
{
    const QColor dark = settings.reversed ? settings.highlights : settings.shadows, light = settings.reversed ? settings.shadows : settings.highlights;
    std::array<uint8_t, 256 * 3> table{};
    for (int i = 0; i < 256; ++i) { const double t = i / 255.0;
        table[i * 3] = uint8_t(qRound(dark.red() + (light.red() - dark.red()) * t));
        table[i * 3 + 1] = uint8_t(qRound(dark.green() + (light.green() - dark.green()) * t));
        table[i * 3 + 2] = uint8_t(qRound(dark.blue() + (light.blue() - dark.blue()) * t));
    }
    return table;
}

QImage RasterOperations::gradientMap(const QImage &image, const QColor &shadows, const QColor &highlights, bool reversed)
{
    if (DeepImage::isDeep(image)) return DeepImage::lookup(image, [&](const QImage &low) { return gradientMap(low, shadows, highlights, reversed); });
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const std::array<uint8_t, 256 * 3> table = gradientMapTable({shadows, highlights, reversed});
    if (GpuRenderer::shouldAttempt(result.size(), 8)) { // ~3.4× faster at 1080p (tools/GpuBenchmark).
        GpuGraph graph;
        const auto gpu = graph.execute(GpuOperations::appendGradientMap(graph, graph.source(result), table, 1), result.format());
        if (!gpu.isNull()) return gpu;
    }
    adjust_gradient_map(result.bits(), size_t(result.width()), size_t(result.height()), size_t(result.bytesPerLine()), table.data());
    return result;
}

QImage RasterOperations::grain(const QImage &image, double amount, double size, double roughness, quint32 seed,
                               const QPointF &origin, double unitsPerPixel)
{
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return grain(low, amount, size, roughness, seed, origin, unitsPerPixel); });
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (amount <= 0) return result;
    adjust_grain(result.bits(), size_t(result.width()), size_t(result.height()), size_t(result.bytesPerLine()),
                 std::clamp(amount, 0.0, 100.0), std::clamp(size, .5, 20.0), std::clamp(roughness, 0.0, 100.0), seed,
                 origin.x(), origin.y(), unitsPerPixel);
    return result;
}

QImage RasterOperations::gaussianBlur(const QImage &image, double radius)
{
    if (DeepImage::isDeep(image)) return DeepImage::gaussianBlur(image, radius);
    if (auto gpu = GpuOperations::gaussian(image.convertToFormat(QImage::Format_RGBA8888_Premultiplied),radius,true); !gpu.isNull()) return gpu;
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (!std::isfinite(radius) || radius <= 0) return source;
    radius = std::clamp(radius, .1, 250.0);
    constexpr int passes = 3;
    const double ideal = std::sqrt(12 * radius * radius / passes + 1);
    int lower = int(std::floor(ideal)); if (!(lower & 1)) --lower;
    const int upper = lower + 2;
    const int lowerCount = qRound((12 * radius * radius - passes * lower * lower - 4 * passes * lower - 3 * passes) / (-4.0 * lower - 4));
    const auto boxPass = [](const QImage &input, int boxRadius, bool horizontal) {
        if (boxRadius <= 0) return input;
        QImage output(input.size(), input.format()); const int divisor = boxRadius * 2 + 1;
        const int lines = horizontal ? input.height() : input.width(), length = horizontal ? input.width() : input.height();
        for (int line = 0; line < lines; ++line) {
            qint64 sums[4]{};
            const auto pixel = [&](int position) { const int p = std::clamp(position, 0, length - 1); return horizontal ? input.constScanLine(line) + p * 4 : input.constScanLine(p) + line * 4; };
            for (int position = -boxRadius; position <= boxRadius; ++position) { const uchar *value = pixel(position); for (int c = 0; c < 4; ++c) sums[c] += value[c]; }
            for (int position = 0; position < length; ++position) {
                uchar *out = horizontal ? output.scanLine(line) + position * 4 : output.scanLine(position) + line * 4;
                for (int c = 0; c < 4; ++c) out[c] = uchar((sums[c] + divisor / 2) / divisor);
                const uchar *leaving = pixel(position - boxRadius), *entering = pixel(position + boxRadius + 1);
                for (int c = 0; c < 4; ++c) sums[c] += int(entering[c]) - int(leaving[c]);
            }
        }
        return output;
    };
    QImage result = source;
    for (int pass = 0; pass < passes; ++pass) {
        const int width = pass < lowerCount ? lower : upper, boxRadius = std::max(0, (width - 1) / 2);
        result = boxPass(boxPass(result, boxRadius, true), boxRadius, false);
    }
    return result;
}

QImage RasterOperations::featherMask(const QImage &image, double amount)
{
    if (image.isNull() || !std::isfinite(amount) || amount <= 0) return image;
    const double radius = std::clamp(amount / 2.0, 0.1, 125.0);
    const QImage source = image.convertToFormat(QImage::Format_Grayscale8);
    if (auto gpu = GpuOperations::gaussian(source.convertToFormat(QImage::Format_RGBA8888_Premultiplied),radius,true); !gpu.isNull()) return gpu.convertToFormat(QImage::Format_Grayscale8);
    constexpr int passes = 3;
    const double ideal = std::sqrt(12 * radius * radius / passes + 1);
    int lower = int(std::floor(ideal)); if (!(lower & 1)) --lower;
    const int upper = lower + 2;
    const int lowerCount = qRound((12 * radius * radius - passes * lower * lower - 4 * passes * lower - 3 * passes) / (-4.0 * lower - 4));
    const auto boxPass = [](const QImage &input, int boxRadius, bool horizontal) {
        if (boxRadius <= 0) return input;
        QImage output(input.size(), QImage::Format_Grayscale8);
        const int divisor = boxRadius * 2 + 1;
        const int lines = horizontal ? input.height() : input.width();
        const int length = horizontal ? input.width() : input.height();
        for (int line = 0; line < lines; ++line) {
            int sum = 0;
            const auto pixel = [&](int position) {
                const int p = std::clamp(position, 0, length - 1);
                return int(horizontal ? input.constScanLine(line)[p] : input.constScanLine(p)[line]);
            };
            for (int position = -boxRadius; position <= boxRadius; ++position)
                sum += pixel(position);
            for (int position = 0; position < length; ++position) {
                uchar *out = horizontal ? output.scanLine(line) + position : output.scanLine(position) + line;
                *out = uchar((sum + divisor / 2) / divisor);
                const int leaving = pixel(position - boxRadius);
                const int entering = pixel(position + boxRadius + 1);
                sum += entering - leaving;
            }
        }
        return output;
    };
    QImage result = source;
    for (int pass = 0; pass < passes; ++pass) {
        const int width = pass < lowerCount ? lower : upper;
        const int boxRadius = std::max(0, (width - 1) / 2);
        result = boxPass(boxPass(result, boxRadius, true), boxRadius, false);
    }
    return result;
}

namespace {
QImage transposed(const QImage &image)
{
    QImage result(image.height(), image.width(), image.format());
    for (int y = 0; y < image.height(); ++y) {
        const quint32 *row = reinterpret_cast<const quint32 *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) reinterpret_cast<quint32 *>(result.scanLine(x))[y] = row[x];
    }
    return result;
}

// An even streak `reach` pixels either side along a line of `slope` rows per column (|slope| <= 1): every pixel takes
// the average of the 2 * reach + 1 pixels on that line around it, those past the image counting as transparent. The
// image is sheared so each line runs along one row, then averaged with running sums: constant work per pixel however
// long the streak.
QImage horizontalStreak(const QImage &source, double slope, int reach)
{
    const int width = source.width(), height = source.height();
    QImage result(source.size(), source.format());
    result.fill(Qt::transparent);
    if (width < 1 || height < 1) return result;
    std::vector<int> shift(static_cast<size_t>(width));
    int lowest = 0, highest = 0;
    for (int u = 0; u < width; ++u) {
        shift[size_t(u)] = int(std::lround(slope * u));
        lowest = std::min(lowest, shift[size_t(u)]); highest = std::max(highest, shift[size_t(u)]);
    }
    const double divisor = 2.0 * reach + 1;
    std::vector<qint64> prefix(size_t(width + 1) * 4);
    // Sheared row r holds the pixels (u, r + shift(u)); every output pixel lies on exactly one of them.
    for (int r = -highest; r < height - lowest; ++r) {
        bool any = false;
        for (int u = 0; u < width; ++u) {
            const int v = r + shift[size_t(u)];
            qint64 *next = prefix.data() + size_t(u + 1) * 4;
            const qint64 *previous = next - 4;
            if (v < 0 || v >= height) { for (int c = 0; c < 4; ++c) next[c] = previous[c]; continue; }
            const uchar *pixel = source.constScanLine(v) + u * 4;
            for (int c = 0; c < 4; ++c) next[c] = previous[c] + pixel[c];
            any = true;
        }
        if (!any) continue;
        for (int u = 0; u < width; ++u) {
            const int v = r + shift[size_t(u)];
            if (v < 0 || v >= height) continue;
            const qint64 *high = prefix.data() + size_t(std::min(width, u + reach + 1)) * 4;
            const qint64 *low = prefix.data() + size_t(std::max(0, u - reach)) * 4;
            uchar *out = result.scanLine(v) + u * 4;
            for (int c = 0; c < 4; ++c) out[c] = uchar(std::clamp(int(std::lround(double(high[c] - low[c]) / divisor)), 0, 255));
        }
    }
    return result;
}
} // namespace

QImage RasterOperations::motionBlur(const QImage &image, double angleDegrees, double distance)
{
    if (DeepImage::isDeep(image)) return DeepImage::motionBlur(image, angleDegrees, distance);
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (!std::isfinite(angleDegrees) || !std::isfinite(distance) || distance <= 0) return source;
    angleDegrees = std::clamp(angleDegrees, -90.0, 90.0); distance = std::clamp(distance, 1.0, 2000.0);
    if(auto gpu=GpuOperations::motion(source,angleDegrees,distance);!gpu.isNull())return gpu;
    // Photoshop smears evenly along the whole distance, counterclockwise from horizontal (y runs down here).
    const double radians = angleDegrees * 3.14159265358979323846 / 180.0;
    const double dx = std::cos(radians), dy = -std::sin(radians);
    // Walked along whichever axis the streak leans to, one pixel per step on that axis.
    const bool horizontal = std::abs(dx) >= std::abs(dy);
    const double major = horizontal ? dx : dy, minor = horizontal ? dy : dx;
    const int reach = std::max(0, int(std::lround(distance * std::abs(major) / 2)));
    const double slope = major == 0 ? 0 : minor / major;
    if (horizontal) return horizontalStreak(source, slope, reach);
    return transposed(horizontalStreak(transposed(source), slope, reach));
}

std::optional<QImage> RasterOperations::contentAwareFill(const QImage &image, const QImage &coverage)
{
    if (image.isNull() || coverage.size() != image.size()) return std::nullopt;
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage mask = coverage.convertToFormat(QImage::Format_Grayscale8);
    const int status = content_fill(result.bits(), size_t(result.bytesPerLine()), mask.constBits(),
                                    size_t(mask.bytesPerLine()), result.width(), result.height());
    return status == 1 ? std::optional<QImage>(result) : std::nullopt;
}

std::optional<QImage> RasterOperations::spotHeal(const QImage &image, const QImage &coverage,
                                                  double opacity, int mode, quint32 seed)
{
    if (image.isNull() || coverage.size() != image.size() || opacity <= 0 || opacity > 1 || mode < 0 || mode > 2) return std::nullopt;
    QImage result = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const QImage mask = coverage.convertToFormat(QImage::Format_Grayscale8);
    std::vector<uint8_t> packed(size_t(mask.width()) * mask.height());
    for (int y = 0; y < mask.height(); ++y) std::copy_n(mask.constScanLine(y), mask.width(), packed.data() + size_t(y) * mask.width());
    const int status = spot_heal(result.bits(), packed.data(), size_t(result.width()), size_t(result.height()),
                                 size_t(result.bytesPerLine()), float(opacity), mode, seed);
    return status == 0 ? std::optional<QImage>(result) : std::nullopt;
}

QJsonObject RasterOperations::levelsSettingsToJson(const LevelsSettings &settings)
{
    QJsonArray ranges;
    for (const LevelRange &unnormalized : settings.ranges) {
        const LevelRange range = unnormalized.normalized();   // never write what macOS would refuse to open
        ranges.append(QJsonObject{
            {QStringLiteral("black"), range.black},
            {QStringLiteral("gamma"), range.gamma},
            {QStringLiteral("white"), range.white},
            {QStringLiteral("outputBlack"), range.outputBlack},
            {QStringLiteral("outputWhite"), range.outputWhite}
        });
    }
    return {
        {QStringLiteral("kind"), QStringLiteral("Levels")},
        {QStringLiteral("levels"), QJsonObject{
            {QStringLiteral("channel"), QStringLiteral("RGB")},
            {QStringLiteral("ranges"), ranges}
        }}
    };
}

QJsonObject RasterOperations::curvesSettingsToJson(const CurvesSettings &settings)
{
    QJsonArray channels;
    for (const auto &curve : settings.channels) {
        QJsonArray points;
        for (const CurvePoint &point : curve) {
            points.append(QJsonObject{{QStringLiteral("x"), point.x}, {QStringLiteral("y"), point.y}});
        }
        channels.append(points);
    }
    return {
        {QStringLiteral("kind"), QStringLiteral("Curves")},
        {QStringLiteral("curves"), QJsonObject{
            {QStringLiteral("channel"), QStringLiteral("RGB")},
            {QStringLiteral("channels"), channels}
        }}
    };
}

QJsonObject RasterOperations::hueSaturationSettingsToJson(const HueSaturationSettings &settings)
{
    static const QStringList names{
        QStringLiteral("Master"), QStringLiteral("Reds"), QStringLiteral("Yellows"),
        QStringLiteral("Greens"), QStringLiteral("Cyans"), QStringLiteral("Blues"),
        QStringLiteral("Magentas")
    };
    QJsonArray adjustments, bands;
    for (int i = 0; i < names.size(); ++i) {
        const RangeAdjustment &a = settings.adjustments[size_t(i)];
        adjustments.append(names[i]);
        adjustments.append(QJsonObject{
            {QStringLiteral("hue"), a.hue},
            {QStringLiteral("saturation"), a.saturation},
            {QStringLiteral("lightness"), a.lightness}
        });
        const HueBand &b = settings.bands[size_t(i)];
        bands.append(names[i]);
        bands.append(QJsonObject{
            {QStringLiteral("falloffStart"), b.falloffStart},
            {QStringLiteral("rangeStart"), b.rangeStart},
            {QStringLiteral("rangeEnd"), b.rangeEnd},
            {QStringLiteral("falloffEnd"), b.falloffEnd}
        });
    }
    const int rangeIdx = std::clamp(static_cast<int>(settings.range), 0, static_cast<int>(names.size()) - 1);
    const QJsonObject hsv{
        {QStringLiteral("range"), names.at(rangeIdx)},
        {QStringLiteral("colorize"), settings.colorize},
        {QStringLiteral("invertRange"), settings.invertRange},
        {QStringLiteral("adjustments"), adjustments},
        {QStringLiteral("bands"), bands}
    };
    const RangeAdjustment &master = settings.adjustments[size_t(ColorRange::Master)];
    return QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("Hue/Saturation")},
        {QStringLiteral("hue"), master.hue},
        {QStringLiteral("saturation"), master.saturation},
        {QStringLiteral("lightness"), master.lightness},
        {QStringLiteral("colorize"), settings.colorize},
        {QStringLiteral("hsvSettings"), hsv}
    };
}

QImage RasterOperations::cameraRaw(const QImage &image, const CameraRawSettings &settingsIn,
                                   CameraRawClipping clipping, double scale, quint32 seed,
                                   int visualizePointColor, bool sharpenMask)
{
    if (DeepImage::isDeep(image)) return DeepImage::through(image, [&](const QImage &low) { return cameraRaw(low, settingsIn, clipping, scale, seed, visualizePointColor, sharpenMask); }, true);
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) return image;
    const CameraRawSettings settings = settingsIn.normalized();
    if (settings.isIdentity() && clipping == CameraRawClipping::None && visualizePointColor < 0 && !sharpenMask) {
        return image;
    }
    if (!settings.isValid()) return image;

    const int mode = static_cast<int>(clipping);
    const double pixelScale = (scale > 0.0) ? scale : 1.0;

    const bool foldsColor = clipping == CameraRawClipping::None && !sharpenMask && visualizePointColor < 0
        && settings.mixer.points.isEmpty() && settings.curve.refineSaturation == 0;
    const bool paintColor = !foldsColor && (clipping == CameraRawClipping::None && !sharpenMask) &&
        (settings.adjustsCurve() || settings.adjustsMixer() || settings.adjustsGrading() || visualizePointColor >= 0);
    const bool paintEffects = (clipping == CameraRawClipping::None && !sharpenMask && settings.adjustsEffects());
    const bool paintDetailOptics = (clipping == CameraRawClipping::None) &&
        (settings.adjustsDetail() || settings.adjustsOptics() || sharpenMask);

    QImage source = image;
    if (clipping == CameraRawClipping::None && !sharpenMask && visualizePointColor < 0 && settings.adjustsGeometry()) {
        source = settings.geometry.apply(source);
    }

    QImage result = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    uint8_t *pixels = result.bits();
    const int width = result.width();
    const int height = result.height();
    const int stride = result.bytesPerLine();

    // 1. Calibration
    if (clipping == CameraRawClipping::None && !sharpenMask && settings.adjustsCalibration()) {
        adjust_camera_raw_calibration(pixels, width, height, stride,
                                      settings.calibration.shadowTint,
                                      0, 0, 0, 0, 0, 0,
                                      static_cast<int>(settings.calibration.process));
    }

    // Compose the measured stages once; Shadows measures neighboring pixels between the two halves.
    using namespace CameraRawTables;
    const Brightness measured = (settings.contrast != 0 || settings.highlights != 0) ? brightness(source) : Brightness{};
    if (settings.shadows != 0) {
        const auto before = compose(stages(settings, measured, Part::BeforeShadows));
        if (!before.empty() && !GpuOperations::cameraRawLookup(result, before.data(), grid))
            adjust_camera_raw(result.bits(), width, height, stride, before.data(), grid, 0);
        applyShadows(result, settings.shadows, pixelScale);
        pixels = result.bits();
    }
    auto table = compose(stages(settings, measured, settings.shadows != 0 ? Part::AfterShadows : Part::All));
    if (foldsColor && (settings.adjustsCurve() || settings.adjustsMixer() || settings.adjustsGrading())) {
        if (table.empty()) table = compose({}, true);
        if (settings.adjustsCurve()) {
            const auto tone = settings.curve.toneTable(), red = settings.curve.channelTable(settings.curve.red),
                       green = settings.curve.channelTable(settings.curve.green), blue = settings.curve.channelTable(settings.curve.blue);
            camera_raw_table_curves(table.data(), grid, tone.data(), red.data(), green.data(), blue.data());
        }
        auto colorStages = mixerStages(settings.mixer);
        const auto grading = gradingStages(settings.grading);
        colorStages.insert(colorStages.end(), grading.begin(), grading.end());
        camera_raw_compose_onto(table.data(), grid, colorStages.data(), int(colorStages.size()), size);
    }
    // macOS: the table on Metal (MetalCameraRawLookup) when it can, byte for byte the CPU's; the clipping overlay
    // stays on the CPU.
    if (!table.empty() || mode != 0) {
        if (mode != 0 || table.empty() || !GpuOperations::cameraRawLookup(result, table.data(), grid))
            adjust_camera_raw(result.bits(), width, height, stride, table.empty() ? nullptr : table.data(), grid, mode);
        pixels = result.bits();
    }

    // 3. Curve, Mixer, Grading
    if (paintColor) {
        const auto luma = settings.curve.toneTable();
        const auto redTable = settings.curve.channelTable(settings.curve.red);
        const auto greenTable = settings.curve.channelTable(settings.curve.green);
        const auto blueTable = settings.curve.channelTable(settings.curve.blue);
        const std::array<float, 24> mixer{};
        const auto mixerTable = compose(mixerStages(settings.mixer));
        const auto points = settings.mixer.pointFloats();
        const std::array<float, 12> grade{};
        const auto gradeTable = compose(gradingStages(settings.grading));

        adjust_camera_raw_curve_color(pixels, width, height, stride,
                                      luma.data(), redTable.data(), greenTable.data(), blueTable.data(),
                                      settings.curve.refineSaturation / 100.0, mixer.data(),
                                      static_cast<int>(settings.mixer.points.size()), points.constData(),
                                      grade.data(), settings.grading.blending / 100.0, settings.grading.balance / 100.0,
                                      visualizePointColor, mixerTable.empty() ? nullptr : mixerTable.data(),
                                      gradeTable.empty() ? nullptr : gradeTable.data(), grid);
    }

    // 4. Effects
    if (paintEffects) {
        if (settings.texture != 0.0 || settings.clarity != 0.0 || settings.dehaze != 0.0 ||
            settings.glow != 0.0 || settings.vignetteAmount != 0.0) {
            adjust_camera_raw_effects(pixels, width, height, stride,
                                      settings.texture, settings.clarity, settings.dehaze,
                                      settings.glow, static_cast<int>(settings.glowStyle),
                                      settings.glowRange, settings.glowSpread, settings.glowWarmth,
                                      settings.vignetteAmount, settings.vignetteMidpoint,
                                      settings.vignetteRoundness, settings.vignetteFeather,
                                      settings.vignetteHighlights, static_cast<int>(settings.vignetteStyle),
                                      pixelScale);
        }
        if (settings.grainAmount > 0.0) {
            adjust_grain(pixels, width, height, stride,
                         settings.grainAmount, settings.grainKernelSize(),
                         settings.grainRoughness, seed, 0, 0, 1.0 / pixelScale);
        }
    }

    // 5. Detail & Optics
    if (paintDetailOptics) {
        if (sharpenMask) {
            adjust_camera_raw_sharpen_mask_overlay(pixels, width, height, stride,
                                                   settings.detail.sharpenRadius,
                                                   settings.detail.sharpenDetail,
                                                   settings.detail.sharpenMasking,
                                                   pixelScale);
        } else {
            if (settings.adjustsOptics()) {
                adjust_camera_raw_optics(pixels, width, height, stride,
                                         settings.optics.removeChromaticAberration ? 1 : 0,
                                         settings.optics.enableLensProfile ? 1 : 0,
                                         settings.optics.profileDistortion,
                                         settings.optics.profileVignetting,
                                         settings.optics.distortionK(0.35),
                                         settings.optics.purpleAmount,
                                         settings.optics.purpleHueLow,
                                         settings.optics.purpleHueHigh,
                                         settings.optics.greenAmount,
                                         settings.optics.greenHueLow,
                                         settings.optics.greenHueHigh,
                                         settings.optics.vignetteAmount,
                                         settings.optics.vignetteMidpoint,
                                         pixelScale, exposureTables(), size);
            }
            if (settings.adjustsDetail()) {
                adjust_camera_raw_detail(pixels, width, height, stride,
                                         settings.detail.sharpenAmount,
                                         settings.detail.sharpenRadius,
                                         settings.detail.sharpenDetail,
                                         settings.detail.sharpenMasking,
                                         settings.detail.noiseLuminance,
                                         settings.detail.noiseLuminanceDetail,
                                         settings.detail.noiseLuminanceContrast,
                                         settings.detail.noiseColor,
                                         settings.detail.noiseColorDetail,
                                         settings.detail.noiseColorSmoothness,
                                         pixelScale);
            }
        }
    }

    return result.convertToFormat(image.format());
}

} // namespace compositor
