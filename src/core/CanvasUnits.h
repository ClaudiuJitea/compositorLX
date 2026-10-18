#pragma once
#include <cmath>
namespace compositor
{
enum class CanvasUnit { Pixels, Inches, Centimeters, Millimeters };
inline double pixelsPerUnit(CanvasUnit unit, double dpi)
{
    switch (unit) {
    case CanvasUnit::Pixels:
        return 1;
    case CanvasUnit::Inches:
        return dpi;
    case CanvasUnit::Centimeters:
        return dpi / 2.54;
    case CanvasUnit::Millimeters:
        return dpi / 25.4;
    }
    return 1;
}
inline int canvasPixels(double value, CanvasUnit unit, double dpi)
{
    if (!std::isfinite(value) || !std::isfinite(dpi) || value <= 0 || dpi < 1 ||
        value * pixelsPerUnit(unit, dpi) > 30000)
        return 0;
    return int(std::round(value * pixelsPerUnit(unit, dpi)));
}
} // namespace compositor
