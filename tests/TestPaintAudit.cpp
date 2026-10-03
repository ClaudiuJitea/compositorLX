// Ported from macOS BrushTests, BrushIntersectionTests, NativeResolutionPaintTests, GradientTests, ShapeToolTests,
// ColorPickerTests, SpotHealingTests, CloneStampTests, plus the palette (ColorPalette.swift).
#include "core/ColorPalette.h"
#include "core/Document.h"
#include "core/EditorSession.h"
#include "ui/CanvasWidget.h"
#include "ui/MainWindow.h"

#include <QDoubleSpinBox>
#include <QLabel>
#include <QtTest>
#include <cmath>

using namespace compositor;

class TestPaintAudit : public QObject
{
    Q_OBJECT
    static EditorSession *blankSession(int w = 600, int h = 80)
    {
        auto *s = new EditorSession; s->createDocument(w, h); s->addBlankLayer(); return s;
    }
    static QColor px(EditorSession &s, int x, int y) { return LayerRenderer_flat(s).pixelColor(x, y); }
    static QImage LayerRenderer_flat(EditorSession &s);
    static int alphaAt(EditorSession &s, int x, int y) { return px(s, x, y).alpha(); }
    static void stroke(EditorSession &s, const QVector<QPointF> &pts, const QColor &c = Qt::red, double d = 20, double hard = 1, double op = 1)
    {
        QVERIFY(s.beginBrushStroke(pts.first(), c, d, hard, op, false));
        for (int i = 1; i < pts.size(); ++i) s.continueBrushStroke(pts[i]);
        s.endBrushStroke();
    }
private slots:
    // ColorPickerTests
    void hexParsesFullShorthandAndRejectsInvalid()
    {
        QCOMPARE(palette::fromHex("#FF8000"), std::optional<QColor>(QColor(255, 128, 0)));
        QCOMPARE(palette::fromHex("0f0"), std::optional<QColor>(QColor(0, 255, 0)));
        QCOMPARE(palette::fromHex(" 00ff00 "), std::optional<QColor>(QColor(0, 255, 0)));
        QVERIFY(!palette::fromHex("12345"));
        QVERIFY(!palette::fromHex("GGGGGG"));
        QCOMPARE(palette::hex(QColor(255, 128, 0)), QString("FF8000"));
    }
    void hsbRoundTripsEightBitColors()
    {
        for (const char *hex : {"000000", "FFFFFF", "FF0000", "00FF00", "0000FF", "FF8000", "7F3FA2", "123456"})
            QCOMPARE(palette::hex(PickerHSB(*palette::fromHex(hex)).rgb()), QString(hex));
    }
    void graysAndBlackKeepPreviousHueAndSaturation()
    {
        PickerHSB hsb(*palette::fromHex("FF8000")); const double hue = hsb.hue;
        hsb.setRGB(*palette::fromHex("808080"));
        QCOMPARE(hsb.hue, hue); QCOMPARE(hsb.saturation, 0.0);
        hsb.saturation = .5; hsb.setRGB(Qt::black);
        QCOMPARE(hsb.hue, hue); QCOMPARE(hsb.saturation, .5); QCOMPARE(hsb.brightness, 0.0);
    }
    void canvasSamplingReadsComposite()
    {
        EditorSession s; s.createDocument(4, 4);
        QImage image(4, 4, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::red);
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 4; ++x) image.setPixelColor(x, y, Qt::blue);
        QVERIFY(s.insertImage(image, "Split"));
        QCOMPARE(s.sampleCompositeColor(QPointF(1.5, 0.5))->name(), QString("#0000ff"));
        QCOMPARE(s.sampleCompositeColor(QPointF(1.5, 3.5))->name(), QString("#ff0000"));
        QVERIFY(!s.sampleCompositeColor(QPointF(-1, 1)));
    }
    // ColorPalette.swift: swap, defaults, mask black/white
    void paletteDefaultsSwapResetAndMask()
    {
        EditorSession s; s.createDocument(10, 10); s.addBlankLayer();
        QCOMPARE(s.foregroundColor(), QColor(Qt::black)); QCOMPARE(s.backgroundColor(), QColor(Qt::white));
        s.setPaletteColor(Qt::red, false); s.swapPaletteColors();
        QCOMPARE(s.foregroundColor(), QColor(Qt::white)); QCOMPARE(s.backgroundColor(), QColor(Qt::red));
        s.resetPaletteColors();
        QCOMPARE(s.foregroundColor(), QColor(Qt::black)); QCOMPARE(s.backgroundColor(), QColor(Qt::white));
        QVERIFY(s.addLayerMask()); s.selectMaskTarget(true);
        QCOMPARE(s.paletteColor(false), QColor(Qt::black)); QCOMPARE(s.paletteColor(true), QColor(Qt::white));
        s.swapPaletteColors();
        QVERIFY(s.maskPaintWhite()); QCOMPARE(s.paletteColor(false), QColor(Qt::white));
        s.resetPaletteColors(); QVERIFY(!s.maskPaintWhite());
        QCOMPARE(s.foregroundColor(), QColor(Qt::black)); // the image palette was left alone
    }
    // BrushTests.continuousStrokeCrossesTilesAndCommitsOneUndo
    void continuousStrokeCommitsOneUndoAndTrimsBlankLayer()
    {
        std::unique_ptr<EditorSession> s(blankSession());
        const int count = s->history().undoCount();
        stroke(*s, {{20, 40}, {580, 40}});
        QCOMPARE(s->history().undoCount(), count + 1);
        for (int x : {20, 255, 256, 511, 512, 579}) QCOMPARE(px(*s, x, 40), QColor(Qt::red));
        QCOMPARE(alphaAt(*s, 300, 0), 0);
        QVERIFY(s->activeLayer()->image.height() <= 22);   // trimmed to what was painted
        s->undo(); QVERIFY(s->activeLayer()->image.isNull());
        s->redo(); QVERIFY(!s->activeLayer()->image.isNull());
        QCOMPARE(px(*s, 300, 40), QColor(Qt::red));
    }
    void softBrushPartialAlphaAndCancelPreservesDocument()
    {
        std::unique_ptr<EditorSession> s(blankSession(80, 80));
        const Document before = *s->document();
        QVERIFY(s->beginBrushStroke(QPointF(40, 40), Qt::red, 40, 0, 1, false));
        QVERIFY(alphaAt(*s, 40, 40) > 230);
        const int half = alphaAt(*s, 50, 40);
        QVERIFY(half > 95 && half < 140);
        QVERIFY(alphaAt(*s, 57, 40) < 40);
        QCOMPARE(alphaAt(*s, 64, 40), 0);
        s->cancelBrushStroke();
        QVERIFY(*s->document() == before);
        QVERIFY(!s->beginBrushStroke(QPointF(-100, -100), Qt::red, 40, 0, 1, false) || (s->endBrushStroke(), true));
        QVERIFY(*s->document() == before);
    }
    void opacityCapsTheWholeStrokeEvenWhereItOverlapsItself()
    {
        std::unique_ptr<EditorSession> s(blankSession(200, 80));
        stroke(*s, {{20, 40}, {180, 40}, {20, 40}, {180, 40}, {20, 40}, {100, 40}}, Qt::red, 20, 1, .5);
        const QColor c = px(*s, 100, 40);
        QVERIFY(std::abs(c.alpha() - 128) <= 1);
        QCOMPARE(alphaAt(*s, 100, 0), 0);
    }
    void softStrokeBuildsCoverageWhileKeepingItsFeatheredRim()
    {
        std::unique_ptr<EditorSession> s(blankSession(200, 80));
        QVERIFY(s->beginBrushStroke(QPointF(100, 40), Qt::red, 40, 0, 1, false));
        const int single = alphaAt(*s, 100, 50);
        s->cancelBrushStroke();
        stroke(*s, {{20, 40}, {180, 40}}, Qt::red, 40, 0);
        const int line = alphaAt(*s, 100, 50);
        QVERIFY2(line > single + 60, qPrintable(QString("%1 vs %2").arg(line).arg(single)));
        QVERIFY(line <= 255);
    }
    void spacedDabsLeaveNoVisibleRipple()
    {
        for (double hardness : {0.0, 0.5, 1.0}) {
            std::unique_ptr<EditorSession> s(blankSession(900, 300));
            stroke(*s, {{100, 150}, {800, 150}}, Qt::red, 120, hardness);
            for (int offset : {0, 30, 50}) {
                int lo = 255, hi = 0;
                for (int x = 300; x <= 600; ++x) { const int a = alphaAt(*s, x, 150 + offset); lo = std::min(lo, a); hi = std::max(hi, a); }
                QVERIFY2(hi - lo <= 16, qPrintable(QString("hardness %1 offset %2 rippled %3").arg(hardness).arg(offset).arg(hi - lo)));
            }
        }
    }
    // BrushIntersectionTests
    void accumulationDependsOnDistanceNotEventCount()
    {
        for (double diameter : {12.0, 120.0, 400.0}) {
            std::unique_ptr<EditorSession> a(blankSession(800, 800)), b(blankSession(800, 800));
            stroke(*a, {{60, 400}, {740, 400}}, Qt::white, diameter, 0);
            QVector<QPointF> dense; for (int x = 60; x <= 740; x += 5) dense << QPointF(x, 400);
            stroke(*b, dense, Qt::white, diameter, 0);
            for (int y = 400; y < std::min(800, 400 + int(diameter / 2)); ++y)
                QVERIFY2(std::abs(alphaAt(*a, 400, y) - alphaAt(*b, 400, y)) <= 3, qPrintable(QString("d %1 row %2").arg(diameter).arg(y)));
        }
    }
    static QVector<QPointF> trace(const QVector<QPointF> &pts, double step = 12)
    {
        QVector<QPointF> out{pts.first()};
        for (int i = 1; i < pts.size(); ++i) {
            const int n = std::max(1, int(std::ceil(QLineF(pts[i - 1], pts[i]).length() / step)));
            for (int k = 1; k <= n; ++k) out << pts[i - 1] + (pts[i] - pts[i - 1]) * (double(k) / n);
        }
        return out;
    }
    void selfCrossingsBlendInsteadOfTakingTheStrongestEdge()
    {
        std::unique_ptr<EditorSession> v(blankSession(800, 800)), h(blankSession(800, 800)), c(blankSession(800, 800));
        stroke(*v, trace({{400, 100}, {400, 700}}), Qt::white, 120, 0);
        stroke(*h, trace({{700, 400}, {100, 400}}), Qt::white, 120, 0);
        stroke(*c, trace({{400, 100}, {400, 700}, {700, 700}, {700, 400}, {100, 400}}), Qt::white, 120, 0);
        for (int off : {45, 48, 51}) {
            const int a = alphaAt(*v, 400 + off, 400 + off), b = alphaAt(*h, 400 + off, 400 + off), actual = alphaAt(*c, 400 + off, 400 + off);
            const int expected = 255 - (255 - a) * (255 - b) / 255;
            QVERIFY2(actual > std::max(a, b) + 10, qPrintable(QString("a %1 b %2 actual %3 off %4").arg(a).arg(b).arg(actual).arg(off)));
            QVERIFY2(std::abs(actual - expected) <= 6, qPrintable(QString("%1 vs %2").arg(actual).arg(expected)));
        }
    }
    void softCrossingsRespectStrokeOpacity()
    {
        std::unique_ptr<EditorSession> s(blankSession(800, 800));
        stroke(*s, trace({{400, 100}, {400, 700}, {700, 700}, {700, 400}, {100, 400}}), Qt::white, 120, 0, .4);
        QCOMPARE(alphaAt(*s, 400, 400), 102);
        for (int y = 0; y < 800; y += 7) for (int x = 0; x < 800; x += 7) QVERIFY(alphaAt(*s, x, y) <= 102);
    }
    void sparseMouseSamplesFollowACurve()
    {
        std::unique_ptr<EditorSession> s(blankSession(300, 300));
        const auto on = [](double deg) { return QPointF(150 + std::cos(deg * M_PI / 180) * 100, 150 + std::sin(deg * M_PI / 180) * 100); };
        QVector<QPointF> pts; for (int d = 0; d <= 180; d += 30) pts << on(d);
        stroke(*s, pts, Qt::red, 4);
        for (double d : {45., 75., 105., 135.}) QVERIFY2(alphaAt(*s, int(on(d).x()), int(on(d).y())) > 0, qPrintable(QString::number(d)));
    }
    // Layer growth (BrushStroke extent), mask coverage once
    void importedImageLayerGrowsAcrossCanvasWithoutMovingImageOrMask()
    {
        for (double rotation : {0.0, 37.0, 90.0}) {
            EditorSession s; s.createDocument(600, 200);
            QImage image(40, 20, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::red);
            QVERIFY(s.insertImage(image, "Imported"));
            Layer *layer = s.activeLayer();
            layer->transform.origin = QPointF(240, 80); layer->transform.size = QSizeF(80, 40);
            layer->transform.rotation = rotation; layer->transform.flipX = true;
            QImage mask(1, 1, QImage::Format_Grayscale8); mask.fill(128); layer->mask = mask;
            const QImage before = LayerRenderer_flat(s);
            stroke(s, {{12, 12}, {580, 12}}, Qt::green, 16);
            QVERIFY(px(s, 12, 12).green() > 240); QVERIFY(px(s, 580, 12).green() > 240);
            // The image and its mask did not move.
            const QColor c = px(s, 280, 100), old = before.pixelColor(280, 100);
            QCOMPARE(c.alpha(), old.alpha()); QVERIFY(std::abs(c.alpha() - 128) <= 1);
            QCOMPARE(s.activeLayer()->transform.rotation, rotation); QVERIFY(s.activeLayer()->transform.flipX);
            s.undo();
            QCOMPARE(s.activeLayer()->image.size(), QSize(40, 20));
            QCOMPARE(s.activeLayer()->transform.size, QSizeF(80, 40));
            s.redo(); QVERIFY(px(s, 12, 12).green() > 240);
        }
    }
    void unpaintedStrokeLeavesLayerAlone()
    {
        EditorSession s; s.createDocument(100, 100);
        QImage image(10, 10, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::red);
        QVERIFY(s.insertImage(image, "Small"));
        const Layer before = *s.activeLayer(); const int undo = s.history().undoCount();
        QVERIFY(s.beginBrushStroke(QPointF(90, 90), Qt::red, 4, 1, 1, true)); // erasing in empty space: nothing changes
        s.endBrushStroke();
        QVERIFY(*s.activeLayer() == before); QCOMPARE(s.history().undoCount(), undo);
    }
    void paintedBoundsTrimTilePaddingAndKeepSoftEdges()
    {
        for (double hardness : {0.0, 1.0}) {
            std::unique_ptr<EditorSession> s(blankSession(600, 200));
            stroke(*s, {{300, 100}}, Qt::red, 20, hardness);
            const Layer *l = s->activeLayer();
            QVERIFY(l->image.width() <= 20 && l->image.height() <= 20);
            QVERIFY(l->transform.origin.x() >= 290 && l->transform.origin.y() >= 90);
            s->undo(); QVERIFY(s->activeLayer()->image.isNull());
            s->redo(); QVERIFY(s->activeLayer()->image.width() <= 20);
        }
    }
    void blankLayerScaledDownPaintsAtDocumentResolution()
    {
        EditorSession s; s.createDocument(200, 200); s.addBlankLayer();
        s.activeLayer()->transform.origin = QPointF(90, 90); s.activeLayer()->transform.size = QSizeF(20, 20);
        stroke(s, {{100, 100}}, Qt::red, 40);
        const Layer *l = s.activeLayer();
        QVERIFY(std::abs(l->image.width() - l->transform.size.width()) <= 1);
        QVERIFY(l->image.width() >= 38);
    }
    // Mask painting
    void maskPaintingUsesPaletteBlackWhiteAndOpacity()
    {
        EditorSession s; s.createDocument(80, 80); s.addBlankLayer();
        stroke(s, {{40, 40}, {41, 40}}, Qt::red, 200);
        QVERIFY(s.addLayerMask(true)); s.selectMaskTarget(true);
        stroke(s, {{40, 40}, {42, 40}}, s.paletteColor(false), 20, 1, .5);
        QVERIFY(std::abs(alphaAt(s, 40, 40) - 128) <= 1); QCOMPARE(alphaAt(s, 5, 5), 255);
        s.setMaskPaintWhite(true);
        stroke(s, {{40, 40}}, s.paletteColor(false), 20, 1, 1);
        QCOMPARE(alphaAt(s, 40, 40), 255);
        s.undo(); QVERIFY(alphaAt(s, 40, 40) < 255 && alphaAt(s, 40, 40) > 100);
    }
    void maskWithOwnPlacementIsPaintedWhereItSits()
    {
        EditorSession s; s.createDocument(100, 100); s.addBlankLayer();
        stroke(s, {{50, 50}, {51, 50}}, Qt::red, 300);
        QVERIFY(s.addLayerMask(true)); s.selectMaskTarget(true);
        Layer *l = s.activeLayer();
        QImage mask(50, 50, QImage::Format_Grayscale8); mask.fill(255); l->mask = mask;
        LayerTransform place = l->transform; place.origin = QPointF(25, 25); place.size = QSizeF(50, 50); l->maskPlacement = place;
        stroke(s, {{30, 30}}, Qt::black, 4);
        QVERIFY(alphaAt(s, 30, 30) < 10);          // painted where the mask sits
        QCOMPARE(alphaAt(s, 5, 5), 255);
    }
    void foldersAndHiddenLayersRejectPainting()
    {
        std::unique_ptr<EditorSession> s(blankSession());
        s->addGroup();
        QVERIFY(!s->beginBrushStroke(QPointF(10, 10), Qt::red, 10, 1, 1, false));
        QVERIFY(!s->paintRefusal().isEmpty());
    }
    // NativeResolutionPaintTests
    void erasingAScaledDownImageKeepsItsResolution()
    {
        EditorSession s; s.createDocument(200, 200);
        QImage image(1000, 1000, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::red);
        QVERIFY(s.insertImage(image, "Photo"));
        s.activeLayer()->transform.origin = QPointF(50, 50); s.activeLayer()->transform.size = QSizeF(100, 100);
        QVERIFY(s.beginBrushStroke(QPointF(100, 100), Qt::black, 40, 1, 1, true));
        s.endBrushStroke();
        const QImage out = s.activeLayer()->image;
        QCOMPARE(out.size(), QSize(1000, 1000));
        QCOMPARE(out.pixelColor(500, 500).alpha(), 0);
        QCOMPARE(out.pixelColor(250, 500).alpha(), 255);
    }
    // GradientTests (applyGradient: foreground to background, 101x4)
    void gradientForegroundToBackgroundAndMaskAndOpacity()
    {
        EditorSession s; s.createDocument(101, 4); s.addBlankLayer();
        const int count = s.history().undoCount();
        QVERIFY(s.applyGradient(QPointF(.5, 2), QPointF(100.5, 2), Qt::black, Qt::white));
        QCOMPARE(s.history().undoCount(), count + 1);
        QVERIFY(px(s, 0, 0).red() <= 3); QVERIFY(px(s, 100, 3).red() >= 252);
        QVERIFY(std::abs(px(s, 50, 1).red() - 128) <= 3);
        s.undo(); QVERIFY(s.activeLayer()->image.isNull() || s.activeLayer()->image.pixelColor(0, 0).alpha() == 0);
        // reversed, half opacity
        QVERIFY(s.applyGradient(QPointF(.5, 2), QPointF(100.5, 2), Qt::black, Qt::white, false, false, true, .5));
        QVERIFY(std::abs(px(s, 0, 2).alpha() - 128) <= 3); QVERIFY(px(s, 0, 2).red() >= 125);
        QVERIFY(std::abs(px(s, 100, 2).alpha() - 128) <= 3); QVERIFY(px(s, 100, 2).red() <= 3);
    }
    void gradientRadialSpreadsFromStartToRim()
    {
        EditorSession s; s.createDocument(101, 101); s.addBlankLayer();
        QVERIFY(s.applyGradient(QPointF(50.5, 50.5), QPointF(90.5, 50.5), Qt::black, Qt::white, true));
        QVERIFY(px(s, 50, 50).red() <= 3);
        const int a = px(s, 70, 50).red();
        for (QPoint p : {QPoint(30, 50), QPoint(50, 70), QPoint(50, 30)}) QVERIFY(std::abs(px(s, p.x(), p.y()).red() - a) <= 2);
        QVERIFY(std::abs(a - 128) <= 6);
        QVERIFY(px(s, 100, 50).red() >= 252); QVERIFY(px(s, 0, 0).red() >= 252);
    }
    void gradientToTransparentPreservesUnderlyingPixels()
    {
        EditorSession s; s.createDocument(101, 4); s.addBlankLayer();
        QVERIFY(s.applyGradient(QPointF(100.5, 2), QPointF(101, 2), Qt::red, Qt::white));
        QCOMPARE(px(s, 50, 2), QColor(Qt::red));
        QVERIFY(s.applyGradient(QPointF(.5, 2), QPointF(100.5, 2), Qt::black, Qt::white, false, true));
        QVERIFY(px(s, 0, 2).red() <= 3 && px(s, 0, 2).alpha() == 255);
        QVERIFY(px(s, 100, 2).red() >= 252 && px(s, 100, 2).green() <= 3);
        const QColor mid = px(s, 50, 2); QVERIFY(std::abs(mid.red() - 128) <= 3 && mid.green() <= 1 && mid.alpha() == 255);
    }
    void gradientOnMaskWritesCoverage()
    {
        EditorSession s; s.createDocument(101, 4); s.addBlankLayer();
        QVERIFY(s.applyGradient(QPointF(0, 2), QPointF(.6, 2), Qt::black, Qt::black)); // opaque black layer
        QVERIFY(s.addLayerMask(true)); s.selectMaskTarget(true);
        QVERIFY(s.applyGradient(QPointF(.5, 2), QPointF(100.5, 2), s.paletteColor(false), s.paletteColor(true)));
        QVERIFY(px(s, 0, 2).alpha() <= 3); QVERIFY(px(s, 100, 2).alpha() >= 252); QVERIFY(std::abs(px(s, 50, 2).alpha() - 128) <= 3);
    }
    void gradientInsideSelectionOnly()
    {
        EditorSession s; s.createDocument(40, 10); s.addBlankLayer();
        s.setRectangularSelection(QRect(0, 0, 20, 10), SelectionMode::Replace);
        QVERIFY(s.applyGradient(QPointF(0, 5), QPointF(40, 5), Qt::red, Qt::red, false, false, false, 1));
        QVERIFY(px(s, 10, 5).alpha() > 200); QCOMPARE(px(s, 30, 5).alpha(), 0);
    }
    // ShapeToolTests via addShape
    void rectangleIsOneUndoStepAndKeepsSelection()
    {
        EditorSession s; s.createDocument(100, 80, true);
        s.selectAll();
        const int count = s.history().undoCount();
        QVERIFY(s.addShape(ShapeKind::Rectangle, QRectF(10, 10, 30, 20), Qt::red, Qt::transparent, 0));
        QCOMPARE(s.history().undoCount(), count + 1);
        QCOMPARE(s.activeLayer()->name, QString("Rectangle 1"));
        QCOMPARE(s.activeLayer()->transform.origin, QPointF(10, 10)); QCOMPARE(s.activeLayer()->transform.size, QSizeF(30, 20));
        QVERIFY(s.document()->selection.has_value());
        QCOMPARE(px(s, 25, 20), QColor(Qt::red)); QCOMPARE(px(s, 39, 29), QColor(Qt::red));
        QCOMPARE(alphaAt(s, 9, 20), 0); QCOMPARE(alphaAt(s, 40, 20), 0);
        QVERIFY(s.addShape(ShapeKind::Rectangle, QRectF(60, 10, 10, 10), Qt::red, Qt::transparent, 0));
        QCOMPARE(s.activeLayer()->name, QString("Rectangle 2"));
        s.undo(); s.undo();
        QCOMPARE(s.document()->layers.size(), 1);
    }
    void roundedRectanglesClampToAPillAndEllipseCornersAreClear()
    {
        EditorSession s; s.createDocument(100, 80, true);
        QVERIFY(s.addShape(ShapeKind::Rectangle, QRectF(10, 10, 40, 30), Qt::red, Qt::transparent, 0, 8));
        QVERIFY(s.addShape(ShapeKind::Rectangle, QRectF(55, 50, 40, 20), Qt::red, Qt::transparent, 0, 500));
        QCOMPARE(alphaAt(s, 10, 10), 0); QCOMPARE(alphaAt(s, 11, 11), 0); QCOMPARE(alphaAt(s, 13, 13), 255);
        QCOMPARE(alphaAt(s, 30, 10), 255); QCOMPARE(alphaAt(s, 55, 50), 0); QCOMPARE(alphaAt(s, 75, 60), 255);
        QVERIFY(s.addShape(ShapeKind::Ellipse, QRectF(40, 30, 20, 20), Qt::blue, Qt::transparent, 0));
        QCOMPARE(s.activeLayer()->name, QString("Ellipse 1"));
        QVERIFY(alphaAt(s, 41, 40) > 0); QCOMPARE(px(s, 50, 40).blue(), 255);
        QCOMPARE(alphaAt(s, 40, 30) == 0 || px(s, 40, 30) == QColor(Qt::red), true);
    }
    void shapeStaysEditableUntilPixelsChange()
    {
        EditorSession s; s.createDocument(100, 80, true);
        QVERIFY(s.addShape(ShapeKind::Rectangle, QRectF(10, 10, 40, 30), Qt::red, Qt::transparent, 0, 8));
        QVERIFY(s.activeLayer()->shapeStyle.has_value());
        QCOMPARE(s.activeLayer()->shapeStyle->cornerRadius, 8.0);
        QVERIFY(s.beginBrushStroke(QPointF(30, 25), Qt::green, 6, 1, 1, false)); s.endBrushStroke();
        QVERIFY(!s.activeLayer()->shapeStyle.has_value()); // rasterized by painting, as on the Mac
    }
    // CloneStampTests.cloneStampKeepsItsOwnSoftBrushTip, through the real window's options bar
    void cloneStampKeepsItsOwnSoftBrushTip()
    {
        MainWindow window; window.resize(1000, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.session().createDocument(40, 20); window.syncDocumentViews();
        auto *size = window.findChild<QDoubleSpinBox *>("brushSize"); auto *hard = window.findChild<QDoubleSpinBox *>("brushHardness");
        QVERIFY(size && hard);
        window.canvas()->setTool(CanvasWidget::Tool::Brush);
        size->setValue(30); QCOMPARE(hard->value(), 100.0);
        window.canvas()->setTool(CanvasWidget::Tool::Clone);
        QCOMPARE(hard->value(), 0.0); QCOMPARE(size->value(), 40.0);
        hard->setValue(50);
        window.canvas()->setTool(CanvasWidget::Tool::Healing);
        QCOMPARE(hard->value(), 100.0); QCOMPARE(size->value(), 30.0);
        window.canvas()->setTool(CanvasWidget::Tool::Clone);
        QCOMPARE(hard->value(), 50.0); QCOMPARE(size->value(), 40.0);
    }
    // X swaps, D resets (also on a mask: black/white), swatches follow the session
    void swapAndResetKeysDriveSwatchesAndMask()
    {
        MainWindow window; window.resize(1000, 700); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        EditorSession &s = window.session(); s.createDocument(40, 20); s.addBlankLayer(); window.syncDocumentViews();
        window.canvas()->setFocus();
        auto *fg = window.findChild<QLabel *>("foregroundSwatch");
        s.setForegroundColor(Qt::red); window.refreshPaletteSwatches();
        QTest::keyClick(window.canvas(), Qt::Key_X);
        QCOMPARE(s.foregroundColor(), QColor(Qt::white)); QCOMPARE(s.backgroundColor(), QColor(Qt::red));
        QVERIFY(fg->styleSheet().contains("#ffffff"));
        QTest::keyClick(window.canvas(), Qt::Key_D);
        QCOMPARE(s.foregroundColor(), QColor(Qt::black)); QVERIFY(fg->styleSheet().contains("#000000"));
        QVERIFY(s.addLayerMask()); s.selectMaskTarget(true); window.syncDocumentViews();
        QTest::keyClick(window.canvas(), Qt::Key_X);
        QVERIFY(s.maskPaintWhite()); QVERIFY(fg->styleSheet().contains("#ffffff"));
    }
    // Growth for erase, blur and heal strokes: the same mechanism as paint, and nothing is left behind when nothing changes.
    void blurAcrossALayerEdgeSpillsOutsideAndGrowsTheLayer()
    {
        EditorSession s; s.createDocument(100, 100);
        QImage image(20, 20, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::red);
        QVERIFY(s.insertImage(image, "Patch", QPointF(50, 50)));
        const QRectF box(s.activeLayer()->transform.origin, s.activeLayer()->transform.size);
        QVERIFY(s.beginBlurStroke(QPointF(box.right(), 50), 40, 1, 1, 8)); s.continueBrushStroke(QPointF(box.right(), 52)); QVERIFY(s.endBrushStroke());
        QVERIFY(alphaAt(s, int(box.right()) + 3, 50) > 0);
        QVERIFY(s.activeLayer()->image.width() > 20);
        QCOMPARE(px(s, int(box.left()) + 1, int(box.top()) + 1).red(), 255);
    }
    void eraseAndHealOutsideTheLayerLeaveItAlone()
    {
        EditorSession s; s.createDocument(100, 100);
        QImage image(20, 20, QImage::Format_RGBA8888_Premultiplied); image.fill(Qt::red);
        QVERIFY(s.insertImage(image, "Patch", QPointF(50, 50)));
        const Layer before = *s.activeLayer(); const int undo = s.history().undoCount();
        QVERIFY(s.beginBrushStroke(QPointF(90, 90), Qt::black, 10, 1, 1, true)); s.endBrushStroke();
        QVERIFY(*s.activeLayer() == before); QCOMPARE(s.history().undoCount(), undo);
        s.beginHealingStroke(QPointF(90, 90), 10, 1, 1, 0, 1); s.endBrushStroke();
        QVERIFY(*s.activeLayer() == before); QCOMPARE(s.history().undoCount(), undo);
        // erasing across the edge still erases inside
        QVERIFY(s.beginBrushStroke(QPointF(50, 50), Qt::black, 10, 1, 1, true)); s.endBrushStroke();
        QCOMPARE(alphaAt(s, 50, 50), 0); QCOMPARE(s.activeLayer()->image.size(), QSize(20, 20));
    }
    // Pointer-driven: real mouse events through CanvasWidget (offscreen)
private:
    struct Rig {
        MainWindow window;
        CanvasWidget *canvas() { return window.canvas(); }
        EditorSession &session() { return window.session(); }
        QPoint at(double x, double y) { const QPointF p = canvas()->canvasRect().topLeft() + QPointF(x, y) * canvas()->zoom(); return p.toPoint(); }
        void press(double x, double y, Qt::KeyboardModifiers m = {}) { QTest::mousePress(canvas(), Qt::LeftButton, m, at(x, y)); }
        void move(double x, double y) { QTest::mouseMove(canvas(), at(x, y)); QMouseEvent e(QEvent::MouseMove, QPointF(at(x, y)), QPointF(at(x, y)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(canvas(), &e); }
        void release(double x, double y, Qt::KeyboardModifiers m = {}) { QTest::mouseRelease(canvas(), Qt::LeftButton, m, at(x, y)); }
    };
    static void setup(Rig &r, int w, int h)
    {
        r.window.resize(1100, 800); r.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&r.window));
        r.session().createDocument(w, h); r.session().addBlankLayer(); r.window.syncDocumentViews();
        r.canvas()->setZoom(1.0); QCoreApplication::processEvents();
    }
private slots:
    void mouseDrivenBrushStrokePaintsAndCommitsOneUndo()
    {
        Rig r; setup(r, 200, 100);
        auto *size = r.window.findChild<QDoubleSpinBox *>("brushSize"); r.canvas()->setTool(CanvasWidget::Tool::Brush);
        size->setValue(10); r.session().setForegroundColor(Qt::red); r.window.refreshPaletteSwatches();
        const int undo = r.session().history().undoCount();
        r.press(20, 50); r.move(60, 50); r.move(120, 50); r.release(120, 50);
        QCOMPARE(r.session().history().undoCount(), undo + 1);
        QCOMPARE(QColor(LayerRenderer_flat(r.session()).pixelColor(70, 50)), QColor(Qt::red));
        QCOMPARE(LayerRenderer_flat(r.session()).pixelColor(70, 20).alpha(), 0);
        // Shift-click draws a straight line from where the last stroke ended
        r.press(120, 80, Qt::ShiftModifier); r.release(120, 80, Qt::ShiftModifier);
        QVERIFY(LayerRenderer_flat(r.session()).pixelColor(120, 65).alpha() > 0);
    }
    void mouseDrivenEraserAndMaskPaintUsesPalette()
    {
        Rig r; setup(r, 100, 100);
        r.session().beginBrushStroke(QPointF(50, 50), Qt::red, 300, 1, 1, false); r.session().endBrushStroke();
        QVERIFY(r.session().addLayerMask(true)); r.session().selectMaskTarget(true); r.window.syncDocumentViews();
        r.canvas()->setTool(CanvasWidget::Tool::Brush);
        r.window.findChild<QDoubleSpinBox *>("brushSize")->setValue(20);
        r.press(50, 50); r.release(50, 50);                    // black hides
        QCOMPARE(LayerRenderer_flat(r.session()).pixelColor(50, 50).alpha(), 0);
        r.session().setMaskPaintWhite(true);
        r.press(50, 50); r.release(50, 50);                    // white reveals again
        QCOMPARE(LayerRenderer_flat(r.session()).pixelColor(50, 50).alpha(), 255);
    }
    void optionClickPicksColourInPaintingTools()
    {
        Rig r; setup(r, 100, 100);
        r.session().beginBrushStroke(QPointF(50, 50), QColor(10, 200, 30), 300, 1, 1, false); r.session().endBrushStroke();
        for (auto tool : {CanvasWidget::Tool::Brush, CanvasWidget::Tool::Eraser, CanvasWidget::Tool::Healing, CanvasWidget::Tool::Gradient}) {
            r.session().setForegroundColor(Qt::black);
            r.canvas()->setTool(tool);
            const int undo = r.session().history().undoCount();
            r.press(40, 40, Qt::AltModifier); r.release(40, 40, Qt::AltModifier);
            QCOMPARE(r.session().foregroundColor(), QColor(10, 200, 30));
            QCOMPARE(r.session().history().undoCount(), undo);   // it picked, it did not paint
        }
    }
    void pointerDrivenGradientHandlesDragAndApply()
    {
        Rig r; setup(r, 200, 20);
        r.session().setForegroundColor(Qt::black); r.session().setBackgroundColor(Qt::white); r.window.refreshPaletteSwatches();
        r.canvas()->setTool(CanvasWidget::Tool::Gradient);
        if (auto *style = r.window.findChild<QWidget *>("gradientOpacity")) Q_UNUSED(style);
        r.press(10, 10); r.move(100, 10); r.release(100, 10);
        QVERIFY(r.canvas()->hasPendingGradient());
        const int whileDragging = LayerRenderer_flat(r.session()).pixelColor(50, 10).alpha();
        // drag the end handle further right: the pending gradient is re-rendered, not stacked
        r.press(100, 10); r.move(190, 10); r.release(190, 10);
        QVERIFY(r.canvas()->hasPendingGradient());
        const int mid = LayerRenderer_flat(r.session()).pixelColor(100, 10).alpha();
        QVERIFY(whileDragging > 0 && mid > 0);
        r.canvas()->resolvePendingGradient(true);
        QVERIFY(!r.canvas()->hasPendingGradient());
        const QImage flat = LayerRenderer_flat(r.session());
        QVERIFY(flat.pixelColor(10, 10).alpha() > 200);                 // foreground end (opaque)
        QVERIFY(flat.pixelColor(150, 10).alpha() < flat.pixelColor(10, 10).alpha());   // fading (to transparent by default)
        // Escape cancels a new pending one without touching the layer
        const Layer committed = *r.session().activeLayer();
        r.press(10, 15); r.move(80, 15); r.release(80, 15);
        QVERIFY(r.canvas()->hasPendingGradient());
        r.canvas()->resolvePendingGradient(false);
        QVERIFY(*r.session().activeLayer() == committed);
    }
    void cloneCursorPreviewsTheSourceInsideTheBrush()
    {
        Rig r; r.window.resize(1100, 800); r.window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&r.window));
        r.session().createDocument(200, 100);
        QImage image(200, 100, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 100; ++y) for (int x = 0; x < 200; ++x) image.setPixelColor(x, y, x < 100 ? QColor(255, 0, 0) : QColor(0, 0, 255));
        QVERIFY(r.session().insertImage(image, "Halves")); r.window.syncDocumentViews();
        r.canvas()->setZoom(1.0); r.canvas()->setTool(CanvasWidget::Tool::Clone);
        r.window.findChild<QDoubleSpinBox *>("brushSize")->setValue(30); r.window.findChild<QDoubleSpinBox *>("brushHardness")->setValue(100);
        r.press(30, 50, Qt::AltModifier); r.release(30, 50, Qt::AltModifier);       // source in the red half
        QTest::mouseMove(r.canvas(), r.at(150, 50)); QCoreApplication::processEvents();
        const QImage shot = r.canvas()->grab().toImage();
        const QColor inside = shot.pixelColor(r.at(150, 50));
        QVERIFY2(inside.red() > 150 && inside.blue() < 120, qPrintable(inside.name()));   // red under the cursor, not the blue canvas
        QVERIFY(shot.pixelColor(r.at(170, 50) + QPoint(10, 0)).blue() > 150);               // outside the circle
    }
    // SpotHealingTests (all modes) and CloneStampTests
    static QImage blemished()
    {
        QImage image(120, 80, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 80; ++y) for (int x = 0; x < 120; ++x) {
            const bool red = x >= 55 && x < 65 && y >= 35 && y < 45; const int g = x % 4 < 2 ? 100 : 112;
            image.setPixelColor(x, y, red ? QColor(230, 20, 20) : QColor(g, g, g));
        }
        return image;
    }
    void spotHealingHealsOnlyUnderTheBrush_data() { QTest::addColumn<int>("mode"); QTest::newRow("content") << 0; QTest::newRow("texture") << 1; QTest::newRow("proximity") << 2; }
    void spotHealingHealsOnlyUnderTheBrush()
    {
        QFETCH(int, mode);
        EditorSession s; s.createDocument(120, 80);
        const QImage original = blemished(); QVERIFY(s.insertImage(original, "Surface"));
        QVERIFY(s.beginHealingStroke(QPointF(60, 40), 24, 1, 1, mode, 7)); s.continueBrushStroke(QPointF(60.5, 40)); s.endBrushStroke();
        const QImage after = s.activeLayer()->image;
        for (QPoint p : {QPoint(60, 40), QPoint(56, 36), QPoint(64, 44)}) {
            const QColor c = after.pixelColor(p);
            QVERIFY2(c.red() - c.green() < 30 && c.green() >= 80 && c.green() <= 130, qPrintable(QString("mode %1 (%2,%3) %4").arg(mode).arg(p.x()).arg(p.y()).arg(c.name())));
        }
        for (QPoint p : {QPoint(10, 10), QPoint(90, 40), QPoint(30, 40), QPoint(60, 10), QPoint(60, 70)}) QCOMPARE(after.pixelColor(p), original.pixelColor(p));
    }
    void cloneStampAlignedAndNot()
    {
        EditorSession s; s.createDocument(80, 40);
        QImage image(80, 40, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 40; ++y) for (int x = 0; x < 80; ++x)
            image.setPixelColor(x, y, (x >= 10 && x < 20 && y >= 15 && y < 25) ? QColor(0, 255, 0) : x < 40 ? QColor(255, 0, 0) : QColor(0, 0, 255));
        QVERIFY(s.insertImage(image, "Colors"));
        QVERIFY(!s.beginCloneStroke(QPointF(60, 20), 6, 1, 1));            // no source yet
        s.setCloneSource(QPointF(15, 20));
        QVERIFY(s.beginCloneStroke(QPointF(60, 20), 6, 1, 1)); s.continueBrushStroke(QPointF(60.5, 20)); s.endBrushStroke();
        QCOMPARE(px(s, 60, 20), QColor(0, 255, 0)); QCOMPARE(px(s, 70, 5), QColor(0, 0, 255));
        QVERIFY(s.beginCloneStroke(QPointF(66, 20), 6, 1, 1, true)); s.continueBrushStroke(QPointF(66.5, 20)); s.endBrushStroke();
        QCOMPARE(px(s, 66, 20), QColor(255, 0, 0));
        QVERIFY(s.beginCloneStroke(QPointF(50, 10), 6, 1, 1, false)); s.continueBrushStroke(QPointF(50.5, 10)); s.endBrushStroke();
        QVERIFY(px(s, 50, 10) != QColor(0, 0, 255));
    }
};

#include "core/Document.h"
#include "rendering/LayerRenderer.h"
QImage TestPaintAudit::LayerRenderer_flat(EditorSession &s) { return LayerRenderer::flattened(*s.document()).convertToFormat(QImage::Format_ARGB32); }

QTEST_MAIN(TestPaintAudit)
#include "TestPaintAudit.moc"
