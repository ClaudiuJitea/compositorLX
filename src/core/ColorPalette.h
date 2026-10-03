#pragma once

#include <QColor>
#include <QString>

#include <optional>

namespace compositor {

/// Hue in degrees, saturation and brightness 0...1: the colour picker's source of truth, so the hue survives
/// dragging through grays and black (mac ColorPalette.swift PickerHSB).
struct PickerHSB {
    double hue = 0, saturation = 0, brightness = 0;

    PickerHSB() = default;
    PickerHSB(double h, double s, double b) : hue(h), saturation(s), brightness(b) {}
    explicit PickerHSB(const QColor &color) { setRGB(color); }

    [[nodiscard]] QColor rgb() const;
    /// Updates from RGB while keeping the previous hue for grays and the previous saturation for black, as
    /// Photoshop's field does.
    void setRGB(const QColor &color);
    bool operator==(const PickerHSB &) const = default;
};

namespace palette {
/// Snaps to the 8-bit values painting and export actually store.
[[nodiscard]] QColor quantized(const QColor &color);
/// `RRGGBB`, upper case, no hash.
[[nodiscard]] QString hex(const QColor &color);
/// Accepts `RRGGBB` or shorthand `RGB`, with or without a leading `#`; surrounding spaces are ignored.
[[nodiscard]] std::optional<QColor> fromHex(const QString &text);
}

} // namespace compositor
