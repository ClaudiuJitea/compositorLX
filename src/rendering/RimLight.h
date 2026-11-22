#pragma once
#include "rendering/GpuRenderer.h"
#include <QColor>
#include <QImage>
#include <QUuid>
#include <QVector>
#include <atomic>

namespace compositor {
// One of Rim Light's lights (mac RimLight.swift): where it comes from, in degrees counterclockwise from the right, its
// color, how strong it is, how far it wraps around the edges, and how hard its inner edge is rather than feathered,
// each 0–100.
struct RimLight {
    QUuid id = QUuid::createUuid();
    double angle = 135;
    QColor color = QColor::fromRgbF(1, .86f, .7f);
    double strength = 50, width = 30, choke = 0;
    bool operator==(const RimLight &) const = default;
};

// Filter › Rim Light (mac b9a0ed8, 97c3b3f): up to three lights behind the subject that catch its edges, and how much of
// their light glows in the air around it, 0–100.
struct RimLightSettings {
    static constexpr int maximumLights = 3;
    QVector<RimLight> lights{RimLight()};
    double glow = 10;
    // What the next added light starts as: from the side across from the last, a cool one, then white.
    [[nodiscard]] RimLight nextLight() const;
    [[nodiscard]] bool isIdentity() const;
    [[nodiscard]] RimLightSettings normalized() const;
    bool operator==(const RimLightSettings &) const = default;
};

namespace RimLighting {
// The rim's width as a share of the image's longer side, from Width 0 to 100.
double widthShare(double width);
// The subject, white: the segmentation's mask pulled onto the photo's own edges, as Remove Background's Advanced does,
// so hair and fine edges are lit rather than the blur around them. Kept for the image it was made for, so a slider
// only relights. Null when the model is unavailable (`error` says why).
QImage subject(const QImage &image, QString *error = nullptr, std::atomic<bool> *cancelled = nullptr);
// Whether `image`'s subject has been found already, so lighting it is quick.
bool hasSubject(const QImage &image);
// `image` with its rim lights added, in its own format: the lighting worked out as macOS works it in Core Image, the
// whole chain on Vulkan when the renderer allows it, else on every CPU core, which gives the same pixels.
QImage apply(const QImage &image, const RimLightSettings &settings, RenderBackend backend = RenderBackend::Auto,
             bool *accelerated = nullptr, QString *error = nullptr);
// As apply, with the subject's coverage given (0–1 gray at the image's size), for tests and callers that have it.
QImage applyWithSubject(const QImage &image, const QImage &coverage, const RimLightSettings &settings,
                        RenderBackend backend = RenderBackend::Auto, bool *accelerated = nullptr);
} // namespace RimLighting
} // namespace compositor
