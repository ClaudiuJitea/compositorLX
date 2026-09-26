#pragma once

#include "core/Document.h"

#include <QImage>
#include <QRectF>
#include <QSize>

#include <utility>
#include <vector>

namespace compositor {

class LayerEffectsRenderer final {
public:
    static double margin(const LayerEffects &effects);
    static LayerTransform placed(const LayerTransform &transform, const QSize &imageSize, double inset);

    // Coverage generators
    static std::vector<float> coverage(const QImage &image, const QRectF &rect, const QSize &canvasSize, double blur);
    static std::vector<float> outerGlowCoverage(const QImage &image, const QRectF &placed, const QSize &canvasSize, const OuterGlowEffect &glow);
    static std::vector<float> innerGlowCoverage(const QImage &image, const QRectF &placed, const QSize &canvasSize, const InnerGlowEffect &glow);
    static std::vector<float> strokeCoverage(const QImage &image, const QRectF &placed, const QSize &canvasSize, const StrokeEffect &stroke);
    static std::vector<float> shadowCoverage(const QImage &image, const QRectF &placed, const QSize &canvasSize, const ShadowEffect &shadow);
    static std::vector<float> innerCoverage(const QImage &image, const QRectF &placed, const QSize &canvasSize, const InnerShadowEffect &shadow);

    // Extreme filter for morphological dilation (smallest=false) and erosion (smallest=true)
    static std::vector<float> extreme(const std::vector<float> &source, int width, int height, int reach, bool smallest);

    // Full effects rendering on a layer image
    static std::pair<QImage, double> render(const QImage &image, const QImage &mask, const LayerEffects &effects);

    // Cached render
    static std::pair<QImage, double> cached(const QImage &image, const QImage &mask, const LayerEffects &effects);

    // Clear cache
    static void clearCache();
};

} // namespace compositor
