#include "rendering/LayerEffectsRenderer.h"
#include "rendering/LayerRenderer.h"

#include <QColor>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

void boxPass1D(const float *src, float *dst, int lines, int count, int lineStep, int elementStep, int r)
{
    if (r <= 0) {
        for (int l = 0; l < lines; ++l) {
            for (int i = 0; i < count; ++i) {
                dst[l * lineStep + i * elementStep] = src[l * lineStep + i * elementStep];
            }
        }
        return;
    }
    const float scale = 1.0f / float(2 * r + 1);
    for (int l = 0; l < lines; ++l) {
        const int base = l * lineStep;
        double sum = 0.0;
        for (int i = -r; i <= r; ++i) {
            const int clamped = std::clamp(i, 0, count - 1);
            sum += src[base + clamped * elementStep];
        }
        for (int center = 0; center < count; ++center) {
            dst[base + center * elementStep] = float(sum * scale);
            const int leaving = std::clamp(center - r, 0, count - 1);
            const int entering = std::clamp(center + r + 1, 0, count - 1);
            sum += double(src[base + entering * elementStep]) - double(src[base + leaving * elementStep]);
        }
    }
}

void gaussianBlurFloats(std::vector<float> &buffer, int width, int height, double sigma)
{
    if (sigma <= 0.0 || buffer.empty() || width <= 0 || height <= 0) return;
    sigma = std::clamp(sigma, 0.05, 500.0);
    if (sigma <= 16.0) {
        // A true Gaussian (edge pixels repeated, as Core Image's clamped blur does): three box passes have no tail, which
        // lightens a glow's falloff by a few percent against the macOS render.
        const int radius = std::max(1, int(std::ceil(sigma * 3.0)));
        std::vector<float> kernel(size_t(radius) * 2 + 1);
        double total = 0.0;
        for (int i = -radius; i <= radius; ++i) { const double w = std::exp(-(i * i) / (2.0 * sigma * sigma)); kernel[size_t(i + radius)] = float(w); total += w; }
        for (float &k : kernel) k = float(k / total);
        std::vector<float> temp(buffer.size());
        const auto sweep = [&](const float *src, float *dst, int lines, int count, int lineStep, int elementStep) {
            for (int l = 0; l < lines; ++l) {
                const int base = l * lineStep;
                for (int i = 0; i < count; ++i) {
                    double sum = 0.0;
                    for (int k = -radius; k <= radius; ++k) sum += double(kernel[size_t(k + radius)]) * src[base + std::clamp(i + k, 0, count - 1) * elementStep];
                    dst[base + i * elementStep] = float(sum);
                }
            }
        };
        sweep(buffer.data(), temp.data(), height, width, width, 1);
        sweep(temp.data(), buffer.data(), width, height, 1, width);
        return;
    }
    // Large sigmas: more box passes follow a Gaussian's tail closely (six stay within about a level of it).
    const int passes = 6;
    const double ideal = std::sqrt(12.0 * sigma * sigma / passes + 1.0);
    int lower = int(std::floor(ideal));
    if (!(lower & 1)) --lower;
    const int upper = lower + 2;
    const int lowerCount = qRound((12.0 * sigma * sigma - passes * lower * lower - 4.0 * passes * lower - 3.0 * passes) / (-4.0 * lower - 4.0));

    std::vector<float> temp(buffer.size());
    for (int pass = 0; pass < passes; ++pass) {
        const int w = (pass < lowerCount) ? lower : upper;
        const int boxRadius = std::max(0, (w - 1) / 2);
        boxPass1D(buffer.data(), temp.data(), height, width, width, 1, boxRadius);
        boxPass1D(temp.data(), buffer.data(), width, height, 1, width, boxRadius);
    }
}

struct CacheEntry {
    qint64 imageKey = 0;
    qint64 maskKey = 0;
    LayerEffects effects;
    QImage result;
    double inset = 0.0;
};

static QMutex s_cacheMutex;
static std::vector<CacheEntry> s_cache;

} // namespace

double LayerEffectsRenderer::margin(const LayerEffects &effects)
{
    const LayerEffects v = effects.visible();
    double m = 0.0;
    if (v.stroke && !v.stroke->inside) {
        m = std::max(m, v.stroke->size);
    }
    if (v.shadow) {
        m = std::max(m, v.shadow->distance + v.shadow->blur * 3.0);
    }
    if (v.outerGlow) {
        m = std::max(m, v.outerGlow->size * 3.0);
    }
    return std::ceil(m) + 2.0;
}

