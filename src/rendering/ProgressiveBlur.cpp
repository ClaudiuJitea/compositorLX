#include "rendering/ProgressiveBlur.h"
#include "rendering/GpuOperations.h"
#include "rendering/RasterOperations.h"
#include "rendering/DeepImage.h"
#include <algorithm>
#include <cmath>

QImage ProgressiveBlurSettings::apply(const QImage &image) const
{
    if (image.isNull() || !std::isfinite(from) || !std::isfinite(to) || !std::isfinite(start.x()) ||
        !std::isfinite(start.y()) || !std::isfinite(end.x()) || !std::isfinite(end.y()))
        return {};
    if (auto gpu = compositor::GpuOperations::progressive(image,*this); !gpu.isNull()) return gpu;
    double low = std::clamp(from, 0.0, 250.0), high = std::clamp(to, 0.0, 250.0);
    if (inverted)
        std::swap(low, high);
    const double strongest = std::max(low, high);
    if (strongest == 0)
        return image;
    QPointF delta = end - start;
    if (std::hypot(delta.x(), delta.y()) < 1)
        delta = QPointF(0, 1);
    const double length2 = QPointF::dotProduct(delta, delta);
    // A pyramid of full-precision Gaussian surfaces, interpolated by the radius at each pixel.
    constexpr int levels = 12;
    QImage result(image.size(), QImage::Format_RGBA32FPx4_Premultiplied);
    QImage previous = compositor::DeepImage::floating(image);
    std::vector<double> radii(size_t(image.width()) * image.height());
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QPointF p = QPointF(x + .5, y + .5) - start;
            const double t = std::clamp(radial ? std::hypot(p.x(), p.y()) / std::sqrt(length2)
                                               : QPointF::dotProduct(p, delta) / length2,
                                        0.0, 1.0);
            radii[size_t(y) * image.width() + x] = (low + (high - low) * t) / strongest * levels;
        }
    for (int level = 1; level <= levels; ++level) {
        const QImage blurred = compositor::DeepImage::floating(compositor::DeepImage::gaussianBlur(image, strongest * level / levels));
        for (int y = 0; y < image.height(); ++y) {
            auto *out = reinterpret_cast<float *>(result.scanLine(y));
            const auto *a = reinterpret_cast<const float *>(previous.constScanLine(y)),
                       *b = reinterpret_cast<const float *>(blurred.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const double radius = radii[size_t(y) * image.width() + x];
                if (radius < level - 1 || (radius > level && level < levels))
                    continue;
                const double t = std::clamp(radius - (level - 1), 0.0, 1.0);
                for (int c = 0; c < 4; ++c)
                    out[x * 4 + c] = float(a[x * 4 + c] * (1 - t) + b[x * 4 + c] * t);
            }
        }
        previous = blurred;
    }
    return result.convertToFormat(compositor::DeepImage::canonical(image).format());
}
