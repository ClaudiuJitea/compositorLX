#pragma once

#include <QImage>
#include <QRect>
#include <cstdint>
#include <optional>

namespace compositor {

enum class TrimBasedOn {
    TransparentPixels,
    TopLeftPixelColor,
    BottomRightPixelColor
};

struct TrimOptions {
    TrimBasedOn basedOn = TrimBasedOn::TransparentPixels;
    bool top = true;
    bool bottom = true;
    bool left = true;
    bool right = true;
    uint8_t tolerance = 0;

    [[nodiscard]] bool trimsAny() const {
        return top || bottom || left || right;
    }

    bool operator==(const TrimOptions &other) const = default;
};

class ImageTrim {
public:
    /// Calculates the crop rectangle in document/image coordinates to trim according to the specified options.
    /// Returns std::nullopt if no non-trimmed content remains (e.g. fully transparent or single solid color)
    /// or if no edges are selected to trim.
    static std::optional<QRect> calculateTrimRect(const QImage &image, const TrimOptions &options);

    /// Crops a QImage directly using the calculated trim rectangle.
    static std::optional<QImage> trimImage(const QImage &image, const TrimOptions &options);
};

} // namespace compositor
