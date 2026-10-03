#pragma once

#include "core/Document.h"

#include <QByteArray>
#include <QImage>
#include <QMap>
#include <QPointF>
#include <QString>
#include <QStringList>

#include <optional>

namespace compositor {

// Reads a Photoshop 6 type layer ("TySh") into the editor's text model (macOS PSDText.swift; Adobe's Photoshop File
// Formats Specification, Type Tool Object Setting). The engine dictionary inside EngineData supplies the face, size,
// color, tracking, leading and alignment. Anything the model cannot represent (vertical text, shear, uneven scale)
// stays pixels.
struct PSDTextSource {
    TextStyle style;
    QStringList notes;
    QPointF documentAnchor;     // Document point that the image anchor should land on.
    double rotation = 0.0;
    bool flipY = false;
    bool anchorIsFrame = false; // The anchor is the paragraph frame's top-left; otherwise the point-text baseline.
};

struct PSDTextRendered {
    QImage image;
    LayerTransform transform;
};

namespace PSDText {
QString rasterizedNote();
QString firstStyleNote();
QString warpNote();
QString fauxNote();
QString justifyNote();
// Null when the face is installed.
QString missingFontNote(const QString &name);

std::optional<PSDTextSource> parse(const QMap<QString, QByteArray> &extra);
std::optional<PSDTextRendered> render(const PSDTextSource &source);
} // namespace PSDText

} // namespace compositor
