#pragma once
#include "io/CMYK.h"
#include <optional>
namespace compositor::PrintExporter
{
std::optional<QByteArray> pdf(const QImage &image, double resolution,
                              const std::optional<CMYKProfile> &profile = {}, QString *error = nullptr);
std::optional<QByteArray> tiff(const QImage &image, double resolution,
                               const std::optional<CMYKProfile> &profile = {}, QString *error = nullptr);
std::optional<QByteArray> jpeg(const QImage &image, double resolution, int quality,
                               const CMYKProfile &profile, QString *error = nullptr);
} // namespace compositor::PrintExporter
