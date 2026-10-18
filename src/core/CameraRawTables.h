#pragma once

#include "core/CameraRaw.h"
#include "AdjustPixels.h"
#include <vector>

namespace compositor::CameraRawTables
{
inline constexpr int size = 17, grid = 33;
struct Brightness {
    double brightest = 128, luminance = 128, logBrightest = 128;
};
enum class Part { All, BeforeShadows, AfterShadows };
const uint8_t *exposureTables();
Brightness brightness(const QImage &image);
std::vector<CameraRawStage> stages(const CameraRawSettings &settings, const Brightness &brightness,
                                   Part part = Part::All);
std::vector<CameraRawStage> mixerStages(const CameraRawMixerSettings &settings);
std::vector<CameraRawStage> gradingStages(const CameraRawGradingSettings &settings);
std::vector<float> compose(const std::vector<CameraRawStage> &stages, bool identity = false);
CameraRawStage whiteBalance(double temperature, double tint);
std::optional<std::pair<double, double>> neutralize(double red, double green, double blue);
void applyShadows(QImage &image, double amount, double scale);
} // namespace compositor::CameraRawTables
