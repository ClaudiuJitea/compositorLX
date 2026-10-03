#pragma once

#include <QFont>
#include <QString>

namespace compositor {

// A text layer names its face the way macOS Compositor does: a PostScript-style name such as "Helvetica",
// "Helvetica-Bold" or "ArialMT" (never a Qt family plus a bold flag). This resolves such a name to something Qt can
// draw, and composes one back when the user toggles Bold or Italic, so the name a layer saves is the name a macOS
// file would carry.
struct TextFace {
    QString family;                // The Qt family to draw with (the requested one when it is not installed).
    int weight = QFont::Normal;    // QFont::Weight value.
    bool italic = false;
    QString styleName;             // The installed style the name matched exactly, if any.
    bool installed = false;        // The face (or its family) exists on this system.
    [[nodiscard]] bool bold() const { return weight >= QFont::DemiBold; }
};

[[nodiscard]] TextFace resolveTextFace(const QString &name);

// The font for `face` at `pixelSize` (rounded; Qt takes whole pixels).
[[nodiscard]] QFont textFontForFace(const QString &face, double pixelSize);

// `face` with Bold and Italic set as asked, keeping its family. The plain face of a family is its family name.
[[nodiscard]] QString composeTextFace(const QString &face, bool bold, bool italic);

// Forgets what was learned about the installed fonts (tests that change the font set).
void resetTextFaceCache();

} // namespace compositor
