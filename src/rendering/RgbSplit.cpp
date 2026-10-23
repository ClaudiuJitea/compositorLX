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
    const int spectral = std::clamp(int(std::clamp(std::ceil(distance * 2), 0.0, 47.0)) + 1, 1, 48);
    const int smear = blur > 0 ? std::clamp(int(std::clamp(std::ceil(blur / 3), 2.0, 24.0)), 2, 24) : 1;
    const auto smooth = [](double a, double b, double x) {
        const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
        return t * t * (3 - 2 * t);
    };
    #pragma omp parallel for if(width * height > 16384)
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const QPointF at(x + .5, y + .5);
            const QPointF axis = radial ? (at - center) / std::max(1.0, reach) : direction;
            const double hash = std::sin(x * 12.9898 + y * 78.233) * 43758.5453;
            const double jitter = hash - std::floor(hash);
            std::array<double, 3> color{}, alpha{}, total{};
            for (int i = 0; i < spectral; ++i) {
                const double t = spectral == 1 ? 0 : (i + jitter) / spectral * 2 - 1;
                const std::array<double, 3> weight{smooth(-.3, 1, t), 1 - smooth(0, 1, std::abs(t)) + .05, smooth(-.3, 1, -t)};
                for (int j = 0; j < smear; ++j) {
                    const double u = smear == 1 ? 0 : (j + jitter) / smear;
                    const QPointF q = at - axis * (distance * t + blur * u);
                    std::array<double, 4> sample{};
                    if (!((q.x() < 0 && !(sides & 1)) || (q.x() > width && !(sides & 2)) ||
                          (q.y() < 0 && !(sides & 4)) || (q.y() > height && !(sides & 8)))) {
                        const double sx = std::clamp(q.x() - .5, 0.0, double(width - 1));
                        const double sy = std::clamp(q.y() - .5, 0.0, double(height - 1));
                        const int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
                        const double fx = sx - x0, fy = sy - y0;
                        for (int c = 0; c < 4; ++c)
                            sample[c] = (pixels[y0 * stride + x0 * 4 + c] * (1 - fx) + pixels[y0 * stride + x1 * 4 + c] * fx) * (1 - fy)
                                      + (pixels[y1 * stride + x0 * 4 + c] * (1 - fx) + pixels[y1 * stride + x1 * 4 + c] * fx) * fy;
                    }
                    for (int c = 0; c < 3; ++c) { color[c] += weight[c] * sample[c]; alpha[c] += weight[c] * sample[3]; total[c] += weight[c]; }
                }
            }
            const double a = std::max({alpha[0] / total[0], alpha[1] / total[1], alpha[2] / total[2]});
            for (int c = 0; c < 3; ++c) output[y * outStride + x * 4 + c] = float(color[c] / total[c]);
            output[y * outStride + x * 4 + 3] = float(a);
        }
    }
    return result.convertToFormat(image.format());
}

QImage applyFilterLayer(const QImage &image, const QJsonObject &adjustment, const QPointF &origin, double unitsPerPixel)
{
    const auto settings = adjustment.value("filterLayerSettings").toObject();
    const QString kind = adjustment.value("kind").toString();
    const double scale = std::max(.000001, unitsPerPixel);
    const auto point = [&](const char *name, QPointF fallback) {
        const QJsonArray a = settings.value(QLatin1String(name)).toArray();
        return a.size() == 2 ? (QPointF(a[0].toDouble(), a[1].toDouble()) - origin) / scale : fallback;
    };
    if (kind == "Progressive Blur") {
        ProgressiveBlurSettings s;
        s.from = settings.value("progressiveFrom").toDouble() / scale;
        s.to = settings.value("progressiveTo").toDouble(20) / scale;
        s.start = point("progressiveStart", QPointF(image.width() / 2.0, image.height() * .35));
        s.end = point("progressiveEnd", QPointF(image.width() / 2.0, image.height()));
        s.radial = settings.value("progressiveRadial").toBool();
        s.inverted = settings.value("progressiveInverted").toBool();
        return s.apply(image);
    }
    if (kind == "Bloom / Glow") return RasterOperations::bloomGlow(image, settings.value("bloomAmount").toDouble(40), settings.value("bloomRadius").toDouble(24) / scale);
    RgbSplitSettings s;
    s.distance = settings.value("rgbSplitDistance").toDouble(10) / scale;
    s.blur = settings.value("rgbSplitBlur").toDouble() / scale;
    s.angle = settings.value("rgbSplitAngle").toDouble();
    s.radial = settings.value("rgbSplitRadial").toBool();
    s.center = point("rgbSplitCenter", QPointF(image.width() / 2.0, image.height() / 2.0));
    s.reach = settings.value("rgbSplitReach").toDouble(1000) / scale;
    return s.apply(image, 15);
}
}
