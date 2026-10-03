#pragma once

#include "core/DocumentLimits.h"

#include <QByteArray>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <optional>

namespace compositor {

/// Metadata probed from an SVG root element before full rasterization.
struct SvgInfo {
    QSize intrinsicSize;
    QRectF viewBox;
    bool hasViewBox = false;
    bool hasWidth = false;
    bool hasHeight = false;
};

/// High-performance, memory-bounded SVG importer for CompositorLX.
///
/// Implements SVG rasterization matching macOS Compositor reference semantics:
/// - Fits to canvas when a fitting size is supplied, preserving aspect ratio.
/// - Decodes at natural declared/viewBox size when opening as a standalone document.
/// - Enforces strict file size, dimension, and pixel budget limits prior to allocation.
/// - Enforces sRGB color profile conversion and RGBA8888 premultiplied layer formatting.
class SvgImporter final {
public:
    static constexpr qint64 MaxFileBytes = 512 * 1024 * 1024; // 512 MB file limit
    static constexpr int MaxDimension = 30000;                // DocumentLimits.maxSide

    /// Returns true if the file exists and is recognized as an SVG or SVGZ image.
    [[nodiscard]] static bool matches(const QString &path);

    /// Returns true if the byte buffer matches SVG (XML or gzip SVGZ).
    [[nodiscard]] static bool matchesData(const QByteArray &data, const QString &path = {});

    /// Probes SVG dimensions and viewBox from file without decoding full raster pixels.
    [[nodiscard]] static bool probe(const QString &path, SvgInfo *info = nullptr, QString *error = nullptr);

    /// Probes SVG dimensions and viewBox from byte buffer.
    [[nodiscard]] static bool probeData(const QByteArray &data, SvgInfo *info = nullptr, QString *error = nullptr);

    /// Computes the target raster size for an SVG given an optional canvas fitting size.
    /// Matches macOS: scale = min(fitting.width / svg.width, fitting.height / svg.height) ?? 1.
    [[nodiscard]] static QSize computeTargetSize(const QSize &intrinsicSize, const std::optional<QSize> &fitting);

    /// Decodes an SVG from disk into a premultiplied sRGB QImage with bounds checks.
    [[nodiscard]] static QImage read(const QString &path,
                                     const std::optional<QSize> &fitting = std::nullopt,
                                     qint64 remainingPixels = DocumentLimits::documentPixelBudget(),
                                     QString *error = nullptr);

    /// Decodes an SVG from byte buffer into a premultiplied sRGB QImage with bounds checks.
    [[nodiscard]] static QImage readFromData(const QByteArray &data,
                                             const std::optional<QSize> &fitting = std::nullopt,
                                             qint64 remainingPixels = DocumentLimits::documentPixelBudget(),
                                             QString *error = nullptr);

    /// Parses an SVG viewBox string into a QRectF (minX, minY, width, height).
    [[nodiscard]] static QRectF parseViewBox(QStringView str, bool *ok = nullptr);

    /// Parses an SVG length string (px, pt, in, mm, cm, pc, %) into pixel units.
    [[nodiscard]] static double parseLength(QStringView str, double relativeTo = 0.0, bool *ok = nullptr);
};

} // namespace compositor
