#pragma once

#include "core/Document.h"
#include "rendering/TextFaces.h"

#include <QAbstractTextDocumentLayout>
#include <QImage>
#include <QPainter>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextBlock>
#include <QTextLayout>
#include <QFontMetricsF>
#include <cmath>

namespace compositor {

// The gap between the text and its box, in layer pixels (macOS LayerTextStyle.padding): the same for point text and a
// fixed box, so turning one into the other doesn't move the text.
inline constexpr double kTextPadding = 12.0;

// Baseline to baseline, in layer pixels. Leading 0 is Auto: 120% of the size (macOS LayerTextStyle.lineHeight).
[[nodiscard]] inline double textLineHeight(double fontSize, double leading) { return leading > 0.0 ? leading : fontSize * 1.2; }
[[nodiscard]] inline double textLineHeight(const TextStyle &style) { return textLineHeight(style.fontSize, style.leading); }

// `leading` here is the line height to use (0 keeps the font's own): every line is exactly that tall, so lines close
// up, and eventually overlap, as it comes down, the way Photoshop's do.
inline void formatTextDocument(QTextDocument &document, const QFont &font, const QColor &color, int alignment, bool wrap, double tracking = 0.0, double leading = 0.0)
{
    document.setDocumentMargin(0);
    QFont styledFont = font;
    if (tracking != 0.0) {
        styledFont.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
    }
    document.setDefaultFont(styledFont);
    QTextOption option = document.defaultTextOption();
    option.setWrapMode(wrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    option.setAlignment(alignment == 1 ? Qt::AlignHCenter : alignment == 2 ? Qt::AlignRight : Qt::AlignLeft);
    document.setDefaultTextOption(option);
    QTextCursor cursor(&document); cursor.select(QTextCursor::Document);
    QTextCharFormat style; style.setFont(styledFont); style.setForeground(color);
    if (tracking != 0.0) {
        style.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        style.setFontLetterSpacing(tracking);
    }
    cursor.mergeCharFormat(style);
    QTextBlockFormat block;
    block.setAlignment(option.alignment());
    if (leading > 0.0) {
        block.setLineHeight(leading, QTextBlockFormat::FixedHeight);
    }
    cursor.mergeBlockFormat(block);
}

// Document-wide layout without per-letter formatting: size, weight, spacing and alignment come from the default
// font and options, so each letter keeps its own color and face (the inline editor uses this).
inline void layoutTextDocument(QTextDocument &document, const QFont &font, int alignment, bool wrap, double leading = 0.0)
{
    document.setDocumentMargin(0);
    document.setDefaultFont(font);
    QTextOption option = document.defaultTextOption();
    option.setWrapMode(wrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    option.setAlignment(alignment == 1 ? Qt::AlignHCenter : alignment == 2 ? Qt::AlignRight : Qt::AlignLeft);
    document.setDefaultTextOption(option);
    QTextCursor cursor(&document); cursor.select(QTextCursor::Document);
    QTextBlockFormat block;
    block.setAlignment(option.alignment());
    if (leading > 0.0) block.setLineHeight(leading, QTextBlockFormat::FixedHeight);
    cursor.mergeBlockFormat(block);
}

inline QColor textRgbColor(const TextStyle::Rgb &rgb) { return QColor::fromRgbF(rgb.r, rgb.g, rgb.b); }

// A letter's own look. The face is a PostScript-style name (see TextFaces.h); it is kept as a property of its own so
// it reads back exactly as it was set, whatever family, weight and slant it resolves to.
inline constexpr int kTextFaceProperty = QTextFormat::UserProperty + 7;

[[nodiscard]] inline QTextCharFormat textRunFormat(const QColor &color, const QString &face)
{
    QTextCharFormat format;
    format.setForeground(color);
    const TextFace resolved = resolveTextFace(face);
    format.setFontFamilies({resolved.family});
    format.setFontWeight(resolved.weight);
    format.setFontItalic(resolved.italic);
    if (!resolved.styleName.isEmpty()) format.setFontStyleName(resolved.styleName);
    else format.setProperty(QTextFormat::FontStyleName, QString());
    format.setProperty(kTextFaceProperty, face);
    return format;
}

inline QString textFaceOf(const QTextCharFormat &format, const QString &fallback)
{
    if (format.hasProperty(kTextFaceProperty)) return format.stringProperty(kTextFaceProperty);
    if (format.hasProperty(QTextFormat::FontFamilies)) {
        const QStringList families = format.property(QTextFormat::FontFamilies).toStringList();
        if (!families.isEmpty()) return families.first();
    }
    if (format.hasProperty(QTextFormat::FontFamily)) return format.stringProperty(QTextFormat::FontFamily);
    return fallback;
}

// Gives every letter of the document its own color and face. Runs that do not fit the document's text
// (a paragraph break the document normalised, say) are ignored rather than misapplied.
inline void applyTextRuns(QTextDocument &document, const TextStyle &style)
{
    if (!style.colorRuns && !style.fontRuns) return;
    const int count = style.content.size();
    if (count == 0 || document.characterCount() - 1 != count) return;
    const QVector<TextStyle::Rgb> colors = style.unitColors();
    const QVector<QString> faces = style.unitFonts();
    QTextCursor cursor(&document);
    for (int start = 0; start < count;) {
        int end = start + 1;
        while (end < count && colors[end] == colors[start] && faces[end] == faces[start]) ++end;
        cursor.setPosition(start); cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(textRunFormat(textRgbColor(colors[start]), faces[start]));
        start = end;
    }
}

// What a point-text box measures: the text plus its padding, with a caret's worth of width so an empty line still has
// somewhere to type (macOS EditorSession.textBoxSize). `document` must already be formatted.
inline QSize pointTextBoxSize(QTextDocument &document, double fontSize, double lineHeight)
{
    document.setTextWidth(-1);
    const double width = std::ceil(document.idealWidth() + kTextPadding * 2 + fontSize * 0.1);
    const double height = std::ceil(std::max(document.size().height(), lineHeight) + kTextPadding * 2);
    return QSize(int(std::max(16.0, width)), int(std::max(16.0, height)));
}

// Where the first baseline of a fixed-height line falls, measured from the top of the layout, as Qt lays it out.
[[nodiscard]] inline double firstLineBaseline(QTextDocument &document)
{
    document.size();   // lays the text out
    const QTextLayout *layout = document.firstBlock().layout();
    if (!layout || layout->lineCount() == 0) return 0.0;
    const QTextLine line = layout->lineAt(0);
    return line.y() + line.ascent();
}

// AppKit puts a line's extra room above the letters: with a fixed line height the baseline sits the font's descent up
// from the bottom of the line. Qt puts it at a fixed fraction of the line. This is how far down to draw so that the
// baseline is where macOS Compositor's is.
[[nodiscard]] inline double baselineShift(QTextDocument &document, double lineHeight)
{
    const double descent = QFontMetricsF(document.defaultFont()).descent();
    return (lineHeight - descent) - firstLineBaseline(document);
}

// Draws the formatted document into a transparent image of `size`, text padded by kTextPadding. Lines that do not fit the
// box's height are not drawn, as a text container drops them.
inline QImage drawTextBox(QTextDocument &document, const QSize &size, double lineHeight)
{
    if (size.width() < 1 || size.height() < 1 || size.width() > 30000 || size.height() > 30000
        || qint64(size.width()) * size.height() > 100000000LL) return {};
    document.setTextWidth(std::max(1.0, size.width() - 2 * kTextPadding));
    const double shift = baselineShift(document, lineHeight);
    QImage image(size, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.translate(kTextPadding, kTextPadding + shift);
    const double room = std::max(0.0, size.height() - 2 * kTextPadding);
    const int lines = std::max(1, int(std::floor(room / std::max(1.0, lineHeight) + 1e-6)));
    const double top = -(kTextPadding + shift);
    document.drawContents(&painter, QRectF(-kTextPadding, top, size.width(), lines * lineHeight + std::max(0.0, shift) - top));
    return image;
}

// Compatibility entry: `box` is the whole box (padding included) for area text, ignored for point text.
inline QImage drawTextDocument(QTextDocument &document, const QSize &box, bool areaText)
{
    const double lineHeight = document.defaultFont().pixelSize() > 0 ? std::max(1.0, document.begin().blockFormat().lineHeight()) : 1.0;
    const double size = std::max(1, document.defaultFont().pixelSize());
    const QSize target = areaText ? box : pointTextBoxSize(document, size, lineHeight);
    return drawTextBox(document, target, lineHeight);
}

inline QImage renderText(const QString &text, const QSize &box, const QFont &font, const QColor &color, int alignment, bool areaText, double tracking = 0.0, double leading = 0.0)
{
    QTextDocument document; document.setPlainText(text);
    formatTextDocument(document, font, color, alignment, areaText, tracking, leading > 0.0 ? leading : font.pixelSize() * 1.2);
    return drawTextDocument(document, box, areaText);
}

// The font a style's whole text is set in: its face at its size, with its tracking.
[[nodiscard]] inline QFont textStyleFont(const TextStyle &style)
{
    QFont font = textFontForFace(style.fontName, style.fontSize);
    if (style.tracking != 0.0) font.setLetterSpacing(QFont::AbsoluteSpacing, style.tracking);
    return font;
}

inline void prepareStyledDocument(QTextDocument &document, const TextStyle &style, bool wrap)
{
    document.setPlainText(style.content);
    formatTextDocument(document, textStyleFont(style), QColor::fromRgbF(style.red, style.green, style.blue),
                       style.alignment == TextAlignment::Center ? 1 : style.alignment == TextAlignment::Right ? 2 : 0,
                       wrap, style.tracking, textLineHeight(style));
    applyTextRuns(document, style);
}

// How big a style's layer is: its box, or what point text measures (macOS EditorSession.textBoxSize).
[[nodiscard]] inline QSize styledTextBoxSize(const TextStyle &style)
{
    if (style.boxSize) return QSize(int(std::ceil(style.boxSize->width())), int(std::ceil(style.boxSize->height())));
    QTextDocument document;
    prepareStyledDocument(document, style, false);
    return pointTextBoxSize(document, style.fontSize, textLineHeight(style));
}

// The layer raster for a style (macOS EditorSession.textImage): transparent, letters in their own colors and faces,
// padded by kTextPadding, sized by its box or by what it measures.
[[nodiscard]] inline QImage renderStyledText(const TextStyle &style)
{
    QTextDocument document;
    prepareStyledDocument(document, style, style.boxSize.has_value());
    return drawTextBox(document, styledTextBoxSize(style), textLineHeight(style));
}

// Renders a style with each letter in its own color and face. `font` carries size, weight and slant (its family
// is the style's base face); tracking and leading come from the style.
inline QImage renderText(const TextStyle &style, const QSize &box, QFont font, int alignment, bool areaText)
{
    if (style.tracking != 0.0) font.setLetterSpacing(QFont::AbsoluteSpacing, style.tracking);
    QTextDocument document; document.setPlainText(style.content);
    formatTextDocument(document, font, QColor::fromRgbF(style.red, style.green, style.blue), alignment, areaText, style.tracking,
                       textLineHeight(font.pixelSize() > 0 ? font.pixelSize() : style.fontSize, style.leading));
    applyTextRuns(document, style);
    return drawTextDocument(document, box, areaText);
}
} // namespace compositor
