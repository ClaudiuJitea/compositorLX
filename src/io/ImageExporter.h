#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QString>
#include <QUuid>
#include "core/Document.h"

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
    // What Export As starts on (an extension: png, jpg, tiff, gif, exr) for a document not yet exported: the format of
    // the image it began as, so a PNG goes back out as a PNG, otherwise `last`, the format last used (mac startingFormat).
    static QString startingFormat(const QString &sourceImage, const QString &projectPath, const QString &last);
    // The name an export starts with: the project's, or the image the document began as, or Untitled (mac exportName).
    static QString exportName(const QString &projectPath, const QString &sourceImage, const QString &extension);
    // The folder an export or Save As starts in: beside the image the document began as, before it is a project.
    static QString exportFolder(const QString &projectPath, const QString &sourceImage);
    static std::optional<JpegResult> jpeg(const QImage &source, int quality, const QColor &matte,
                                          double resolution, QString *error = nullptr);
    // Export Layers as PNGs (mac 0673a4a ImageExporter.layerPNGData): the layer `id` on its own, as it looks in the
    // document, trimmed to its pixels. None when it has none, as an adjustment layer or an empty one.
    static std::optional<QByteArray> layerPng(const Document &document, const QUuid &id, QString *error = nullptr);
    // `document` with only `id` showing: it, shown even if hidden; what's inside it if it's a folder, and the layers
    // clipped to it, each shown or hidden as it is; and the folders it's in, so their opacity and masks still apply.
    static Document isolating(const Document &document, const QUuid &id);
    // A layer's name as a file's: without the characters a file system can't take, and never empty or hidden.
    static QString layerFileName(const QString &name);
};

} // namespace compositor
