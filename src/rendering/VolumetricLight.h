#pragma once
#include "rendering/GpuRenderer.h"
#include <QColor>
#include <QImage>
#include <QPointF>
#include <optional>
#include <vector>

namespace compositor {
// Filter › Volumetric Light (mac VolumetricLight.swift, 1405545 … 851edb0): a light placed on the image, its beams
// through the air and the haze it lights. `center` is where the light is, in document coordinates, set by clicking the
// canvas; the panel opens with it on the layer's biggest bright area. The rest are 0–100.
struct VolumetricLightSettings {
    std::optional<QPointF> center;
    QColor color = QColor::fromRgbF(1, .86f, .68f);
    double intensity = 30, length = 0, haze = 60, size = 20, shafts = 0;
    // How far into the scene the haze begins, by the photo's own spread of depths.
    double distance = 30;
    // How far bunched-up depths are spread apart (DepthMap::equalized); 0 keeps the model's own spacing.
    double depthContrast = 0;
    // On, the subject stands clear of the haze; off, it takes its own depth and its share of the haze.
    bool keepsSubjectClear = true;
    [[nodiscard]] bool isIdentity() const { return intensity == 0 && haze == 0; }
    [[nodiscard]] VolumetricLightSettings normalized() const;
    bool operator==(const VolumetricLightSettings &) const = default;
};

// A photo's depth from DepthMini: `width` × `height` values, rows top-down, 1 nearest and 0 farthest (mac DepthMap).
struct DepthMap {
    std::vector<float> values;
    int width = 0, height = 0;
    // The depths spread out, `amount` (0–1) of the way to equalized: each moves toward its rank among them all.
    [[nodiscard]] DepthMap equalized(float amount) const;
};

namespace DepthModel {
// DepthMini (macOS Compositor's depth model, distilled from Depth Anything V2 Small), converted from its Core ML
// program to ONNX (scripts/convert-depth-model.py) and run by ONNX Runtime. Shipped with the app, so it never waits.
QString modelPath();
bool isAvailable();
// The model's input sizes, width × height: square, 4:3 and 16:9 either way up; and the one nearest `size`'s shape.
QSize shape(QSize size);
// `image`'s depth: the model's map, normalized, enlarged to 640 on its longer side and pulled onto the photo's edges
// by a guided filter, as macOS does. Kept for the last image. None without the model.
std::optional<DepthMap> depth(const QImage &image, QString *error = nullptr);
} // namespace DepthModel

namespace VolumetricLighting {
// Where the light starts: the biggest bright area of `image`, at its brightest, in its pixels (rows top-down).
QPointF brightestPoint(const QImage &image);
// Where the haze begins and where it's full, as distance (0 nearest, 1 farthest), for Distance: read off the photo's
// own sorted distances when there's depth, else guessed.
std::pair<float, float> ramp(double distance, const std::vector<float> *distances);
// The composed depth the haze goes by, white near, `size` pixels (mac composedDepth), from the model's map and the
// subject's outline (0–1 gray, `size`; null for none).
std::vector<float> composedDepth(const DepthMap &predicted, const std::vector<float> *subject, QSize size, bool keepsSubjectClear);
// Extract Depth: the depth the haze goes by as an opaque gray picture the size of `image`. Null without the model.
QImage depthMap(const QImage &image, const VolumetricLightSettings &settings, QString *error = nullptr);
// `image` with the light at `light` (in its pixels, rows top-down), as macOS works it out. What doesn't change as the
// sliders move is kept for the last image (its glows, air, subject, depth and beams). The beams gather on Vulkan when
// the renderer allows it, else on every CPU core, which draws the same; `accelerated` reports which.
QImage apply(const QImage &image, const VolumetricLightSettings &settings, QPointF light,
             RenderBackend backend = RenderBackend::Auto, bool *accelerated = nullptr);
// The beams alone at the given half size, for comparing the two backends: `small` the photo's half-size copy in linear
// light, `subject` its half-size coverage (gray).
std::vector<float> beams(const QImage &small, const std::vector<float> &subject, float brightest, QPointF light, double side,
                         const VolumetricLightSettings &settings, RenderBackend backend, bool *accelerated = nullptr);
} // namespace VolumetricLighting
} // namespace compositor
