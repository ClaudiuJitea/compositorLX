// Type tool, text layers and Photoshop text/vector import (mirrors macOS TypeToolTests, PSDRoundTripTests text and
// vector cases, PSDVectorFixtures). Numbers are the macOS tests' numbers.
#include "core/Document.h"
#include "core/EditorSession.h"
#include "io/PSDReader.h"
#include "io/PSDText.h"
#include "io/PSDVector.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "rendering/LayerRenderer.h"
#include "rendering/TextFaces.h"
#include "rendering/TextLayout.h"
#include "ui/CanvasWidget.h"
#include "ui/InlineTextEditor.h"
#include "ui/MainWindow.h"
#include <QToolButton>

#include <QFontMetricsF>
#include <QTemporaryDir>
#include <QtTest>

using namespace compositor;

namespace {

// ---- PSD fixture helpers (macOS PSDFixture.swift) ----------------------------------------------------------------
struct Buf {
    QByteArray data;
    void u8(int v) { data.append(char(v)); }
    void u16(int v) { u8((v >> 8) & 0xFF); u8(v & 0xFF); }
    void u32(quint32 v) { u16(int(v >> 16)); u16(int(v & 0xFFFF)); }
    void f64(double v) { quint64 b; memcpy(&b, &v, 8); u32(quint32(b >> 32)); u32(quint32(b & 0xFFFFFFFF)); }
    void bytes(const QByteArray &d) { data.append(d); }
    void str(const char *s) { data.append(s); }
    void id(const QString &s) { if (s.size() == 4) { u32(0); data.append(s.toLatin1()); } else { u32(quint32(s.size())); data.append(s.toLatin1()); } }
    void utf16(const QString &s) { u32(quint32(s.size())); for (const QChar c : s) u16(c.unicode()); }
    void descriptor(const QString &classId, const QVector<QPair<QString, QByteArray>> &items)
    {
        u32(16); u32(0); id(classId); u32(quint32(items.size()));
        for (const auto &item : items) { id(item.first); bytes(item.second); }
    }
};

QByteArray textItem(const QString &text) { Buf b; b.str("TEXT"); b.utf16(text); return b.data; }
QByteArray enumItem(const QString &type, const QString &value) { Buf b; b.str("enum"); b.id(type); b.id(value); return b.data; }
QByteArray rawItem(const QByteArray &payload) { Buf b; b.str("tdta"); b.u32(quint32(payload.size())); b.bytes(payload); return b.data; }
struct Box { double l, t, r, b; };
QByteArray rectItem(const Box &box)
{
    Buf b; b.str("Objc"); b.u32(0); b.id(QStringLiteral("Rctn")); b.u32(4);
    const QVector<QPair<QString, double>> sides{{"Left", box.l}, {"Top ", box.t}, {"Rght", box.r}, {"Btom", box.b}};
    for (const auto &side : sides) { b.id(side.first); b.str("UntF"); b.str("#Pnt"); b.f64(side.second); }
    return b.data;
}
QString parenthesized(const QString &text)
{
    QString out = QStringLiteral("(");
    for (const QChar c : text) { if (c == '\\' || c == '(' || c == ')') out += '\\'; out += c; }
    return out + ')';
}

struct TyShOptions {
    QString text = QStringLiteral("Hello");
    QString font = QStringLiteral("Helvetica");
    double fontSize = 24, red = 0, green = 0, blue = 0;
    int justification = 0;
    double tracking = 0;
    std::optional<double> leading;
    bool fauxBold = false, fauxItalic = false, vertical = false, warp = false;
    std::optional<double> secondSize, secondLeading, secondHorizontalScale;
    double tx = 40, ty = 50, xx = 1, xy = 0, yx = 0, yy = 1;
    std::optional<Box> bounds, glyphBounds;
};

QString engineText(const TyShOptions &o)
{
    const auto run = [&](double size, std::optional<double> leading, double horizontal, double vertical) {
        return QStringLiteral("<<\n/StyleSheet\n<<\n/StyleSheetData\n<<\n/Font 0\n/FontSize %1\n/FauxBold %2\n/FauxItalic %3\n/AutoLeading %4\n/Leading %5\n"
                              "/Tracking %6\n/HorizontalScale %7\n/VerticalScale %8\n/FillColor\n<<\n/Type 1\n/Values [ 1.0 %9 %10 %11 ]\n>>\n>>\n>>\n>>")
            .arg(size).arg(o.fauxBold ? "true" : "false").arg(o.fauxItalic ? "true" : "false").arg(leading ? "false" : "true")
            .arg(leading.value_or(size * 1.2)).arg(o.tracking).arg(horizontal).arg(vertical).arg(o.red).arg(o.green).arg(o.blue);
    };
    const bool hasSecond = o.secondSize || o.secondLeading || o.secondHorizontalScale;
    const QString runs = hasSecond
        ? run(o.fontSize, o.leading, 1, 1) + QLatin1Char('\n') + run(o.secondSize.value_or(o.fontSize), o.secondLeading ? o.secondLeading : o.leading, o.secondHorizontalScale.value_or(1), 1)
        : run(o.fontSize, o.leading, 1, 1);
    return QStringLiteral("<<\n/EngineDict\n<<\n/Editor\n<<\n/Text %1\n>>\n/ParagraphRun\n<<\n/RunArray\n[\n<<\n/ParagraphSheet\n<<\n/Properties\n<<\n/Justification %2\n>>\n>>\n>>\n]\n>>\n"
                          "/StyleRun\n<<\n/RunArray\n[\n%3\n]\n>>\n>>\n/ResourceDict\n<<\n/FontSet\n[\n<<\n/Name %4\n>>\n]\n>>\n>>")
        .arg(parenthesized(o.text)).arg(o.justification).arg(runs).arg(parenthesized(o.font));
}

QByteArray tySh(const TyShOptions &o)
{
    Buf block;
    block.u16(1);
    for (const double v : {o.xx, o.xy, o.yx, o.yy, o.tx, o.ty}) block.f64(v);
    block.u16(50);
    QVector<QPair<QString, QByteArray>> items{{"Txt ", textItem(o.text)}, {"Ornt", enumItem(QStringLiteral("Ornt"), o.vertical ? "Vrtc" : "Hrzn")}};
    if (o.bounds) items.append({"bounds", rectItem(*o.bounds)});
    if (o.glyphBounds) items.append({"boundingBox", rectItem(*o.glyphBounds)});
    items.append({"EngineData", rawItem(engineText(o).toLatin1())});
    block.descriptor(QStringLiteral("TxLr"), items);
    block.u16(1);
    block.descriptor(QStringLiteral("warp"), {{"warpStyle", enumItem(QStringLiteral("warpStyle"), o.warp ? "warpArc" : "warpNone")}});
    return block.data;
}

QMap<QString, QByteArray> extraTySh(const TyShOptions &o = {}) { return {{QStringLiteral("TySh"), tySh(o)}}; }

QByteArray unitKey(const char *key, double value, const char *unit = "#Pxl")
{
    QByteArray d(key); d.append("UntF"); d.append(unit);
    Buf b; b.f64(value); return d + b.data;
}
QByteArray originationData(quint32 type, const Box &rect, const QVector<double> &radii = {})
{
    Buf b;
    b.str("keyOriginType"); b.str("long"); b.u32(type);
    b.str("keyOriginShapeBBox");
    b.bytes(unitKey("Left", rect.l)); b.bytes(unitKey("Top ", rect.t)); b.bytes(unitKey("Rght", rect.r)); b.bytes(unitKey("Btom", rect.b));
    if (radii.size() == 4) {
        b.str("keyOriginRRectRadii");
        const char *keys[] = {"topLeft", "topRight", "bottomRight", "bottomLeft"};
        for (int i = 0; i < 4; ++i) b.bytes(unitKey(keys[i], radii[i]));
    }
    return b.data;
}
QByteArray vectorMask(const QSizeF &canvas, const QVector<QPointF> &corners)
{
    Buf b;
    const auto point = [&](const QPointF &p) {
        b.u32(quint32(qint32((p.y() / canvas.height()) * 0x1000000)));
        b.u32(quint32(qint32((p.x() / canvas.width()) * 0x1000000)));
    };
    b.u32(3); b.u32(0);
    b.u16(6); b.bytes(QByteArray(24, 0));
    b.u16(8); b.bytes(QByteArray(24, 0));
    b.u16(0); b.u16(corners.size()); b.bytes(QByteArray(22, 0));
    for (const QPointF &c : corners) { b.u16(1); point(c); point(c); point(c); }
    return b.data;
}
QByteArray colorDescriptor(double r, double g, double b)
{
    QByteArray d("RGBC");
    const auto add = [&d](const char *key, double v) { d.append(key); d.append("doub"); Buf x; x.f64(v); d.append(x.data); };
    add("Rd  ", r); add("Grn ", g); add("Bl  ", b);
    return d;
}
QByteArray strokeStyle(bool fill, bool stroke, double width, double r, double g, double b)
{
    QByteArray d;
    const auto flag = [&d](const char *key, bool v) { d.append(key); d.append("bool"); d.append(char(v ? 1 : 0)); };
    flag("strokeEnabled", stroke); flag("fillEnabled", fill);
    d.append("strokeStyleLineWidth"); d.append("UntF"); d.append("#Pxl");
    Buf w; w.f64(width); d.append(w.data);
    d.append(colorDescriptor(r, g, b));
    return d;
}
QMap<QString, QByteArray> squareVector(const QSizeF &canvas = QSizeF(200, 200))
{
    return {{"vmsk", vectorMask(canvas, {QPointF(120, 30), QPointF(120, 80), QPointF(20, 80), QPointF(20, 30)})},
            {"SoCo", colorDescriptor(0, 110, 255)}, {"vstk", strokeStyle(true, false, 1, 255, 255, 0)}};
}

QByteArray be16(int v) { QByteArray d; d.append(char(v >> 8)); d.append(char(v)); return d; }
QByteArray be32(quint32 v) { return be16(int(v >> 16)) + be16(int(v & 0xFFFF)); }

// A one-layer 8-bit RGB PSD whose layer carries `extras`; `pixels` is a solid color for the layer's own pixels.
QByteArray layerFile(int canvasW, int canvasH, const QRect &rect, const QColor &pixels, const QMap<QString, QByteArray> &extras,
                     const QString &name = QStringLiteral("Layer"), bool withPixels = true)
{
    QByteArray file("8BPS");
    file += be16(1) + QByteArray(6, 0) + be16(3) + be32(quint32(canvasH)) + be32(quint32(canvasW)) + be16(8) + be16(3);
    file += be32(0) + be32(0);   // color mode data, image resources
    const int w = withPixels ? rect.width() : 0, h = withPixels ? rect.height() : 0;
    QByteArray records = be16(1);
    records += be32(quint32(withPixels ? rect.top() : 0)) + be32(quint32(withPixels ? rect.left() : 0))
             + be32(quint32(withPixels ? rect.top() + h : 0)) + be32(quint32(withPixels ? rect.left() + w : 0));
    const int ids[] = {-1, 0, 1, 2};
    records += be16(4);
    QByteArray payload;
    for (const int id : ids) {
        QByteArray ch = be16(0);
        const char value = char(id == 0 ? pixels.red() : id == 1 ? pixels.green() : id == 2 ? pixels.blue() : pixels.alpha());
        ch += QByteArray(w * h, value);
        records += be16(id & 0xFFFF) + be32(quint32(ch.size()));
        payload += ch;
    }
    records += "8BIMnorm" + QByteArray("\xff\x00\x00\x00", 4);
    QByteArray extra = be32(0) + be32(0);
    QByteArray nameBytes = name.toUtf8();
    extra += char(nameBytes.size()) + nameBytes;
    extra += QByteArray((4 - ((nameBytes.size() + 1) % 4)) % 4, 0);
    for (auto it = extras.cbegin(); it != extras.cend(); ++it) {
        extra += "8BIM" + it.key().leftJustified(4, ' ').left(4).toLatin1() + be32(quint32(it.value().size())) + it.value();
        if (it.value().size() % 2) extra += char(0);
    }
    records += be32(quint32(extra.size())) + extra + payload;
    QByteArray info = be32(quint32(records.size())) + records;
    file += be32(quint32(info.size())) + info;
    file += be16(0) + QByteArray(canvasW * canvasH * 3, char(255));
    return file;
}

int inkPixels(const QImage &image, int minAlpha = 128)
{
    int n = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) if (image.pixelColor(x, y).alpha() >= minAlpha) ++n;
    return n;
}

