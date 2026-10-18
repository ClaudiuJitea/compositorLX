#include "io/LensCorrection.h"
#include "rendering/DeepImage.h"
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <algorithm>
#include <cmath>
#include <lensfun.h>
#include <memory>
#include <mutex>
#include <vector>

namespace compositor::LensCorrection {
namespace {
const lfDatabase &database() {
  static const auto db = [] {
    auto result = std::make_unique<lfDatabase>();
    if (result->Load() != LF_NO_ERROR) {
#ifdef COMPOSITOR_LENSFUN_DATA_DIR
      const QDir directory(QString::fromUtf8(COMPOSITOR_LENSFUN_DATA_DIR));
      for (const QString &name :
           directory.entryList({QStringLiteral("*.xml")}, QDir::Files))
        result->Load(QFile::encodeName(directory.filePath(name)).constData());
#endif
    }
    return result;
  }();
  return *db;
}
struct Profile {
  const lfCamera *camera = nullptr;
  const lfLens *lens = nullptr;
};
Profile find(const LensMetadata &m) {
  if (m.cameraMake.isEmpty() || m.cameraModel.isEmpty() ||
      m.lensModel.isEmpty() || !std::isfinite(m.focalLength) ||
      m.focalLength <= 0)
    return {};
  const auto &db = database();
  const lfCamera **cameras = db.FindCameras(m.cameraMake.toUtf8().constData(),
                                            m.cameraModel.toUtf8().constData());
  const lfCamera *camera = cameras ? cameras[0] : nullptr;
  lf_free(cameras);
  if (!camera)
    return {};
  const QByteArray maker = m.lensMake.toUtf8(), model = m.lensModel.toUtf8();
  const lfLens **lenses = db.FindLenses(
      camera, maker.isEmpty() ? nullptr : maker.constData(), model.constData());
  // Equal-ranked candidates can describe different lenses; never silently
  // choose one.
  const lfLens *lens = lenses ? lenses[0] : nullptr;
  if (lens && lenses[1] && lenses[1]->Score == lens->Score)
    lens = nullptr;
  lf_free(lenses);
  return {camera, lens};
}
} // namespace
QString profileName(const LensMetadata &metadata) {
  const auto profile = find(metadata);
  if (!profile.lens)
    return {};
  lfModifier modifier(profile.lens, profile.camera->CropFactor, 100, 100);
  const int enabled = modifier.Initialize(
      profile.lens, LF_PF_F32, metadata.focalLength,
      std::max(1.0f, metadata.aperture), std::max(.01f, metadata.distance), 0,
      profile.lens->Type,
      LF_MODIFY_DISTORTION | LF_MODIFY_VIGNETTING | LF_MODIFY_SCALE, false);
  return enabled & (LF_MODIFY_DISTORTION | LF_MODIFY_VIGNETTING)
             ? QString::fromUtf8(lf_mlstr_get(profile.lens->Model))
             : QString();
}
QImage apply(const QImage &image, const LensMetadata &metadata,
             std::atomic<bool> *cancelled) {
  if (image.isNull() || (cancelled && cancelled->load()))
    return {};
  const auto profile = find(metadata);
  if (!profile.lens)
    return image;
  lfModifier modifier(profile.lens, profile.camera->CropFactor, image.width(),
                      image.height());
  const int enabled = modifier.Initialize(
      profile.lens, LF_PF_F32, metadata.focalLength,
      std::max(1.0f, metadata.aperture), std::max(.01f, metadata.distance), 0,
      profile.lens->Type,
      LF_MODIFY_DISTORTION | LF_MODIFY_VIGNETTING | LF_MODIFY_SCALE, false);
  QImage source =
      image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
  if (!source.colorSpace().isValid())
    source.setColorSpace(QColorSpace::SRgb);
  const QColorSpace originalSpace = source.colorSpace();
  source.convertToColorSpace(QColorSpace::SRgbLinear);
  const auto finish = [&](QImage result) {
    result.setColorSpace(QColorSpace::SRgbLinear);
    result.convertToColorSpace(originalSpace);
    return result.convertToFormat(image.format());
  };
  if (enabled & LF_MODIFY_VIGNETTING)
    for (int y = 0; y < source.height(); ++y) {
      if (cancelled && cancelled->load())
        return {};
      modifier.ApplyColorModification(source.scanLine(y), 0, y, source.width(),
                                      1, LF_CR_4(RED, GREEN, BLUE, UNKNOWN),
                                      source.bytesPerLine());
    }
  if (!(enabled & (LF_MODIFY_DISTORTION | LF_MODIFY_SCALE)))
    return finish(source);
  QImage out(source.size(), source.format());
  std::vector<float> coordinates(size_t(source.width()) * 2);
  for (int y = 0; y < out.height(); ++y) {
    if (cancelled && cancelled->load())
      return {};
    if (!modifier.ApplyGeometryDistortion(0, y, out.width(), 1,
                                          coordinates.data()))
      return finish(source);
    auto *row = reinterpret_cast<float *>(out.scanLine(y));
    for (int x = 0; x < out.width(); ++x) {
      const float sx = coordinates[x * 2], sy = coordinates[x * 2 + 1];
      if (!std::isfinite(sx) || !std::isfinite(sy) || sx < 0 || sy < 0 ||
          sx > source.width() - 1 || sy > source.height() - 1) {
        std::fill_n(row + x * 4, 4, 0.f);
        continue;
      }
      const int x0 = int(sx), y0 = int(sy),
                x1 = std::min(x0 + 1, source.width() - 1),
                y1 = std::min(y0 + 1, source.height() - 1);
      const float fx = sx - x0, fy = sy - y0;
      const auto *a = reinterpret_cast<const float *>(source.constScanLine(y0));
      const auto *b = reinterpret_cast<const float *>(source.constScanLine(y1));
      for (int c = 0; c < 4; ++c)
        row[x * 4 + c] =
            (a[x0 * 4 + c] * (1 - fx) + a[x1 * 4 + c] * fx) * (1 - fy) +
            (b[x0 * 4 + c] * (1 - fx) + b[x1 * 4 + c] * fx) * fy;
    }
  }
  return finish(out);
}
} // namespace compositor::LensCorrection