LayerTransform LayerEffectsRenderer::placed(const LayerTransform &transform, const QSize &imageSize, double inset)
{
    LayerTransform grown = transform;
    const double width = imageSize.width();
    const double height = imageSize.height();
    if (width <= inset * 2.0 || height <= inset * 2.0) {
        return transform;
    }
    grown.size = QSizeF(transform.size.width() * width / (width - inset * 2.0),
                        transform.size.height() * height / (height - inset * 2.0));
    grown.origin = QPointF(transform.center().x() - grown.size.width() / 2.0,
                           transform.center().y() - grown.size.height() / 2.0);
    return grown;
}

std::vector<float> LayerEffectsRenderer::extreme(const std::vector<float> &source, int width, int height, int reach, bool smallest)
{
    if (width <= 0 || height <= 0 || source.size() != size_t(width * height)) return {};
    const int radius = std::max(0, reach);
    std::vector<float> pass(source.size(), 0.0f);
    std::vector<float> result(source.size(), 0.0f);

    const auto sweep = [radius, smallest](const float *input, float *output, int lines, int count, int lineStep, int elementStep) {
        std::vector<int> queue(count);
        for (int line = 0; line < lines; ++line) {
            const int base = line * lineStep;
            int head = 0, tail = 0, next = 0;
            for (int center = 0; center < count; ++center) {
                while (next <= std::min(count - 1, center + radius)) {
                    const float value = input[base + next * elementStep];
                    while (tail > head) {
                        const float previous = input[base + queue[tail - 1] * elementStep];
                        if (smallest ? (previous < value) : (previous > value)) break;
                        --tail;
                    }
                    queue[tail++] = next++;
                }
                while (head < tail && queue[head] < center - radius) {
                    ++head;
                }
                const bool outside = (center < radius || center + radius >= count);
                output[base + center * elementStep] = (smallest && outside) ? 0.0f : input[base + queue[head] * elementStep];
            }
        }
    };

    sweep(source.data(), pass.data(), height, width, width, 1);
    sweep(pass.data(), result.data(), width, height, 1, width);
    return result;
}

std::vector<float> LayerEffectsRenderer::coverage(const QImage &image, const QRectF &rect, const QSize &canvasSize, double blur)
{
    const int width = canvasSize.width();
    const int height = canvasSize.height();
    if (width <= 0 || height <= 0) return {};

    std::vector<float> levels(width * height, 0.0f);
    if (image.isNull()) return levels;

    QImage sharp(canvasSize, QImage::Format_ARGB32_Premultiplied);
    sharp.fill(Qt::transparent);
    {
        QPainter p(&sharp);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(rect, image);
    }

    for (int y = 0; y < height; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(sharp.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            levels[y * width + x] = qAlpha(line[x]) / 255.0f;
        }
    }

    if (blur > 0.0) {
        gaussianBlurFloats(levels, width, height, blur / 2.0);
    }

    return levels;
}

std::vector<float> LayerEffectsRenderer::outerGlowCoverage(const QImage &image, const QRectF &placed, const QSize &canvasSize, const OuterGlowEffect &glow)
{
    const int width = canvasSize.width();
    const int height = canvasSize.height();
    const auto shape = coverage(image, placed, canvasSize, 0.0);
    auto soft = coverage(image, placed, canvasSize, glow.size);
    const int total = width * height;
    for (int i = 0; i < total; ++i) {
        soft[i] = std::clamp(soft[i] * (1.0f - shape[i]), 0.0f, 1.0f);
    }
    return soft;
}

std::vector<float> LayerEffectsRenderer::innerGlowCoverage(const QImage &image, const QRectF &placed, const QSize &canvasSize, const InnerGlowEffect &glow)
{
    const int width = canvasSize.width();
    const int height = canvasSize.height();
    auto shape = coverage(image, placed, canvasSize, 0.0);
    const auto blurred = coverage(image, placed, canvasSize, glow.size);
    const int total = width * height;
    for (int i = 0; i < total; ++i) {
        shape[i] = std::clamp(shape[i] * (1.0f - blurred[i]), 0.0f, 1.0f);
    }
    return shape;
}