QRect inkBounds(const QImage &image, int minAlpha = 128)
{
    int l = image.width(), t = image.height(), r = -1, b = -1;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).alpha() >= minAlpha) { l = std::min(l, x); r = std::max(r, x); t = std::min(t, y); b = std::max(b, y); }
    return r < 0 ? QRect() : QRect(l, t, r - l + 1, b - t + 1);
}

TextStyle style(const QString &content, const QString &face = QStringLiteral("DejaVu Sans"), double size = 40)
{
    TextStyle s; s.content = content; s.fontName = face; s.fontSize = size; return s;
}

} // namespace

class TestTextLayers : public QObject
{
    Q_OBJECT
private slots:
    // TypeToolTests
    void createEditCancelAndUndo();
    void transformsDuplicatesAndClippingKeepTextEditable();
    void saveReopenAndRasterize();
    void rasterHasTransparentBackgroundAndColoredGlyphs();
    void clippingToTextExportsColoredGlyphsOnTransparency();
    void paragraphBoxKeepsEditableTextWithItsBox();
    void emptyNewParagraphIsDiscarded();
    void invalidDraftsDoNotChangeDocument();
    void clickPlacesFirstBaselineAndMetricsMatchMac();
    void leadingIsBaselineToBaseline();
    void boxResizeReflowsWithoutScalingFont();
    void textStyleRoundTripThroughEditSurvivesScale();
    // Faces
    void postScriptNamesResolveToWeights();
    void boldFaceDrawsHeavierAndKeepsItsNameThroughSave();
    void boldItalicToggleComposesFaceNames();
    void editingAMacFaceDoesNotRenameIt();
    // Text layer behaviour in the rest of the app
    void mergeAndFlattenRasterizeText();
    void canvasSizeAndFlipKeepTextMetadata();
    void textWithEffectsStaysEditable();
    void fillWithForegroundRecolorsLiveText();
    // PSD
    void photoshopPointTextImportsAsEditableText();
    void photoshopTextSizeUsesMatrixScaleNotDocumentResolution();
    void photoshopTextKeepsTheFirstStyleAndReportsTheRest();
    void photoshopTextReportsLeadingOnlyStyleDifferences();
    void photoshopParagraphTextKeepsItsBox();
    void oversizedPhotoshopParagraphFrameStaysPixels();
    void warpedPhotoshopTextStaysEditableAndSaysSo();
    void verticalOrBrokenPhotoshopTextStaysPixels();
    void missingPhotoshopFontIsReportedButStaysEditable();
    void rotatedPhotoshopTextKeepsItsAngle();
    void importedPhotoshopTextRendersAtItsPlace();
    void importedPostScriptBoldRendersBold();
    void vectorMaskIsRasterizedWithFillAndStroke();
    void fillEllipseImportsAsALiveShape();
    void strokedRectangleImportsAsALiveShapeAndReportsTheStroke();
    void fourSharpCornersInferARectangleWithoutOrigination();
    void hugeOriginationSizeIsRejectedWithoutTrapping();
    void nonFiniteOriginationSizeIsIgnored();
    void hugeStrokeWidthIsRejectedWithoutTrapping();
    void liveShapeImportSavesAndReloads();
    // Type tool on the canvas
    void textHitTestFollowsRotationVisibilityAndFolders();
    void eightHandlesResizeTheBoxAndReflow();
    void overflowMarkerShowsWhenTextDoesNotFit();
    void mixedFacesShareOneBaseline();
    void copyAndCopyMergedIncludeTextPixels();
    void copiedTextLayersPromoteTheFormat();
    void boldAndItalicButtonsSetTheFaceOfNewAndExistingText();
};

// ---- TypeToolTests ---------------------------------------------------------------------------------------------

void TestTextLayers::createEditCancelAndUndo()
{
    EditorSession session;
    session.createDocument(800, 600);
    QVERIFY(session.addText(style(QStringLiteral("Text")), QRectF(18, 28, 0, 0), false, false, false, false));
    const QUuid id = session.activeLayer()->id;
    QVERIFY(session.activeLayer()->text.has_value());
    QCOMPARE(session.activeLayer()->name, QStringLiteral("Text"));
    // The layer is named after its first words, on one line.
    TextStyle two = style(QStringLiteral("Hello\n  Compositor"));
    two.fontSize = 48;
    QVERIFY(session.addText(two, QRectF(18, 28, 0, 0), false, false, false, false));
    QCOMPARE(session.activeLayer()->name, QStringLiteral("Hello Compositor"));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(18, 28));
    const QUuid second = session.activeLayer()->id;

    TextStyle changed = *session.activeLayer()->text;
    changed.content = QStringLiteral("Changed");
    QVERIFY(session.updateText(second, changed, QRectF(18, 28, 0, 0), false, false, false, false));
    QCOMPARE(session.activeLayer()->text->content, QStringLiteral("Changed"));
    session.undo();
    QCOMPARE(session.activeLayer()->text->content, QStringLiteral("Hello\n  Compositor"));
    session.undo();
    QVERIFY(session.document()->layers.last().id == id);
    session.redo();
    QCOMPARE(session.activeLayer()->id, second);
    // An edit that changes nothing is not an undo step.
    const int layersBefore = session.document()->layers.size();
    QVERIFY(session.updateText(second, *session.activeLayer()->text, session.activeLayer()->transform.origin.isNull() ? QRectF() : QRectF(session.activeLayer()->transform.origin, session.activeLayer()->transform.size), false, false, false, false));
    QCOMPARE(session.document()->layers.size(), layersBefore);
    session.undo();
    QCOMPARE(session.document()->layers.last().id, id);   // that one step was the New Text Layer, not a no-op edit
}

