#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QString>

#include <optional>

namespace compositor {

struct JpegResult {
    QByteArray data;
    QImage preview;
};

class ImageExporter final {
public:
    static std::optional<JpegResult> jpeg(const QImage &source, int quality, const QColor &matte,
                                          double resolution, QString *error = nullptr);
};

} // namespace compositor
