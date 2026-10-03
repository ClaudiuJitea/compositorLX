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
    // The flattened canvas as PNG: sRGB-tagged, with the document's resolution as pixels-per-meter, premultiplied
    // alpha converted for the file (mac ImageExporter.pngData).
    static std::optional<QByteArray> png(const QImage &source, double resolution, QString *error = nullptr);
    // Bytes to `path` through a temporary sibling that replaces it only when complete (mac ImageExporter.write).
    static bool writeAtomically(const QByteArray &data, const QString &path, QString *error = nullptr);
    static std::optional<JpegResult> jpeg(const QImage &source, int quality, const QColor &matte,
                                          double resolution, QString *error = nullptr);
};

} // namespace compositor