void TestTextLayers::transformsDuplicatesAndClippingKeepTextEditable()
{
    EditorSession session;
    session.createDocument(800, 600);
    QVERIFY(session.addText(style(QStringLiteral("Text")), QRectF(20, 20, 0, 0), false, false, false, false));
    const QUuid id = session.activeLayer()->id;
    Layer &layer = *session.activeLayer();
    layer.transform.rotation = 30;
    layer.transform.size.setWidth(layer.transform.size.width() * 2);
    const LayerTransform old = layer.transform;
    TextStyle longer = *layer.text;
    longer.content = QStringLiteral("Longer text");
    QVERIFY(session.updateText(id, longer, QRectF(), false, false, false, false));
    const LayerTransform updated = session.activeLayer()->transform;
    QCOMPARE(updated.rotation, 30.0);
    // The top-left corner stays where it was on the canvas, and the width keeps its 2x stretch.
    QTransform map; map.translate(old.center().x(), old.center().y()); map.rotate(old.rotation); map.translate(-old.size.width() / 2, -old.size.height() / 2);
    QTransform map2; map2.translate(updated.center().x(), updated.center().y()); map2.rotate(updated.rotation); map2.translate(-updated.size.width() / 2, -updated.size.height() / 2);
    QVERIFY(QLineF(map.map(QPointF(0, 0)), map2.map(QPointF(0, 0))).length() < 0.001);
    QCOMPARE(updated.size.width() / session.activeLayer()->image.width(), 2.0);
    session.duplicateActiveLayer();
    QCOMPARE(session.activeLayer()->text->content, QStringLiteral("Longer text"));
    const QUuid target = session.activeLayer()->id;
    QVERIFY(session.linkMask(id, target));
    QCOMPARE(session.activeLayer()->maskSourceId, std::optional<QUuid>(id));
    for (const Layer &l : session.document()->layers) if (l.id == id) QVERIFY(l.text.has_value());
}

void TestTextLayers::saveReopenAndRasterize()
{
    EditorSession session;
    session.createDocument(800, 600);
    TextStyle s = style(QStringLiteral("Café 日本語\nSecond line"), QStringLiteral("DejaVu Sans"), 72);
    s.alignment = TextAlignment::Right;
    s.tracking = 3;
    QVERIFY(session.addText(s, QRectF(), false, false, false, false));
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("t.comp"));
    ProjectWriter::save(*session.document(), path);
    Document loaded = ProjectReader::load(path);
    QVERIFY(loaded.layers.last().text.has_value());
    QCOMPARE(*loaded.layers.last().text, *session.activeLayer()->text);
    QCOMPARE(loaded.layers.last().image, session.activeLayer()->image);
    // A pixel edit rasterizes the text: nothing editable is saved afterwards.
    EditorSession reopened;
    reopened.createDocument(100, 100);
    QVERIFY(reopened.addText(style(QStringLiteral("Paint me")), QRectF(), false, false, false, false));
    QVERIFY(reopened.invertActiveLayerPixels());
    QVERIFY(!reopened.activeLayer()->text.has_value());
    ProjectWriter::save(*reopened.document(), dir.filePath(QStringLiteral("r.comp")));
    QVERIFY(!ProjectReader::load(dir.filePath(QStringLiteral("r.comp"))).layers.last().text.has_value());
}

void TestTextLayers::rasterHasTransparentBackgroundAndColoredGlyphs()
{
    TextStyle s = style(QStringLiteral("TYPE"));
    s.red = 1;
    const QImage image = renderStyledText(s).convertToFormat(QImage::Format_RGBA8888);
    int ink = 0, clear = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.alpha() == 0) ++clear;
            else { ++ink; QVERIFY(c.red() > 0 && c.green() == 0 && c.blue() == 0); }
        }
    QVERIFY(ink > 100 && clear > 100);
}

void TestTextLayers::clippingToTextExportsColoredGlyphsOnTransparency()
{
    EditorSession session;
    session.createDocument(400, 300);
    QVERIFY(session.addText(style(QStringLiteral("Text")), QRectF(0, 0, 0, 0), false, false, false, false));
    const QUuid source = session.activeLayer()->id;
    const QSize size = session.activeLayer()->image.size();
    QImage fill(size, QImage::Format_RGBA8888_Premultiplied); fill.fill(QColor(255, 0, 0));
    QVERIFY(session.insertPixelLayer(fill, QPointF(0, 0), QStringLiteral("Clipped color"), QStringLiteral("Fill"), false));
    QVERIFY(session.linkMask(source, session.activeLayer()->id));
    const QImage flat = LayerRenderer::flattened(*session.document()).convertToFormat(QImage::Format_RGBA8888);
    int ink = 0, clear = 0;
    for (int y = 0; y < flat.height(); ++y)
        for (int x = 0; x < flat.width(); ++x) {
            const QColor c = flat.pixelColor(x, y);
            if (c.alpha() == 0) ++clear;
            else if (c.alpha() == 255) { ++ink; QVERIFY2(c.red() >= 240 && c.green() <= 5, qPrintable(QString::number(c.red()) + "," + QString::number(c.green()))); }
        }
    QVERIFY(ink > 100 && clear > 100);
}

void TestTextLayers::paragraphBoxKeepsEditableTextWithItsBox()
{
    EditorSession session;
    session.createDocument(800, 600);
    TextStyle s = style(QStringLiteral("Text that wraps inside its paragraph box"));
    QVERIFY(session.addText(s, QRectF(40, 60, 200, 120), false, false, false, true));
    QCOMPARE(session.activeLayer()->transform.size, QSizeF(200, 120));
    QCOMPARE(session.activeLayer()->image.size(), QSize(200, 120));
    QCOMPARE(*session.activeLayer()->text->boxSize, QSizeF(200, 120));
    // It wrapped: more than one line of ink rows separated by a gap, and nothing outside the padded area.
    const QRect ink = inkBounds(session.activeLayer()->image);
    QVERIFY(ink.left() >= int(kTextPadding) - 1 && ink.right() <= 200 - int(kTextPadding));
    QVERIFY(ink.height() > 60);
}

void TestTextLayers::emptyNewParagraphIsDiscarded()
{
    EditorSession session;
    session.createDocument(800, 600);
    const int count = session.document()->layers.size();
    QVERIFY(!session.addText(style(QStringLiteral("  \n ")), QRectF(0, 0, 100, 100), false, false, false, true));
    QCOMPARE(session.document()->layers.size(), count);
}

void TestTextLayers::invalidDraftsDoNotChangeDocument()
{
    EditorSession session;
    session.createDocument(800, 600);
    TextStyle s = style(QStringLiteral("Text"), QStringLiteral("DejaVu Sans"), 72);
    s.fontSize = std::nan("");
    QVERIFY(!session.addText(s, QRectF(), false, false, false, false));
    s.fontSize = 72;
    s.tracking = 5000;
    QVERIFY(!session.addText(s, QRectF(), false, false, false, false));
    s.tracking = 0;
    s.fontSize = 3000;
    QVERIFY(!session.addText(s, QRectF(), false, false, false, false));
    // The full macOS range is accepted: 1 to 2000 px.
    s.fontSize = 1;
    QVERIFY(session.addText(s, QRectF(), false, false, false, false));
    QCOMPARE(session.document()->layers.size(), 1);
}

