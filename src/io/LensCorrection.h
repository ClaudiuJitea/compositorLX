#pragma once
#include <QImage>
#include <QString>
#include <atomic>

namespace compositor {
struct LensMetadata {
  QString cameraMake, cameraModel, lensMake, lensModel;
  float focalLength = 0, aperture = 0, distance = 1000;
};

namespace LensCorrection {
// A missing or ambiguous profile leaves the image unchanged and keeps the RAW
// control unavailable.
QString profileName(const LensMetadata &metadata);
QImage apply(const QImage &image, const LensMetadata &metadata,
             std::atomic<bool> *cancelled = nullptr);
} // namespace LensCorrection
} // namespace compositor