std::vector<float> LayerEffectsRenderer::strokeCoverage(const QImage &image, const QRectF &placed, const QSize &canvasSize, const StrokeEffect &stroke)
{
    const int width = canvasSize.width();
    const int height = canvasSize.height();
    auto shape = coverage(image, placed, canvasSize, 0.0);
    const int reach = std::max(1, int(std::round(stroke.size)));
    const auto moved = extreme(shape, width, height, reach, stroke.inside);
    const int total = width * height;
    for (int i = 0; i < total; ++i) {
        shape[i] = stroke.inside ? std::max(0.0f, shape[i] - moved[i])
                                 : std::max(0.0f, moved[i] - shape[i]);
    }
    return shape;
}

std::vector<float> LayerEffectsRenderer::shadowCoverage(const QImage &image, const QRectF &placed, const QSize &canvasSize, const ShadowEffect &shadow)
{
    const QPointF offset = shadow.offset();
    const QRectF offsetRect = placed.translated(offset);
    return coverage(image, offsetRect, canvasSize, shadow.blur);
}

std::vector<float> LayerEffectsRenderer::innerCoverage(const QImage &image, const QRectF &placed, const QSize &canvasSize, const InnerShadowEffect &shadow)
{
    const int width = canvasSize.width();
    const int height = canvasSize.height();
    auto shape = coverage(image, placed, canvasSize, 0.0);
    const QPointF offset = shadow.offset();
    const QRectF offsetRect = placed.translated(offset);
    const auto moved = coverage(image, offsetRect, canvasSize, shadow.blur);
    const int total = width * height;
    for (int i = 0; i < total; ++i) {
        shape[i] = std::clamp(shape[i] * (1.0f - moved[i]), 0.0f, 1.0f);
    }
    return shape;
}