void TestTextLayers::clickPlacesFirstBaselineAndMetricsMatchMac()
{
    // Point text is as big as it measures plus its 12 px padding on every side (macOS textBoxSize).
    TextStyle s = style(QStringLiteral("Hxg"), QStringLiteral("DejaVu Sans"), 40);
    QImage image = renderStyledText(s);
    QCOMPARE(kTextPadding, 12.0);
    QCOMPARE(textLineHeight(s), 48.0);   // Auto leading is 120% of the size
    QVERIFY(image.height() == int(std::ceil(48 + 24)));
    const QRect ink = inkBounds(image);
    QVERIFY(ink.left() >= 12 && ink.left() <= 20);
    // The first baseline sits the font's descent up from the bottom of the first line: 12 + 48 - descent.
    const double descent = QFontMetricsF(textStyleFont(s)).descent();
    QRect h; // the 'H' stem columns
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) for (int x = 12; x < 20; ++x) if (image.pixelColor(x, y).alpha() > 128) bottom = y;
    Q_UNUSED(h);
    QVERIFY2(std::abs((bottom + 1) - (12 + 48 - descent)) <= 1.5, qPrintable(QString::number(bottom + 1)));
    // A fixed box clips to what fits.
    s.boxSize = QSizeF(150, 70);
    s.content = QStringLiteral("one two three four five six seven eight nine ten");
    s.fontSize = 20;
    QCOMPARE(renderStyledText(s).size(), QSize(150, 70));
}

void TestTextLayers::leadingIsBaselineToBaseline()
{
    for (const double leading : {0.0, 30.0, 100.0}) {
        TextStyle s = style(QStringLiteral("H\nH"), QStringLiteral("DejaVu Sans"), 20);
        s.leading = leading;
        const QImage image = renderStyledText(s);
        QVector<int> bottoms;
        bool inRun = false; int last = -1;
        for (int y = 0; y < image.height(); ++y) {
            bool row = false;
            for (int x = 12; x < 20; ++x) if (image.pixelColor(x, y).alpha() > 128) row = true;
            if (row) { inRun = true; last = y; }
            else if (inRun) { bottoms.append(last); inRun = false; }
        }
        if (inRun) bottoms.append(last);
        QCOMPARE(bottoms.size(), 2);
        QVERIFY2(std::abs((bottoms[1] - bottoms[0]) - textLineHeight(s)) <= 1.5, qPrintable(QString::number(leading)));
    }
}

void TestTextLayers::boxResizeReflowsWithoutScalingFont()
{
    EditorSession session;
    session.createDocument(800, 600);
    QVERIFY(session.addText(style(QStringLiteral("Paragraph text reflow verification"), QStringLiteral("DejaVu Sans"), 20), QRectF(40, 40, 200, 100), false, false, false, true));
    const QUuid id = session.activeLayer()->id;
    session.selectLayer(id);
    session.activeLayer()->transform.size = QSizeF(360, 180);
    session.redrawSelectedShapes();
    QCOMPARE(session.activeLayer()->text->fontSize, 20.0);
    QCOMPARE(*session.activeLayer()->text->boxSize, QSizeF(360, 180));
    QCOMPARE(session.activeLayer()->image.size(), QSize(360, 180));
}

void TestTextLayers::textStyleRoundTripThroughEditSurvivesScale()
{
    // Point text scaled evenly by the Transform tool becomes bigger type, so editing it later does not undo the scale.
    EditorSession session;
    session.createDocument(800, 600);
    QVERIFY(session.addText(style(QStringLiteral("Scale"), QStringLiteral("DejaVu Sans"), 30), QRectF(100, 100, 0, 0), false, false, false, false));
    const QUuid id = session.activeLayer()->id;
    session.selectLayer(id);
    session.activeLayer()->transform.size = QSizeF(session.activeLayer()->transform.size.width() * 2, session.activeLayer()->transform.size.height() * 2);
    session.redrawSelectedShapes();
    QCOMPARE(session.activeLayer()->text->fontSize, 60.0);
    TextStyle edited = *session.activeLayer()->text;
    edited.content = QStringLiteral("Scaled");
    QVERIFY(session.updateText(id, edited, QRectF(), false, false, false, false));
    QCOMPARE(session.activeLayer()->text->fontSize, 60.0);
    QCOMPARE(session.activeLayer()->transform.size, QSizeF(session.activeLayer()->image.size()));
    // Uneven stretch stays a stretched bitmap that is still editable text (as on macOS).
    session.activeLayer()->transform.size = QSizeF(session.activeLayer()->image.width() * 3, session.activeLayer()->image.height());
    const QImage before = session.activeLayer()->image;
    session.redrawSelectedShapes();
    QCOMPARE(session.activeLayer()->image, before);
    QVERIFY(session.activeLayer()->text.has_value());
}

// ---- Faces -------------------------------------------------------------------------------------------------------

void TestTextLayers::postScriptNamesResolveToWeights()
{
    QVERIFY(!resolveTextFace(QStringLiteral("Arial-BoldMT")).italic);
    QVERIFY(resolveTextFace(QStringLiteral("Arial-BoldMT")).bold());
    QCOMPARE(resolveTextFace(QStringLiteral("Arial-BoldMT")).family, QStringLiteral("Arial"));
    QVERIFY(resolveTextFace(QStringLiteral("Arial-BoldItalicMT")).italic);
    QVERIFY(resolveTextFace(QStringLiteral("ArialMT")).installed);
    QVERIFY(!resolveTextFace(QStringLiteral("ArialMT")).bold());
    QVERIFY(resolveTextFace(QStringLiteral("Helvetica-Bold")).bold());
    QVERIFY(resolveTextFace(QStringLiteral("Helvetica-Oblique")).italic);
    QVERIFY(resolveTextFace(QStringLiteral("DejaVuSans-BoldOblique")).bold());
    QVERIFY(!resolveTextFace(QStringLiteral("DefinitelyMissingFontXYZ")).installed);
    QCOMPARE(resolveTextFace(QStringLiteral("DejaVu Sans")).family, QStringLiteral("DejaVu Sans"));
}

void TestTextLayers::boldFaceDrawsHeavierAndKeepsItsNameThroughSave()
{
    TextStyle plain = style(QStringLiteral("Weight"), QStringLiteral("Arial"), 60);
    TextStyle bold = plain; bold.fontName = QStringLiteral("Arial-BoldMT");
    QVERIFY(inkPixels(renderStyledText(bold)) > inkPixels(renderStyledText(plain)) * 1.08);

    EditorSession session;
    session.createDocument(500, 300);
    QVERIFY(session.addText(bold, QRectF(), false, false, false, false));
    QCOMPARE(session.activeLayer()->text->fontName, QStringLiteral("Arial-BoldMT"));
    QTemporaryDir dir; QVERIFY(dir.isValid());
    ProjectWriter::save(*session.document(), dir.filePath(QStringLiteral("b.comp")));
    Document loaded = ProjectReader::load(dir.filePath(QStringLiteral("b.comp")));
    QCOMPARE(loaded.layers.last().text->fontName, QStringLiteral("Arial-BoldMT"));
}

void TestTextLayers::boldItalicToggleComposesFaceNames()
{
    QCOMPARE(composeTextFace(QStringLiteral("Helvetica"), true, false), QStringLiteral("Helvetica-Bold"));
    QCOMPARE(composeTextFace(QStringLiteral("Helvetica"), true, true), QStringLiteral("Helvetica-BoldOblique"));
    QCOMPARE(composeTextFace(QStringLiteral("Helvetica-Bold"), false, false), QStringLiteral("Helvetica"));
    QCOMPARE(composeTextFace(QStringLiteral("DejaVu Sans"), true, false), QStringLiteral("DejaVuSans-Bold"));
    QVERIFY(resolveTextFace(composeTextFace(QStringLiteral("DejaVu Sans"), true, true)).bold());
    QVERIFY(resolveTextFace(composeTextFace(QStringLiteral("DejaVu Sans"), true, true)).italic);
    // Bold survives a save: it is in fontName, not in metadata that is dropped.
    EditorSession session;
    session.createDocument(500, 300);
    QVERIFY(session.addText(style(QStringLiteral("Bold")), QRectF(), true, false, false, false));
    QTemporaryDir dir; QVERIFY(dir.isValid());
    ProjectWriter::save(*session.document(), dir.filePath(QStringLiteral("x.comp")));
    const Document loaded = ProjectReader::load(dir.filePath(QStringLiteral("x.comp")));
    QVERIFY(resolveTextFace(loaded.layers.last().text->fontName).bold());
}

void TestTextLayers::editingAMacFaceDoesNotRenameIt()
{
    EditorSession session;
    session.createDocument(500, 300);
    TextStyle s = style(QStringLiteral("Mac"), QStringLiteral("Helvetica-Bold"), 40);
    QVERIFY(session.addText(s, QRectF(), false, false, false, false));
    const QUuid id = session.activeLayer()->id;
    TextStyle edited = *session.activeLayer()->text;
    edited.content = QStringLiteral("Mac edited");
    QVERIFY(session.updateText(id, edited, QRectF(), false, false, false, false));
    QCOMPARE(session.activeLayer()->text->fontName, QStringLiteral("Helvetica-Bold"));
}

