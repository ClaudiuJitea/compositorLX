#include "rendering/DeepImage.h"
#include "rendering/RgbSplit.h"
#include "rendering/GpuRenderer.h"
#include "rendering/RgbSplitVulkan.h"
#include "rendering/ProgressiveBlur.h"
#include "rendering/RasterOperations.h"
#include <QJsonArray>
#include <array>
#include <algorithm>
#include <cmath>

namespace compositor {
QImage RgbSplitSettings::apply(const QImage &image, unsigned sides, RgbSplitBackend backend, RgbSplitExecution *execution) const
{
    RgbSplitExecution report;
    if (backend == RgbSplitBackend::Auto) {
        const auto choice = qgetenv("COMPOSITOR_RGB_SPLIT_BACKEND").toLower();
        if (choice == "cpu") backend = RgbSplitBackend::Cpu;
        else if (choice == "vulkan") backend = RgbSplitBackend::Vulkan;
        else if (GpuRenderer::backend() == RenderBackend::Cpu) backend = RgbSplitBackend::Cpu;
        else if (GpuRenderer::backend() == RenderBackend::Vulkan) backend = RgbSplitBackend::Vulkan;
        else {
            // Small images and cheap effects cost less than conversion/dispatch. These
            // conservative cutoffs include transfers in the measured GPU render time.
            const double samples = std::clamp(std::ceil(distance * 2) + 1, 1., 48.)
                * (blur > 0 ? std::clamp(std::ceil(blur / 3), 2., 24.) : 1.);
            backend = qint64(image.width()) * image.height() >= 262144 && samples >= 16
                ? RgbSplitBackend::Vulkan : RgbSplitBackend::Cpu;
        }
    }
    if (!image.isNull() && (distance > 0 || blur > 0) && backend == RgbSplitBackend::Vulkan) {
        QImage accelerated = tryRgbSplitVulkan(image, *this, sides, report);
        if (!accelerated.isNull()) { if (execution) *execution = report; return accelerated; }
    }
    if (execution) *execution = report;
    return applyCpu(image, sides);
}

int RgbSplitSettings::spectralSamples() const
{
    return std::clamp(int(std::clamp(std::ceil(distance * 2), 0.0, 47.0)) + 1, 1, 48);
}

int RgbSplitSettings::smearSamples() const
{
    return blur > 0 ? std::clamp(int(std::clamp(std::ceil(blur / 3), 2.0, 24.0)), 2, 24) : 1;
}

int RgbSplitSettings::twoPassPad(QSize size) const
{
    double spread = distance;
    if (radial) {
        double farthest = 0;
        for (const QPointF corner : {QPointF(0, 0), QPointF(size.width(), 0), QPointF(0, size.height()), QPointF(size.width(), size.height())})
            farthest = std::max(farthest, std::hypot(corner.x() - center.x(), corner.y() - center.y()));
        spread = distance * farthest / std::max(reach, 1.0);
    }
    return int(std::ceil(std::clamp(spread, 0.0, 1e5))) + 2;
}

namespace {
double splitJitter(int x, int y)
{
    const double hash = std::sin(x * 12.9898 + y * 78.233) * 43758.5453;
    return hash - std::floor(hash);
}
double smoothstep(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}
// Bilinear at `q` (pixel centers at .5) in a float RGBA plane, its edge pixels carried on past it.
std::array<double, 4> sampleAt(const float *pixels, qsizetype stride, int width, int height, QPointF q)
{
    const double sx = std::clamp(q.x() - .5, 0.0, double(width - 1));
    const double sy = std::clamp(q.y() - .5, 0.0, double(height - 1));
    const int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const double fx = sx - x0, fy = sy - y0;
    std::array<double, 4> sample{};
    for (int c = 0; c < 4; ++c)
        sample[c] = (pixels[y0 * stride + x0 * 4 + c] * (1 - fx) + pixels[y0 * stride + x1 * 4 + c] * fx) * (1 - fy)
                  + (pixels[y1 * stride + x0 * 4 + c] * (1 - fx) + pixels[y1 * stride + x1 * 4 + c] * fx) * fy;
    return sample;
}
bool outsideSides(QPointF q, int width, int height, unsigned sides)
{
    return (q.x() < 0 && !(sides & 1)) || (q.x() > width && !(sides & 2)) || (q.y() < 0 && !(sides & 4)) || (q.y() > height && !(sides & 8));
}
std::array<double, 3> spectrumWeight(double t)
{
    return {smoothstep(-.3, 1, t), 1 - smoothstep(0, 1, std::abs(t)) + .05, smoothstep(-.3, 1, -t)};
}
} // namespace

QImage RgbSplitSettings::applyCpu(const QImage &image, unsigned sides) const
{
    if (image.isNull() || (distance <= 0 && blur <= 0)) return image;
    for (double value : {distance, blur, angle, reach, center.x(), center.y()})
        if (!std::isfinite(value)) return image;
    const QImage source = DeepImage::floating(image);
    if (source.isNull()) return {};
    QImage result(source.size(), source.format());
    if (result.isNull()) return {};
    const int width = source.width(), height = source.height();
    const auto *pixels = reinterpret_cast<const float *>(source.constBits());
    const qsizetype stride = source.bytesPerLine() / sizeof(float);
    auto *output = reinterpret_cast<float *>(result.bits());
    const qsizetype outStride = result.bytesPerLine() / sizeof(float);
    const double radians = angle * M_PI / 180;
    const QPointF direction(std::cos(radians), std::sin(radians));
    const int spectral = spectralSamples(), smear = smearSamples();
    const auto axisAt = [&](QPointF at) { return radial ? (at - center) / std::max(1.0, reach) : direction; };
    // The spectrum, from `plane` (`pw` × `ph`, its origin `offset` pixels before the image's) into the result.
    const auto spectrum = [&](const float *plane, qsizetype planeStride, int pw, int ph, int offset, unsigned planeSides, int streaks) {
        #pragma omp parallel for if(width * height > 16384)
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const QPointF at(x + .5, y + .5);
                const QPointF axis = axisAt(at);
                const double jitter = splitJitter(x, y);
                std::array<double, 3> color{}, alpha{}, total{};
                for (int i = 0; i < spectral; ++i) {
                    const double t = spectral == 1 ? 0 : (i + jitter) / spectral * 2 - 1;
                    const auto weight = spectrumWeight(t);
                    for (int j = 0; j < streaks; ++j) {
                        // Each streak runs both ways along the split, centered on the pixel, so the image smears out
                        // without shifting one way (macOS MetalDispersion).
                        const double u = streaks == 1 ? 0 : (j + jitter) / streaks - .5;
                        const QPointF q = at - axis * (distance * t + blur * u) + QPointF(offset, offset);
                        std::array<double, 4> sample{};
                        if (!outsideSides(q, pw, ph, planeSides)) sample = sampleAt(plane, planeStride, pw, ph, q);
                        for (int c = 0; c < 3; ++c) { color[c] += weight[c] * sample[c]; alpha[c] += weight[c] * sample[3]; total[c] += weight[c]; }
                    }
                }
                const double a = std::max({alpha[0] / total[0], alpha[1] / total[1], alpha[2] / total[2]});
                for (int c = 0; c < 3; ++c) output[y * outStride + x * 4 + c] = float(std::min(color[c] / total[c], a));
                output[y * outStride + x * 4 + 3] = float(a);
            }
        }
    };
    if (smear <= 1) {
        spectrum(pixels, stride, width, height, 0, sides, 1);
        return result.convertToFormat(image.format());
    }
    // Two passes (mac 79f98de): the streaks into a copy padded by the spectrum's reach, then the spectrum from it.
    const int pad = twoPassPad(source.size());
    const int pw = width + pad * 2, ph = height + pad * 2;
    std::vector<float> streaked(size_t(pw) * ph * 4);
    #pragma omp parallel for if(pw * ph > 16384)
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const QPointF at(x - pad + .5, y - pad + .5);
            const QPointF axis = axisAt(at);
            const double jitter = splitJitter(x - pad, y - pad);
            std::array<double, 4> sum{};
            for (int j = 0; j < smear; ++j) {
                const double u = (j + jitter) / smear - .5;
                const QPointF q = at - axis * (blur * u);
                if (outsideSides(q, width, height, sides)) continue;
                const auto sample = sampleAt(pixels, stride, width, height, q);
                for (int c = 0; c < 4; ++c) sum[c] += sample[c];
            }
            for (int c = 0; c < 4; ++c) streaked[(size_t(y) * pw + x) * 4 + c] = float(sum[c] / smear);
        }
    }
    spectrum(streaked.data(), qsizetype(pw) * 4, pw, ph, pad, 15, 1);
    return result.convertToFormat(image.format());
}

