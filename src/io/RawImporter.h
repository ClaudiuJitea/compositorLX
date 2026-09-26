#pragma once

#include "core/CameraRaw.h"

#include <QImage>
#include <QString>
#include <QStringList>
#include <atomic>
#include <optional>

namespace compositor {

struct RawDimensions {
    int rawWidth = 0;
    int rawHeight = 0;
    int visibleWidth = 0;
    int visibleHeight = 0;
    int outputWidth = 0;
    int outputHeight = 0;
    double pixelAspect = 1.0;
    int flip = 0;
};

class RawImporter {
public:
    using RawDimensions = compositor::RawDimensions;

    static constexpr qint64 MaxFileBytes = 512 * 1024 * 1024; // 512 MB
    static constexpr int MaxDimension = 30000;                // 30,000 px
    static constexpr qint64 MaxDecodedPixels = 100000000;     // 100 MP
    static constexpr qint64 MaxOutputPixels = 100000000;      // 100 MP

    static QStringList supportedExtensions();
    static bool matches(const QString &filePath);
    static bool probeDimensions(const QString &filePath, RawDimensions &dims);
    static bool pixelSize(const QString &filePath, int &width, int &height);
    static std::optional<RawDevelopSettings> asShot(const QString &filePath);
    static QImage develop(const QString &filePath, const RawDevelopSettings &settings, int limit = 0,
                          std::atomic<bool> *cancelled = nullptr, qint64 pixelBudget = MaxOutputPixels);
};

} // namespace compositor