// ---- The rest of the app -----------------------------------------------------------------------------------------

void TestTextLayers::mergeAndFlattenRasterizeText()
{
    EditorSession session;
    session.createDocument(400, 300);
    QImage base(400, 300, QImage::Format_RGBA8888_Premultiplied); base.fill(Qt::white);
    QVERIFY(session.insertPixelLayer(base, QPointF(0, 0), QStringLiteral("Base"), QStringLiteral("Fill"), false));
    QVERIFY(session.addText(style(QStringLiteral("Merge me")), QRectF(10, 10, 0, 0), false, false, false, false));
    QVERIFY(session.activeLayer()->text.has_value());
    QVERIFY(session.canMergeLayers());
    QVERIFY(session.mergeLayers());
    QVERIFY(!session.activeLayer()->text.has_value());
    QVERIFY(session.activeLayer()->shape.isEmpty());
    QVERIFY(session.addText(style(QStringLiteral("Flatten me")), QRectF(10, 100, 0, 0), false, false, false, false));
    QVERIFY(session.flattenImage());
    for (const Layer &l : session.document()->layers) QVERIFY(!l.text.has_value());
}

void TestTextLayers::canvasSizeAndFlipKeepTextMetadata()
{
    EditorSession session;
    session.createDocument(400, 300);
    QVERIFY(session.addText(style(QStringLiteral("Keep")), QRectF(10, 10, 0, 0), false, false, false, false));
    const TextStyle before = *session.activeLayer()->text;
    QVERIFY(session.resizeCanvas(QSize(600, 400), 4));
    QVERIFY(session.activeLayer()->text.has_value());
    QCOMPARE(*session.activeLayer()->text, before);
    QVERIFY(session.flipLayers(true));
    QVERIFY(session.activeLayer()->text.has_value());
    QVERIFY(session.activeLayer()->transform.flipX);
    // Editing a flipped layer keeps the flip.
    TextStyle edited = before; edited.content = QStringLiteral("Keep!");
    QVERIFY(session.updateText(session.activeLayer()->id, edited, QRectF(), false, false, false, false));
    QVERIFY(session.activeLayer()->transform.flipX);
}

void TestTextLayers::textWithEffectsStaysEditable()
{
    EditorSession session;
    session.createDocument(400, 300);
    QVERIFY(session.addText(style(QStringLiteral("Glow")), QRectF(10, 10, 0, 0), false, false, false, false));
    const QUuid id = session.activeLayer()->id;
    QVERIFY(session.addLayerEffect(id, LayerEffectKind::OuterGlow));
    QVERIFY(session.activeLayer()->text.has_value());
    TextStyle edited = *session.activeLayer()->text; edited.content = QStringLiteral("Glow!");
    QVERIFY(session.updateText(id, edited, QRectF(), false, false, false, false));
    QVERIFY(session.activeLayer()->effects.has_value());
    QVERIFY(session.activeLayer()->text.has_value());
    QImage flat = LayerRenderer::flattened(*session.document());
    QVERIFY(inkPixels(flat, 1) > inkPixels(session.activeLayer()->image, 128));   // the glow adds pixels around the letters
}

void TestTextLayers::fillWithForegroundRecolorsLiveText()
{
    EditorSession session;
    session.createDocument(400, 300);
    QVERIFY(session.addText(style(QStringLiteral("Fill")), QRectF(10, 10, 0, 0), false, false, false, false));
    QVERIFY(session.fillSelection(QColor(255, 0, 0)));
    QVERIFY(session.activeLayer()->text.has_value());
    QCOMPARE(session.activeLayer()->text->red, 1.0);
    const QImage image = session.activeLayer()->image.convertToFormat(QImage::Format_RGBA8888);
    int red = 0;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) { const QColor c = image.pixelColor(x, y); if (c.alpha() > 200 && c.red() > 200 && c.green() < 50) ++red; }
    QVERIFY(red > 50);
    session.undo();
    QCOMPARE(session.activeLayer()->text->red, 0.0);
}

// ---- PSD text ----------------------------------------------------------------------------------------------------

void TestTextLayers::photoshopPointTextImportsAsEditableText()
{
    const auto parsed = PSDText::parse(extraTySh());
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->style.content, QStringLiteral("Hello"));
    QCOMPARE(parsed->style.fontName, QStringLiteral("Helvetica"));
    QCOMPARE(parsed->style.fontSize, 24.0);
    QVERIFY(parsed->style.red == 0 && parsed->style.green == 0 && parsed->style.blue == 0);
    QCOMPARE(parsed->style.alignment, TextAlignment::Left);
    QVERIFY(parsed->notes.isEmpty());

    PSDImportResult result; QString error;
    const QByteArray file = layerFile(32, 32, QRect(1, 2, 8, 8), Qt::black, extraTySh(), QStringLiteral("Greeting"));
    QVERIFY2(PSDReader::read(file, result, &error), qPrintable(error));
    const Layer &layer = result.document.layers.first();
    QVERIFY(layer.text.has_value());
    QCOMPARE(layer.text->content, QStringLiteral("Hello"));
    QCOMPARE(layer.text->fontName, QStringLiteral("Helvetica"));
    QCOMPARE(layer.text->fontSize, 24.0);
    QCOMPARE(layer.image, renderStyledText(*layer.text));
    for (const PSDConversion &c : result.conversions) QVERIFY(c.message != PSDText::rasterizedNote());
    QVERIFY(std::abs(layer.transform.origin.x() - 40) < 80);
    QVERIFY(std::abs(layer.transform.origin.y() - 50) < 80);
    // It saves and reloads as the same editable text.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    ProjectWriter::save(result.document, dir.filePath(QStringLiteral("p.comp")));
    const Document loaded = ProjectReader::load(dir.filePath(QStringLiteral("p.comp")));
    QCOMPARE(*loaded.layers.first().text, *layer.text);
}

void TestTextLayers::photoshopTextSizeUsesMatrixScaleNotDocumentResolution()
{
    QCOMPARE(PSDText::parse(extraTySh())->style.fontSize, 24.0);
    TyShOptions two; two.xx = 2; two.yy = 2;
    QCOMPARE(PSDText::parse(extraTySh(two))->style.fontSize, 48.0);
    TyShOptions big; big.fontSize = 25; big.xx = 2; big.yy = 2;
    QCOMPARE(PSDText::parse(extraTySh(big))->style.fontSize, 50.0);
}

void TestTextLayers::photoshopTextKeepsTheFirstStyleAndReportsTheRest()
{
    TyShOptions o; o.red = 1; o.justification = 2; o.tracking = 1000; o.leading = 30; o.secondSize = 48;
    const auto parsed = PSDText::parse(extraTySh(o));
    QVERIFY(parsed);
    QCOMPARE(parsed->style.content, QStringLiteral("Hello"));
    QCOMPARE(parsed->style.alignment, TextAlignment::Center);
    QCOMPARE(parsed->style.tracking, 24.0);
    QCOMPARE(parsed->style.leading, 30.0);
    QCOMPARE(parsed->style.red, 1.0);
    QCOMPARE(parsed->style.fontSize, 24.0);
    QVERIFY(parsed->notes.contains(PSDText::firstStyleNote()));
}

void TestTextLayers::photoshopTextReportsLeadingOnlyStyleDifferences()
{
    TyShOptions a; a.leading = 30; a.secondLeading = 48;
    const auto byLeading = PSDText::parse(extraTySh(a));
    QCOMPARE(byLeading->style.leading, 30.0);
    QVERIFY(byLeading->notes.contains(PSDText::firstStyleNote()));
    TyShOptions b; b.secondHorizontalScale = 1.2;
    QVERIFY(PSDText::parse(extraTySh(b))->notes.contains(PSDText::firstStyleNote()));
}

void TestTextLayers::photoshopParagraphTextKeepsItsBox()
{
    TyShOptions o; o.tx = 10; o.ty = 30; o.bounds = Box{0, 0, 200, 80}; o.glyphBounds = Box{0, -10, 40, 10};
    const auto parsed = PSDText::parse(extraTySh(o));
    QVERIFY(parsed);
    QVERIFY(parsed->anchorIsFrame);
    QCOMPARE(parsed->style.boxSize->width(), 224.0);
    QCOMPARE(parsed->style.boxSize->height(), 104.0);
    QCOMPARE(parsed->documentAnchor, QPointF(10, 30));
    // The imported layer's frame lands at the anchor (its padding is outside the text frame).
    const auto rendered = PSDText::render(*parsed);
    QVERIFY(rendered);
    QCOMPARE(rendered->image.size(), QSize(224, 104));
    QVERIFY(std::abs(rendered->transform.origin.x() - (10 - kTextPadding)) < 0.01);
    QVERIFY(std::abs(rendered->transform.origin.y() - (30 - kTextPadding)) < 0.01);
}