std::pair<QImage, double> LayerEffectsRenderer::render(const QImage &image, const QImage &mask, const LayerEffects &effects)
{
    const LayerEffects visible = effects.visible();
    if (visible.isEmpty() || !visible.isValid() || image.isNull()) {
        return {image, 0.0};
    }
    const double inset = margin(visible);
    const int width = image.width() + int(inset) * 2;
    const int height = image.height() + int(inset) * 2;
    if (width <= 0 || height <= 0 || qint64(width) * height > 80000000LL) {
        return {image, 0.0};
    }

    const QRectF placedRect(inset, inset, image.width(), image.height());
    const QSize canvasSize(width, height);

    QImage shown = image;
    if (!mask.isNull()) {
        shown = LayerRenderer::applyMask(image, mask, Sampling::HighQuality);
    }
    shown = shown.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage context(canvasSize, QImage::Format_ARGB32_Premultiplied);
    context.fill(Qt::transparent);

    const auto fill = [&](const QColor &color, double alpha, const std::vector<float> &cov) {
        if (alpha <= 0.0 || cov.empty()) return;
        const double r = color.redF();
        const double g = color.greenF();
        const double b = color.blueF();
        for (int y = 0; y < height; ++y) {
            QRgb *dst = reinterpret_cast<QRgb *>(context.scanLine(y));
            for (int x = 0; x < width; ++x) {
                const float c = cov[y * width + x];
                if (c <= 0.0f) continue;
                const float srcA = std::clamp(float(alpha * c), 0.0f, 1.0f);
                const int sA = qRound(srcA * 255.0f);
                if (sA <= 0) continue;
                const int invA = 255 - sA;
                const int sR = qRound(float(r) * srcA * 255.0f);
                const int sG = qRound(float(g) * srcA * 255.0f);
                const int sB = qRound(float(b) * srcA * 255.0f);

                const QRgb d = dst[x];
                const int dR = qRed(d);
                const int dG = qGreen(d);
                const int dB = qBlue(d);
                const int dA = qAlpha(d);

                const int oR = std::min(255, sR + (dR * invA + 127) / 255);
                const int oG = std::min(255, sG + (dG * invA + 127) / 255);
                const int oB = std::min(255, sB + (dB * invA + 127) / 255);
                const int oA = std::min(255, sA + (dA * invA + 127) / 255);

                dst[x] = qRgba(oR, oG, oB, oA);
            }
        }
    };

    // 1. Drop Shadow
    if (visible.shadow && visible.shadow->opacity > 0.0) {
        const auto alpha = shadowCoverage(shown, placedRect, canvasSize, *visible.shadow);
        const QColor col = QColor::fromRgbF(visible.shadow->red, visible.shadow->green, visible.shadow->blue);
        fill(col, visible.shadow->opacity, alpha);
    }

    // 2. Outer Glow
    if (visible.outerGlow && visible.outerGlow->opacity > 0.0) {
        const auto alpha = outerGlowCoverage(shown, placedRect, canvasSize, *visible.outerGlow);
        const QColor col = QColor::fromRgbF(visible.outerGlow->red, visible.outerGlow->green, visible.outerGlow->blue);
        fill(col, visible.outerGlow->opacity, alpha);
    }

    // 3. Outside Stroke
    if (visible.stroke && !visible.stroke->inside && visible.stroke->size > 0.0 && visible.stroke->opacity > 0.0) {
        const auto alpha = strokeCoverage(shown, placedRect, canvasSize, *visible.stroke);
        const QColor col = QColor::fromRgbF(visible.stroke->red, visible.stroke->green, visible.stroke->blue);
        fill(col, visible.stroke->opacity, alpha);
    }

    // 4. Base Layer Pixels (shown) SourceOver
    {
        QPainter p(&context);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.drawImage(placedRect, shown);
    }

    // 5. Color Overlay
    if (visible.colorOverlay && visible.colorOverlay->opacity > 0.0) {
        const auto shape = coverage(shown, placedRect, canvasSize, 0.0);
        const QColor col = QColor::fromRgbF(visible.colorOverlay->red, visible.colorOverlay->green, visible.colorOverlay->blue);
        fill(col, visible.colorOverlay->opacity, shape);
    }

    // 6. Inner Glow
    if (visible.innerGlow && visible.innerGlow->opacity > 0.0) {
        const auto insideGlow = innerGlowCoverage(shown, placedRect, canvasSize, *visible.innerGlow);
        const QColor col = QColor::fromRgbF(visible.innerGlow->red, visible.innerGlow->green, visible.innerGlow->blue);
        fill(col, visible.innerGlow->opacity, insideGlow);
    }

    // 7. Inner Shadow
    if (visible.innerShadow && visible.innerShadow->opacity > 0.0) {
        const auto insideShadow = innerCoverage(shown, placedRect, canvasSize, *visible.innerShadow);
        const QColor col = QColor::fromRgbF(visible.innerShadow->red, visible.innerShadow->green, visible.innerShadow->blue);
        fill(col, visible.innerShadow->opacity, insideShadow);
    }

    // 8. Inside Stroke
    if (visible.stroke && visible.stroke->inside && visible.stroke->size > 0.0 && visible.stroke->opacity > 0.0) {
        const auto alpha = strokeCoverage(shown, placedRect, canvasSize, *visible.stroke);
        const QColor col = QColor::fromRgbF(visible.stroke->red, visible.stroke->green, visible.stroke->blue);
        fill(col, visible.stroke->opacity, alpha);
    }

    return {context, inset};
}

std::pair<QImage, double> LayerEffectsRenderer::cached(const QImage &image, const QImage &mask, const LayerEffects &effects)
{
    const LayerEffects visible = effects.visible();
    if (visible.isEmpty() || !visible.isValid() || image.isNull()) {
        return {image, 0.0};
    }
    const qint64 imgKey = image.cacheKey();
    const qint64 mskKey = mask.isNull() ? 0 : mask.cacheKey();

    {
        QMutexLocker locker(&s_cacheMutex);
        for (auto it = s_cache.begin(); it != s_cache.end(); ++it) {
            if (it->imageKey == imgKey && it->maskKey == mskKey && it->effects == visible) {
                return {it->result, it->inset};
            }
        }
    }

    auto rendered = render(image, mask, effects);

    {
        QMutexLocker locker(&s_cacheMutex);
        if (s_cache.size() >= 8) {
            s_cache.erase(s_cache.begin());
        }
        s_cache.push_back({imgKey, mskKey, visible, rendered.first, rendered.second});
    }

    return rendered;
}

void LayerEffectsRenderer::clearCache()
{
    QMutexLocker locker(&s_cacheMutex);
    s_cache.clear();
}

} // namespace compositor