namespace {
QPointF filterPoint(const QJsonObject &settings, const char *name, QPointF fallback, const QPointF &origin, double scale)
{
    const QJsonArray a = settings.value(QLatin1String(name)).toArray();
    return a.size() == 2 ? (QPointF(a[0].toDouble(), a[1].toDouble()) - origin) / scale : fallback;
}
}
ProgressiveBlurSettings progressiveFromJson(const QJsonObject &adjustment, QSize size, const QPointF &origin, double unitsPerPixel)
{
    const auto settings = adjustment.value("filterLayerSettings").toObject();
    const double scale = std::max(.000001, unitsPerPixel);
    ProgressiveBlurSettings s;
    s.from = settings.value("progressiveFrom").toDouble() / scale;
    s.to = settings.value("progressiveTo").toDouble(20) / scale;
    s.start = filterPoint(settings, "progressiveStart", QPointF(size.width() / 2.0, size.height() * .35), origin, scale);
    s.end = filterPoint(settings, "progressiveEnd", QPointF(size.width() / 2.0, size.height()), origin, scale);
    s.radial = settings.value("progressiveRadial").toBool();
    s.inverted = settings.value("progressiveInverted").toBool();
    return s;
}
RgbSplitSettings rgbSplitFromJson(const QJsonObject &adjustment, QSize size, const QPointF &origin, double unitsPerPixel)
{
    const auto settings = adjustment.value("filterLayerSettings").toObject();
    const double scale = std::max(.000001, unitsPerPixel);
    RgbSplitSettings s;
    s.distance = settings.value("rgbSplitDistance").toDouble(10) / scale;
    s.blur = settings.value("rgbSplitBlur").toDouble() / scale;
    s.angle = settings.value("rgbSplitAngle").toDouble();
    s.radial = settings.value("rgbSplitRadial").toBool();
    s.center = filterPoint(settings, "rgbSplitCenter", QPointF(size.width() / 2.0, size.height() / 2.0), origin, scale);
    s.reach = settings.value("rgbSplitReach").toDouble(1000) / scale;
    return s;
}
BloomFilterSettings bloomFromJson(const QJsonObject &adjustment, double unitsPerPixel)
{
    const auto settings = adjustment.value("filterLayerSettings").toObject();
    return {settings.value("bloomAmount").toDouble(40), settings.value("bloomRadius").toDouble(24) / std::max(.000001, unitsPerPixel)};
}
QImage applyFilterLayer(const QImage &image, const QJsonObject &adjustment, const QPointF &origin, double unitsPerPixel)
{
    const QString kind = adjustment.value("kind").toString();
    if (kind == "Progressive Blur") return progressiveFromJson(adjustment, image.size(), origin, unitsPerPixel).apply(image);
    if (kind == "Bloom / Glow") {
        const auto bloom = bloomFromJson(adjustment, unitsPerPixel);
        return RasterOperations::bloomGlow(image, bloom.amount, bloom.radius);
    }
    return rgbSplitFromJson(adjustment, image.size(), origin, unitsPerPixel).apply(image, 15);
}
}