void TestTextLayers::oversizedPhotoshopParagraphFrameStaysPixels()
{
    TyShOptions o; o.bounds = Box{0, 0, 40000, 100}; o.glyphBounds = Box{0, 0, 40, 10};
    QVERIFY(!PSDText::parse(extraTySh(o)).has_value());
    PSDImportResult result; QString error;
    QVERIFY(PSDReader::read(layerFile(16, 16, QRect(0, 0, 4, 4), QColor(0, 255, 0), extraTySh(o), QStringLiteral("Billboard")), result, &error));
    const Layer &layer = result.document.layers.first();
    QVERIFY(!layer.text.has_value());
    QCOMPARE(layer.image.width(), 4);
    bool noted = false;
    for (const PSDConversion &c : result.conversions) noted = noted || c.message == PSDText::rasterizedNote();
    QVERIFY(noted);
}

void TestTextLayers::warpedPhotoshopTextStaysEditableAndSaysSo()
{
    TyShOptions o; o.fauxBold = true; o.warp = true;
    const auto parsed = PSDText::parse(extraTySh(o));
    QVERIFY(parsed);
    QCOMPARE(parsed->style.content, QStringLiteral("Hello"));
    QVERIFY(parsed->notes.contains(PSDText::warpNote()));
    QVERIFY(parsed->notes.contains(PSDText::fauxNote()));
}

void TestTextLayers::verticalOrBrokenPhotoshopTextStaysPixels()
{
    TyShOptions v; v.vertical = true;
    QVERIFY(!PSDText::parse(extraTySh(v)).has_value());
    QVERIFY(!PSDText::parse({{QStringLiteral("TySh"), QByteArray("\x00\x01", 2)}}).has_value());
    TyShOptions uneven; uneven.xx = 2; uneven.yy = 1;
    QVERIFY(!PSDText::parse(extraTySh(uneven)).has_value());
    PSDImportResult result; QString error;
    QVERIFY(PSDReader::read(layerFile(16, 16, QRect(0, 0, 4, 4), QColor(0, 0, 255), extraTySh(v), QStringLiteral("Sideways")), result, &error));
    QVERIFY(!result.document.layers.first().text.has_value());
    QCOMPARE(result.document.layers.first().image.width(), 4);
    bool noted = false;
    for (const PSDConversion &c : result.conversions) noted = noted || c.message == PSDText::rasterizedNote();
    QVERIFY(noted);
}

void TestTextLayers::missingPhotoshopFontIsReportedButStaysEditable()
{
    TyShOptions o; o.font = QStringLiteral("DefinitelyMissingFontXYZ");
    PSDImportResult result; QString error;
    QVERIFY(PSDReader::read(layerFile(64, 64, QRect(0, 0, 2, 2), Qt::black, extraTySh(o), QStringLiteral("Missing")), result, &error));
    QCOMPARE(result.document.layers.first().text->fontName, QStringLiteral("DefinitelyMissingFontXYZ"));
    bool noted = false;
    for (const PSDConversion &c : result.conversions) noted = noted || c.message == PSDText::missingFontNote(QStringLiteral("DefinitelyMissingFontXYZ"));
    QVERIFY(noted);
    // Saved and reloaded, the name is still the original and the PNG is byte-for-byte what was drawn.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    ProjectWriter::save(result.document, dir.filePath(QStringLiteral("m.comp")));
    const Document loaded = ProjectReader::load(dir.filePath(QStringLiteral("m.comp")));
    QCOMPARE(loaded.layers.first().text->fontName, QStringLiteral("DefinitelyMissingFontXYZ"));
    QCOMPARE(loaded.layers.first().image, result.document.layers.first().image);
}

void TestTextLayers::rotatedPhotoshopTextKeepsItsAngle()
{
    TyShOptions o; o.xx = 0; o.xy = -1; o.yx = 1; o.yy = 0;
    const auto parsed = PSDText::parse(extraTySh(o));
    QVERIFY(parsed);
    QVERIFY(std::abs(parsed->rotation - 90) < 0.01);
    QVERIFY(!parsed->flipY);
    const auto rendered = PSDText::render(*parsed);
    QVERIFY(rendered);
    QVERIFY(std::abs(rendered->transform.rotation - 90) < 0.01);
}

void TestTextLayers::importedPhotoshopTextRendersAtItsPlace()
{
    // The baseline's start lands on the layer's (tx, ty): the ink sits just above and right of it.
    TyShOptions o; o.text = QStringLiteral("Hxg"); o.font = QStringLiteral("DejaVu Sans"); o.fontSize = 40; o.tx = 100; o.ty = 120;
    PSDImportResult result; QString error;
    QVERIFY(PSDReader::read(layerFile(300, 300, QRect(0, 0, 4, 4), Qt::black, extraTySh(o), QStringLiteral("Placed")), result, &error));
    const Document &doc = result.document;
    const QImage flat = LayerRenderer::flattened(doc).convertToFormat(QImage::Format_RGBA8888);
    const QRect ink = inkBounds(flat);
    QVERIFY2(ink.left() >= 98 && ink.left() <= 106, qPrintable(QString::number(ink.left())));
    // 'H' bottom (baseline) is at ty; the 'g' descends below.
    int hBottom = -1;
    for (int y = 0; y < flat.height(); ++y) for (int x = 100; x < 112; ++x) if (flat.pixelColor(x, y).alpha() > 128) hBottom = std::max(hBottom, y);
    QVERIFY2(std::abs((hBottom + 1) - 120) <= 2, qPrintable(QString::number(hBottom + 1)));
}

void TestTextLayers::importedPostScriptBoldRendersBold()
{
    TyShOptions plain; plain.font = QStringLiteral("ArialMT"); plain.fontSize = 60; plain.text = QStringLiteral("Weight");
    TyShOptions bold = plain; bold.font = QStringLiteral("Arial-BoldMT");
    PSDImportResult a, b; QString error;
    QVERIFY(PSDReader::read(layerFile(400, 200, QRect(0, 0, 4, 4), Qt::black, extraTySh(plain)), a, &error));
    QVERIFY(PSDReader::read(layerFile(400, 200, QRect(0, 0, 4, 4), Qt::black, extraTySh(bold)), b, &error));
    QVERIFY(inkPixels(b.document.layers.first().image) > inkPixels(a.document.layers.first().image) * 1.08);
    QCOMPARE(b.document.layers.first().text->fontName, QStringLiteral("Arial-BoldMT"));
}

// ---- PSD vector --------------------------------------------------------------------------------------------------

void TestTextLayers::vectorMaskIsRasterizedWithFillAndStroke()
{
    const QSizeF canvas(200, 200);
    QMap<QString, QByteArray> extra = squareVector(canvas);
    bool tooLarge = false;
    const auto raster = PSDVector::raster(extra, canvas, 100000000, &tooLarge);
    QVERIFY(raster);
    QVERIFY(raster->bounds.width() >= 99 && raster->bounds.height() >= 49);
    QVERIFY(raster->image.width() >= 99 && raster->image.height() >= 49);
    QCOMPARE(raster->image.pixelColor(50, 20).blue(), 255);   // filled with the solid color
    extra["vstk"] = strokeStyle(true, true, 10, 255, 255, 0);
    extra["SoCo"] = colorDescriptor(0, 0, 0);
    const auto stroked = PSDVector::raster(extra, canvas, 100000000, &tooLarge);
    QVERIFY(stroked);
    QVERIFY(stroked->bounds.width() > raster->bounds.width());
    QVERIFY(stroked->bounds.height() > raster->bounds.height());
    // The stroke is drawn: some yellow, and the black fill inside it.
    int yellow = 0;
    for (int y = 0; y < stroked->image.height(); ++y) for (int x = 0; x < stroked->image.width(); ++x) { const QColor c = stroked->image.pixelColor(x, y); if (c.alpha() > 250 && c.red() > 240 && c.green() > 240 && c.blue() < 20) ++yellow; }
    QVERIFY(yellow > 100);
}

