#pragma once
#include "rendering/GpuRenderer.h"
#include <QImage>
#include <QString>
#include <array>

namespace compositor {
// Filter › Lens Effects (macOS LensEffects.swift): what light does inside a camera's lens and the filters in front of
// it. Each effect is made from the picture's own light, in linear light as optics work, so only real light sources
// flare and nothing is pasted on.
enum class LensEffect { AnamorphicFlare, Starburst, Mist, Prism, Bokeh };
// The shape a lens draws its out-of-focus lights in: wide open, a hexagon of blades, an anamorphic lens's tall ovals,
// or clipped into a lemon shape toward the frame's edges.
enum class BokehShape { Round, Hexagon, Oval, CatsEye };

struct LensEffectSettings {
    static constexpr std::array<int, 5> starPointChoices{4, 6, 8, 12, 14};
    static constexpr double bokehSizeMin = .5, bokehSizeMax = 6;
    static constexpr std::array<LensEffect, 5> effects{LensEffect::AnamorphicFlare, LensEffect::Starburst,
                                                       LensEffect::Mist, LensEffect::Prism, LensEffect::Bokeh};
    static constexpr std::array<BokehShape, 4> shapes{BokehShape::Round, BokehShape::Hexagon, BokehShape::Oval,
                                                      BokehShape::CatsEye};

    LensEffect effect = LensEffect::AnamorphicFlare;
    // Anamorphic Flare and Starburst: how bright a light has to be to flare, 50–100%; past 95, how big and solid too.
    double threshold = 90;
    // Anamorphic Flare: strength 0–100, the streak's reach as a percentage of the layer's longer side, and how much
    // the lens's coatings turn the streak blue along its length.
    double flareAmount = 50, flareLength = 60, flareBlue = 80;
    // Starburst: strength, the spikes' reach (percent of the longer side), how many, and their angle in degrees.
    double starAmount = 50, starLength = 12;
    int starPoints = 6;
    double starAngle = 15;
    // Mist: how much light scatters, and how far, 0–100.
    double mistStrength = 50, mistSize = 30;
    // Prism: how strong the copies are, how many, how far apart (percent of the longer side), which way in degrees
    // (counterclockwise from right), copies on both sides rather than one, and how much they're softened.
    double prismAmount = 80, prismCopies = 5, prismOffset = 6, prismAngle = 180;
    bool prismBothWays = false;
    double prismBlur = 0;
    // Bokeh, always a full lens blur: the discs' size (percent of the longer side), how brightly lights open into
    // them, how much of a disc's light gathers at its rim, and the aperture's shape.
    double bokehRadius = 2, bokehBrightness = 50, bokehRing = 100;
    BokehShape bokehShape = BokehShape::Round;

    // Nothing to do: OK closes as Cancel does.
    [[nodiscard]] bool isIdentity() const;
    [[nodiscard]] LensEffectSettings normalized() const;
    // The effect on an 8-bit, 16-bit or HDR layer's pixels, returned in the layer's canonical format. Sizes are
    // proportions of the image, so a smaller preview looks as the result will. Vulkan compute when the renderer allows
    // it (Automatic: a large enough picture), otherwise the CPU, which draws the same; `accelerated` reports the GPU.
    [[nodiscard]] QImage apply(const QImage &image, RenderBackend backend = RenderBackend::Auto,
                               bool *accelerated = nullptr) const;
    bool operator==(const LensEffectSettings &) const = default;

    static QString effectName(LensEffect effect);
    static QString shapeName(BokehShape shape);
};

namespace LensOptics {
// Bokeh's lens blur (macOS MetalBokeh's kernel) on straight float RGBA (Format_RGBA32FPx4*), `radius` in pixels of
// `image`; Vulkan compute when the renderer allows it, otherwise the same loop on the CPU. `accelerated` reports which.
QImage bokehKernel(const QImage &image, double radius, BokehShape shape, double ring,
                   RenderBackend backend = RenderBackend::Auto, bool *accelerated = nullptr);
} // namespace LensOptics
} // namespace compositor
