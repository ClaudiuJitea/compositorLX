#pragma once
#include "rendering/GpuRenderer.h"
#include "rendering/LensEffects.h"
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QRectF>

namespace compositor {
// The filters that came to the Filter menu and to adjustment layers in macOS 1.5.9–1.6.2 (LayerAdjustment.swift
// VignetteLayerSettings, TonalLayerSettings, SharpenLayerSettings, ReduceNoiseLayerSettings, LensEffectSettings), with
// their defaults, ranges and the pixels each makes. Each layer keeps its settings in an object of its own inside the
// adjustment's `filterLayerSettings` (project format 18, 19 and 21).
struct VignetteLayerSettings {
    double amount = 35, midpoint = 50, roundness = 100, feather = 60, highlights = 25;
    QColor color = Qt::black;
    [[nodiscard]] QJsonObject json() const;
    static VignetteLayerSettings fromJson(const QJsonObject &object);
    bool operator==(const VignetteLayerSettings &) const = default;
};
struct TonalLayerSettings {
    double amount = 50, radius = 16, shadows = 40, midtones = 60, highlights = 30;
    [[nodiscard]] QJsonObject json() const;
    static TonalLayerSettings fromJson(const QJsonObject &object);
    bool operator==(const TonalLayerSettings &) const = default;
};
// Filter › Sharpen (mac b0f0b2b): an unsharp mask, the difference from a blur of `radius` pixels (0.1–20) added back,
// `amount` percent of it (0–300).
struct SharpenSettings {
    double amount = 100, radius = 1;
    [[nodiscard]] SharpenSettings normalized() const;
    [[nodiscard]] bool isIdentity() const { return normalized().amount == 0; }
    [[nodiscard]] QJsonObject json() const;
    static SharpenSettings fromJson(const QJsonObject &object);
    bool operator==(const SharpenSettings &) const = default;
};
// Filter › Reduce Noise (mac c1c5356): Camera Raw's noise reduction on its own. Strength smooths the grain in the
// brightness by non-local means, measured against the noise in the picture; Preserve Details asks patches to be more
// alike before they count; Color Noise smooths colored speckle. Each 0–100.
struct ReduceNoiseSettings {
    double strength = 50, details = 50, color = 50;
    [[nodiscard]] ReduceNoiseSettings normalized() const;
    [[nodiscard]] bool isIdentity() const { const auto s = normalized(); return s.strength == 0 && s.color == 0; }
    [[nodiscard]] QJsonObject json() const;
    static ReduceNoiseSettings fromJson(const QJsonObject &object);
    bool operator==(const ReduceNoiseSettings &) const = default;
};

namespace FilterLayers {
// An unsharp mask as Core Image's CIUnsharpMask makes it: on premultiplied colors in linear light, the edges carried on
// past the image's sides; 8-bit, 16-bit and HDR layers alike, returned in the layer's format. Vulkan compute where the
// renderer allows it, otherwise the CPU, which gives the same pixels to within a level.
QImage sharpen(const QImage &image, const SharpenSettings &settings, double scale = 1,
               RenderBackend backend = RenderBackend::Auto, bool *accelerated = nullptr);
// Reduce Noise at `scale` (the preview's size against the layer's): adjust_camera_raw_detail with only its noise
// reduction, whose non-local means runs on Vulkan when it can (installGpuNonlocalMeans).
QImage reduceNoise(const QImage &image, const ReduceNoiseSettings &settings, double scale = 1);
// A Vignette layer: the vignette framing `canvas` (in `image`'s pixels), recoloring only the pixels that are there.
QImage vignetteLayer(const QImage &image, const VignetteLayerSettings &settings, const QRectF &canvas);
// The new kinds' layer rendering, on what is under the layer: `canvas` is the document in `image`'s pixels, which
// Vignette frames and Lens Effects measures itself by. Empty when `adjustment` is none of them.
QImage applyLayer(const QImage &image, const QJsonObject &adjustment, const QRectF &canvas);
// How far a layer of these kinds reads past the pixels it changes, in document pixels (mac samplingMargin).
int reach(const QJsonObject &adjustment);
// Whether the document graph draws this kind itself (Vignette, Tonal Contrast, Sharpen); Lens Effects and Reduce
// Noise run their own GPU work through the layer renderer.
bool drawsOnGraph(const QString &kind);
// The kind's passes on a document graph over a region whose top-left pixel is `origin` on a `canvas`-sized document.
// -1 when `kind` is not one this draws.
int appendLayer(GpuGraph &graph, int input, const QJsonObject &adjustment, QPoint origin, QSize canvas);
// Points the C non-local means (AdjustPixels.c adjust_nonlocal_means_gpu) at Vulkan, as macOS points it at Metal.
// Safe to call more than once.
void installGpuNonlocalMeans();
// The C kernel's non-local means on Vulkan, in bands of rows: false when it did not run (no device, CPU forced).
bool nonlocalMeansGpu(const float *luma, const float *coverage, float *out, int width, int height, int search, int patch,
                      double h, double sigma);
} // namespace FilterLayers
} // namespace compositor