void TestTextLayers::fillEllipseImportsAsALiveShape()
{
    const QSizeF canvas(1920, 1080);
    QMap<QString, QByteArray> extra = squareVector(canvas);
    extra["vogk"] = originationData(5, Box{454, 513, 782, 841});
    bool tooLarge = false;
    const auto live = PSDVector::live(extra, canvas, 100000000, &tooLarge);
    QVERIFY(live);
    QCOMPARE(live->style.kind, ShapeKind::Ellipse);
    QVERIFY(std::abs(live->style.green - 110.0 / 255) < 0.01);
    QVERIFY(std::abs(live->style.blue - 1) < 0.01);
    QVERIFY(live->notes.isEmpty());
    QVERIFY(std::abs(live->bounds.left() - 454) < 1 && std::abs(live->bounds.width() - 328) < 1);
    // The corners of an ellipse's box are transparent, its center is the fill.
    QCOMPARE(live->image.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(live->image.pixelColor(live->image.width() / 2, live->image.height() / 2).blue(), 255);
    PSDImportResult result; QString error;
    QVERIFY(PSDReader::read(layerFile(1920, 1080, QRect(0, 0, 4, 4), Qt::black, extra, QStringLiteral("cercle-bleu")), result, &error));
    const Layer &layer = result.document.layers.first();
    QVERIFY(layer.shapeStyle.has_value());
    QCOMPARE(layer.shapeStyle->kind, ShapeKind::Ellipse);
    QVERIFY(result.conversions.isEmpty());
    QCOMPARE(layer.image, live->image);
}

void TestTextLayers::strokedRectangleImportsAsALiveShapeAndReportsTheStroke()
{
    const QSizeF canvas(1920, 1080);
    QMap<QString, QByteArray> extra = squareVector(canvas);
    extra["vstk"] = strokeStyle(true, true, 10, 255, 255, 0);
    extra["vogk"] = originationData(2, Box{945, 153, 1591, 335}, {0, 0, 0, 0});
    bool tooLarge = false;
    const auto live = PSDVector::live(extra, canvas, 100000000, &tooLarge);
    QVERIFY(live);
    QCOMPARE(live->style.kind, ShapeKind::Rectangle);
    QCOMPARE(live->style.cornerRadius, 0.0);
    bool strokeNote = false;
    for (const QString &n : live->notes) strokeNote = strokeNote || n.contains(QStringLiteral("stroke"));
    QVERIFY(strokeNote);
    PSDImportResult result; QString error;
    QVERIFY(PSDReader::read(layerFile(1920, 1080, QRect(0, 0, 4, 4), Qt::black, extra, QStringLiteral("rectangle-contour-jaune")), result, &error));
    QVERIFY(result.document.layers.first().shapeStyle.has_value());
    bool reported = false, rasterized = false;
    for (const PSDConversion &c : result.conversions) { reported = reported || (c.layerName == QStringLiteral("rectangle-contour-jaune") && c.message.contains(QStringLiteral("stroke"))); rasterized = rasterized || c.message.contains(QStringLiteral("rasterized")); }
    QVERIFY(reported);
    QVERIFY(!rasterized);
    // A rounded origination keeps its radius.
    extra["vogk"] = originationData(2, Box{945, 153, 1591, 335}, {12, 12, 12, 12});
    QCOMPARE(PSDVector::live(extra, canvas, 100000000, &tooLarge)->style.cornerRadius, 12.0);
}

void TestTextLayers::fourSharpCornersInferARectangleWithoutOrigination()
{
    const QSizeF canvas(200, 200);
    bool tooLarge = false;
    const auto live = PSDVector::live(squareVector(canvas), canvas, 100000000, &tooLarge);
    QVERIFY(live);
    QCOMPARE(live->style.kind, ShapeKind::Rectangle);
    QVERIFY(live->notes.isEmpty());
    QVERIFY(live->bounds.width() >= 99 && live->bounds.height() >= 49);
}

void TestTextLayers::hugeOriginationSizeIsRejectedWithoutTrapping()
{
    QMap<QString, QByteArray> extra{{"vogk", originationData(5, Box{0, 0, 1e20, 1e20})}, {"SoCo", colorDescriptor(0, 110, 255)}, {"vstk", strokeStyle(true, false, 1, 255, 255, 0)}};
    bool tooLarge = false;
    QVERIFY(!PSDVector::live(extra, QSizeF(1920, 1080), 100000000, &tooLarge).has_value());
    QVERIFY(tooLarge);
    PSDImportResult result; QString error;
    QVERIFY(!PSDReader::read(layerFile(1920, 1080, QRect(0, 0, 4, 4), Qt::black, extra), result, &error));
}

void TestTextLayers::nonFiniteOriginationSizeIsIgnored()
{
    QMap<QString, QByteArray> extra{{"vogk", originationData(5, Box{10, 10, std::numeric_limits<double>::infinity(), 110})}, {"SoCo", colorDescriptor(0, 110, 255)}, {"vstk", strokeStyle(true, false, 1, 255, 255, 0)}};
    bool tooLarge = false;
    QVERIFY(!PSDVector::live(extra, QSizeF(1920, 1080), 100000000, &tooLarge).has_value());
    QVERIFY(!tooLarge);
}

void TestTextLayers::hugeStrokeWidthIsRejectedWithoutTrapping()
{
    QMap<QString, QByteArray> extra = squareVector();
    extra["SoCo"] = colorDescriptor(0, 0, 0);
    extra["vstk"] = strokeStyle(true, true, 1e20, 255, 255, 0);
    bool tooLarge = false;
    QVERIFY(!PSDVector::raster(extra, QSizeF(200, 200), 100000000, &tooLarge).has_value());
    QVERIFY(tooLarge);
}

void TestTextLayers::liveShapeImportSavesAndReloads()
{
    QMap<QString, QByteArray> extra = squareVector(QSizeF(300, 300));
    extra["vogk"] = originationData(1, Box{20, 30, 120, 80});
    PSDImportResult result; QString error;
    QVERIFY(PSDReader::read(layerFile(300, 300, QRect(0, 0, 4, 4), Qt::black, extra, QStringLiteral("Box")), result, &error));
    QTemporaryDir dir; QVERIFY(dir.isValid());
    ProjectWriter::save(result.document, dir.filePath(QStringLiteral("s.comp")));
    const Document loaded = ProjectReader::load(dir.filePath(QStringLiteral("s.comp")));
    QVERIFY(loaded.layers.first().shapeStyle.has_value());
    QCOMPARE(loaded.layers.first().transform.origin, QPointF(20, 30));
    QCOMPARE(loaded.layers.first().image.size(), QSize(100, 50));
}

void TestTextLayers::textHitTestFollowsRotationVisibilityAndFolders()
{
    EditorSession session;
    session.createDocument(800, 600);
    TextStyle big = style(QStringLiteral("WWWWWWWWWW"), QStringLiteral("DejaVu Sans"), 60);
    QVERIFY(session.addText(big, QRectF(100, 250, 0, 0), false, false, false, false));
    auto document = std::make_shared<Document>(*session.document());
    Layer &text = document->layers.last();
    const QUuid id = text.id;
    text.transform.rotation = 90;   // a wide strip turned upright about its center
    const QPointF center = text.transform.center();
    CanvasWidget canvas;
    canvas.resize(900, 700);
    canvas.setDocument(document);
    canvas.setZoom(1.0);
    canvas.setTool(CanvasWidget::Tool::Text);
    canvas.show();
    QSignalSpy spy(&canvas, &CanvasWidget::textLayerEditRequested);
    const auto doubleClickAt = [&](const QPointF &documentPoint) {
        const QPoint at = canvas.widgetRectForDocumentRect(QRectF(documentPoint, QSizeF(0, 0))).topLeft().toPoint();
        QTest::mouseDClick(&canvas, Qt::LeftButton, Qt::NoModifier, at);
    };
    // Inside the turned rectangle but outside the unturned one: hit. Inside the unturned one only: miss.
    doubleClickAt(center + QPointF(0, text.transform.size.width() / 2 - 5));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.last().first().toUuid(), id);
    doubleClickAt(center + QPointF(text.transform.size.width() / 2 - 5, 0));
    QCOMPARE(spy.count(), 1);
    // A hidden layer, and one inside a hidden folder, are not picked.
    Layer folder; folder.id = QUuid::createUuid(); folder.name = QStringLiteral("F"); folder.group = true; folder.visible = false;
    document->layers.insert(document->layers.size() - 1, folder);
    document->layers.last().parentId = folder.id;
    doubleClickAt(center);
    QCOMPARE(spy.count(), 1);
    document->layers[document->layers.size() - 2].visible = true;
    document->layers.last().parentId.reset();
    document->layers.last().visible = false;
    doubleClickAt(center);
    QCOMPARE(spy.count(), 1);
}

void TestTextLayers::eightHandlesResizeTheBoxAndReflow()
{
    InlineTextEditor editor;
    editor.canvasBox = QRectF(50, 50, 200, 100);
    editor.areaText = true;
    editor.syncCanvasGeometry();
    editor.show();
    QCOMPARE(editor.handleAt(QPoint(0, 0)), 0);
    QCOMPARE(editor.handleAt(QPoint(100, 0)), 1);
    QCOMPARE(editor.handleAt(QPoint(199, 0)), 2);
    QCOMPARE(editor.handleAt(QPoint(199, 50)), 3);
    QCOMPARE(editor.handleAt(QPoint(199, 99)), 4);
    QCOMPARE(editor.handleAt(QPoint(100, 99)), 5);
    QCOMPARE(editor.handleAt(QPoint(0, 99)), 6);
    QCOMPARE(editor.handleAt(QPoint(0, 50)), 7);
    QCOMPARE(editor.handleAt(QPoint(100, 50)), -1);
    // Dragging the top-left corner moves the origin and grows the box; the bottom-right changes only the size.
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
    QMouseEvent move(QEvent::MouseMove, QPointF(-29, -19), editor.mapToGlobal(QPointF(-29, -19)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&editor, &move);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier, QPoint(-29, -19));
    QCOMPARE(editor.canvasBox.left(), 20.0);
    QCOMPARE(editor.canvasBox.top(), 30.0);
    QCOMPARE(editor.canvasBox.right(), 250.0);
    QCOMPARE(editor.canvasBox.bottom(), 150.0);
    QVERIFY(editor.areaText);
    QVERIFY(editor.wasResized());
}

void TestTextLayers::overflowMarkerShowsWhenTextDoesNotFit()
{
    const auto markerPixel = [](InlineTextEditor &editor) {
        const QImage image = editor.grab().toImage();
        return image.pixelColor(image.width() - 1, image.height() - 1 - 2);
    };
    InlineTextEditor fits;
    fits.canvasBox = QRectF(0, 0, 220, 120);
    fits.areaText = true;
    TextStyle s = style(QStringLiteral("short"), QStringLiteral("DejaVu Sans"), 20);
    s.boxSize = QSizeF(220, 120);
    fits.fontSize = 20;
    fits.loadStyle(s);
    fits.setLineWrapMode(QTextEdit::FixedPixelWidth);
    fits.syncCanvasGeometry();
    fits.applySpacing();
    fits.show();
    QVERIFY(markerPixel(fits).lightness() > 200);
    InlineTextEditor over;
    over.canvasBox = QRectF(0, 0, 220, 60);
    over.areaText = true;
    s.content = QStringLiteral("one two three four five six seven eight nine ten eleven twelve thirteen");
    s.boxSize = QSizeF(220, 60);
    over.fontSize = 20;
    over.loadStyle(s);
    over.setLineWrapMode(QTextEdit::FixedPixelWidth);
    over.syncCanvasGeometry();
    over.applySpacing();
    over.show();
    QVERIFY(markerPixel(over).lightness() < 80);
}

void TestTextLayers::mixedFacesShareOneBaseline()
{
    // A larger-descent face on the same line must not move the baseline: the line is a fixed height whatever it holds.
    TextStyle plain = style(QStringLiteral("HHHH"), QStringLiteral("DejaVu Sans"), 40);
    TextStyle mixed = plain;
    mixed.setFont(QStringLiteral("Courier New"), 2, 2);
    const auto baseline = [](const QImage &image) {
        int bottom = -1;
        for (int y = 0; y < image.height(); ++y) for (int x = 12; x < 20; ++x) if (image.pixelColor(x, y).alpha() > 128) bottom = y;
        return bottom + 1;
    };
    QCOMPARE(baseline(renderStyledText(mixed)), baseline(renderStyledText(plain)));
    // The baseline of the later letters is the same as the first's.
    const QImage image = renderStyledText(mixed);
    int firstBottom = -1, lastBottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 12; x < 40; ++x) if (image.pixelColor(x, y).alpha() > 128) firstBottom = y;
        for (int x = image.width() - 40; x < image.width() - 12; ++x) if (image.pixelColor(x, y).alpha() > 128) lastBottom = y;
    }
    QVERIFY2(std::abs(firstBottom - lastBottom) <= 1, qPrintable(QString("%1 %2").arg(firstBottom).arg(lastBottom)));
}

void TestTextLayers::copyAndCopyMergedIncludeTextPixels()
{
    EditorSession session;
    session.createDocument(400, 300);
    QImage base(400, 300, QImage::Format_RGBA8888_Premultiplied); base.fill(Qt::white);
    QVERIFY(session.insertPixelLayer(base, QPointF(0, 0), QStringLiteral("Base"), QStringLiteral("Fill"), false));
    TextStyle s = style(QStringLiteral("Copy"), QStringLiteral("DejaVu Sans"), 60);
    QVERIFY(session.addText(s, QRectF(20, 20, 0, 0), false, false, false, false));
    const auto layerCopy = session.copiedPixels(false);
    QVERIFY(layerCopy.has_value());
    QVERIFY(inkPixels(layerCopy->first, 128) > 100);          // the letters, on transparency
    QVERIFY(layerCopy->first.pixelColor(0, 0).alpha() == 0);
    const auto merged = session.copiedPixels(true);
    QVERIFY(merged.has_value());
    // Merged: the same canvas pixels the exporter flattens, with the letters in it.
    const QImage flat = LayerRenderer::flattened(*session.document());
    const QImage mergedImage = merged->first.convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(mergedImage.size(), flat.size());
    QCOMPARE(mergedImage.convertToFormat(QImage::Format_RGBA8888), flat.convertToFormat(QImage::Format_RGBA8888));
    int dark = 0;
    for (int y = 0; y < mergedImage.height(); ++y) for (int x = 0; x < mergedImage.width(); ++x) if (mergedImage.pixelColor(x, y).lightness() < 60) ++dark;
    QVERIFY(dark > 100);
}

void TestTextLayers::copiedTextLayersPromoteTheFormat()
{
    EditorSession session;
    session.createDocument(400, 300);
    TextStyle s = style(QStringLiteral("Runs"), QStringLiteral("DejaVu Sans"), 40);
    s.setColor({1, 0, 0}, 0, 2);
    s.setFont(QStringLiteral("Courier New"), 2, 2);
    QVERIFY(session.addText(s, QRectF(), false, false, false, false));
    Document target;
    target.formatVersion = 9;
    target.layers.append(session.document()->layers.last());
    QVERIFY(ProjectWriter::minimumRequiredVersion(target) >= 11);
}

void TestTextLayers::boldAndItalicButtonsSetTheFaceOfNewAndExistingText()
{
    MainWindow window;
    window.show();
    EditorSession &session = window.session();
    session.createDocument(400, 300);
    window.syncDocumentViews();
    auto *canvas = window.findChild<CanvasWidget *>();
    auto *bold = window.findChild<QToolButton *>(QStringLiteral("textBold"));
    auto *italic = window.findChild<QToolButton *>(QStringLiteral("textItalic"));
    QVERIFY(canvas && bold && italic);
    QVERIFY(!window.findChild<QToolButton *>(QStringLiteral("textUnderline")));   // not representable, so not offered
    canvas->setTool(CanvasWidget::Tool::Text);
    QCoreApplication::processEvents();
    QVERIFY(bold->isVisible() && italic->isVisible());
    // Before any text is open, the buttons set what new text starts as.
    bold->setChecked(true);
    emit canvas->textBoxRequested(QRectF(40, 60, 0, 0), false);
    InlineTextEditor *editor = window.inlineTextEditor();
    QVERIFY(editor);
    QTest::keyClicks(editor, "Bold start");
    QVERIFY(resolveTextFace(editor->faceAtSelection().isEmpty() ? editor->fallbackFace : editor->faceAtSelection()).bold());
    // Toggling Italic on with the text open changes its letters' face; the saved name carries both.
    italic->setChecked(true);
    QVERIFY(resolveTextFace(editor->fallbackFace).italic || resolveTextFace(editor->faceAtSelection()).italic);
    editor->finish(true);
    QCoreApplication::processEvents();
    const Layer *layer = session.activeLayer();
    QVERIFY(layer && layer->text.has_value());
    const TextFace saved = resolveTextFace(layer->text->fontName);
    QVERIFY(saved.bold() && saved.italic);
    // A click starts the first letter on the pointer: the box sits its padding to the left and the baseline's height above.
    QVERIFY(std::abs(layer->transform.origin.x() - (40 - kTextPadding)) < 0.5);
    QVERIFY(layer->transform.origin.y() < 60 - kTextPadding);
}

QTEST_MAIN(TestTextLayers)
#include "TestTextLayers.moc"
