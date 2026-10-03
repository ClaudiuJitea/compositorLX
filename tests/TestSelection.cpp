// Ported from macOS SelectionTests, SelectionEditTests, SelectionFeatherTests, SelectionClipboardTests, MagicWandTests, SmartEditTests.
#include "core/Document.h"
#include "core/EditorSession.h"
#include "core/SelectionOps.h"
#include "rendering/LayerRenderer.h"

#include <QtTest>

using namespace compositor;

class TestSelection : public QObject
{
    Q_OBJECT
    static QImage solid(int w, int h, QColor c) { QImage i(w, h, QImage::Format_RGBA8888_Premultiplied); i.fill(c); return i; }
    static int cov(const EditorSession &s, int x, int y) { return s.document()->selection ? s.document()->selection->convertToFormat(QImage::Format_Grayscale8).constScanLine(y)[x] : 0; }
    static QPolygonF square(double x, double y, double size) { return QPolygonF({QPointF(x, y), QPointF(x + size, y), QPointF(x + size, y + size), QPointF(x, y + size)}); }
    static QRect bounds(const EditorSession &s) { return SelectionOps::coverageBounds(*s.document()->selection); }
    static QImage render(const EditorSession &s) { return LayerRenderer::flattened(*s.document()); }
    static QColor px(const QImage &i, int x, int y) { return i.convertToFormat(QImage::Format_RGBA8888).pixelColor(x, y); }
    static EditorSession blank(int w = 100, int h = 100) { EditorSession s; s.createDocument(w, h, true); return s; }
    // 100x40, red left half, blue right half.
    static void twoColor(EditorSession &s)
    {
        s.createDocument(100, 40, true);
        QImage image = solid(100, 40, Qt::blue);
        for (int y = 0; y < 40; ++y) for (int x = 0; x < 50; ++x) std::fill_n(image.scanLine(y) + x * 4, 4, uchar(0)), image.scanLine(y)[x * 4] = 255, image.scanLine(y)[x * 4 + 3] = 255;
        s.activeLayer()->image = image;
    }
private slots:
    void replaceAddSubtract()
    {
        auto s = blank();
        s.setPolygonSelection(square(10, 10, 40));
        QVERIFY(cov(s, 30, 30) == 255 && cov(s, 70, 70) == 0);
        s.setPolygonSelection(square(50, 50, 40), SelectionMode::Add);
        QVERIFY(cov(s, 30, 30) == 255 && cov(s, 70, 70) == 255);
        s.setPolygonSelection(square(20, 20, 20), SelectionMode::Subtract);
        QVERIFY(cov(s, 30, 30) == 0 && cov(s, 15, 15) == 255);
        s.setPolygonSelection(square(60, 10, 20));
        QVERIFY(cov(s, 70, 20) == 255 && cov(s, 70, 70) == 0 && cov(s, 15, 15) == 0);
    }
    void modifiersPickMode()
    {
        QCOMPARE(SelectionOps::modeForModifiers(false, false, 0), 0);
        QCOMPARE(SelectionOps::modeForModifiers(true, false, 0), 1);
        QCOMPARE(SelectionOps::modeForModifiers(true, true, 0), 2);
        QCOMPARE(SelectionOps::modeForModifiers(false, true, 0), 2);
        QCOMPARE(SelectionOps::modeForModifiers(false, false, 1), 1);
        auto s = blank();
        s.setPolygonSelection(square(-50, -50, 100));
        QCOMPARE(bounds(s), QRect(0, 0, 50, 50));
    }
    void emptySelectionIsDistinctFromNone()
    {
        auto s = blank();
        s.setPolygonSelection(square(0, 0, 50), SelectionMode::Subtract);
        QVERIFY(!s.document()->selection);
        s.setPolygonSelection(square(10, 10, 20));
        s.setPolygonSelection(square(0, 0, 60), SelectionMode::Subtract);
        QVERIFY(s.document()->selection && !SelectionOps::hasCoverage(*s.document()->selection));
        QCOMPARE(cov(s, 20, 20), 0);
        s.deselect();
        QVERIFY(!s.document()->selection);
    }
    void clickDeselectsAndStepsUndo()
    {
        auto s = blank();
        const int count = s.history().undoCount();
        s.setPolygonSelection(square(10, 10, 40));
        QCOMPARE(s.history().undoCount(), count + 1); QCOMPARE(s.history().undoName(), QString("Lasso"));
        s.setPolygonSelection(QPolygonF({QPointF(5, 5)}));
        QVERIFY(!s.document()->selection); QCOMPARE(s.history().undoName(), QString("Deselect"));
        s.undo(); QVERIFY(s.document()->selection && SelectionOps::hasCoverage(*s.document()->selection));
        s.undo(); QVERIFY(!s.document()->selection);
        s.redo(); QCOMPARE(cov(s, 30, 30), 255);
    }
    void polygonalName()
    {
        auto s = blank();
        s.setPolygonSelection(square(10, 10, 80), SelectionMode::Replace, true, "Polygonal Lasso");
        QCOMPARE(s.history().undoName(), QString("Polygonal Lasso"));
        QVERIFY(cov(s, 80, 80) == 255 && cov(s, 5, 50) == 0);
    }
    void antialiasingControlsEdges()
    {
        auto s = blank();
        const QPolygonF tri({QPointF(0, 0), QPointF(100, 0), QPointF(0, 100)});
        s.setPolygonSelection(tri, SelectionMode::Replace, true);
        bool soft = false; for (int x = 0; x < 100; ++x) { const int c = cov(s, x, 99 - x); if (c > 0 && c < 255) soft = true; }
        QVERIFY(soft);
        s.setPolygonSelection(tri, SelectionMode::Replace, false);
        for (int x = 0; x < 100; ++x) { const int c = cov(s, x, 99 - x); QVERIFY(c == 0 || c == 255); }
    }
    void selectAllInverse()
    {
        auto s = blank();
        s.selectAll(); QVERIFY(cov(s, 0, 0) == 255 && cov(s, 99, 99) == 255);
        s.setPolygonSelection(square(0, 0, 50));
        s.invertSelection(); QVERIFY(cov(s, 25, 25) == 0 && cov(s, 75, 75) == 255);
        s.selectAll(); s.invertSelection();
        QVERIFY(!s.document()->selection);
        s.undo(); QVERIFY(s.document()->selection && SelectionOps::hasCoverage(*s.document()->selection));
    }
    void movingOutlineIsWholePixelsOneUndoAndLossless()
    {
        auto s = blank();
        s.setPolygonSelection(square(10, 10, 20));
        const int count = s.history().undoCount();
        QVERIFY(s.moveSelection(QPoint(40, 40)));
        QCOMPARE(s.history().undoCount(), count + 1); QCOMPARE(s.history().undoName(), QString("Move Selection"));
        QCOMPARE(bounds(s), QRect(50, 50, 20, 20));
        QVERIFY(cov(s, 55, 55) == 255 && cov(s, 15, 15) == 0);
        s.undo(); QCOMPARE(bounds(s), QRect(10, 10, 20, 20));
        // Off the canvas and back keeps the whole shape.
        QVERIFY(s.moveSelection(QPoint(-25, 0)));
        QCOMPARE(cov(s, 0, 20), 255);
        QVERIFY(s.moveSelection(QPoint(25, 0)));
        QCOMPARE(bounds(s), QRect(10, 10, 20, 20));
        // Arrow nudges are one undo step each.
        const int n = s.history().undoCount();
        s.moveSelection(QPoint(1, 0)); s.moveSelection(QPoint(0, -10));
        QCOMPARE(bounds(s), QRect(11, 0, 20, 20)); QCOMPARE(s.history().undoCount(), n + 2);
    }
    void expandContract()
    {
        auto s = blank();
        s.setPolygonSelection(square(40, 40, 20));
        QVERIFY(s.expandSelection(5));
        QCOMPARE(s.history().undoName(), QString("Expand Selection"));
        QCOMPARE(bounds(s), QRect(35, 35, 30, 30));
        QVERIFY(cov(s, 37, 50) == 255 && cov(s, 33, 50) == 0);
        // Rounded corners: the corner pixel of the grown box is not selected.
        QCOMPARE(cov(s, 35, 35), 0);
        QVERIFY(s.contractSelection(8));
        QCOMPARE(bounds(s), QRect(43, 43, 14, 14));
        s.undo(); QCOMPARE(bounds(s).width(), 30);
    }
    void expandStaysOnCanvasContractCanEmpty()
    {
        auto s = blank();
        s.selectAll();
        s.expandSelection(10);
        QCOMPARE(bounds(s), QRect(0, 0, 100, 100));
        QVERIFY(s.contractSelection(10));
        QVERIFY(cov(s, 5, 50) == 0 && cov(s, 50, 50) == 255);
        QVERIFY(s.contractSelection(45));
        QVERIFY(!SelectionOps::hasCoverage(*s.document()->selection));
        QVERIFY(!s.contractSelection(1));
    }
    void bigRadiusExpand()
    {
        auto s = blank(300, 300);
        s.setPolygonSelection(square(100, 100, 40));
        QVERIFY(s.expandSelection(40));
        QCOMPARE(bounds(s), QRect(60, 60, 120, 120));
        QCOMPARE(cov(s, 120, 65), 255);
        QCOMPARE(cov(s, 120, 55), 0);
    }
    void featherSoftensSelectionAndFill()
    {
        auto s = blank(60, 20);
        s.setPolygonSelection(square(20, 0, 20), SelectionMode::Replace, false);
        QVERIFY(s.featherSelection(6));
        int fading = 0; for (int x = 0; x < 60; ++x) { const int c = cov(s, x, 10); if (c > 8 && c < 247) ++fading; }
        QVERIFY2(fading >= 4, "no soft edge");
        QVERIFY(s.fillSelection(Qt::red));
        int soft = 0; for (int x = 0; x < 60; ++x) { const int a = s.activeLayer()->image.constScanLine(10)[x * 4 + 3]; if (a > 8 && a < 247) ++soft; }
        QVERIFY2(soft >= 4, "fill has a hard edge");
    }
    void marqueeBoxes()
    {
        auto s = blank();
        QVERIFY(s.setRectangularSelection(QRect(20, 30, 40, 41)));
        QCOMPARE(s.history().undoName(), QString("Rectangular Marquee"));
        QCOMPARE(bounds(s), QRect(20, 30, 40, 41));
        QVERIFY(cov(s, 20, 30) == 255 && cov(s, 19, 30) == 0);
        s.setRectangularSelection(QRect(80, 80, 10, 10), SelectionMode::Add);
        QVERIFY(cov(s, 85, 85) == 255 && cov(s, 40, 50) == 255);
        s.setRectangularSelection(QRect(30, 40, 20, 20), SelectionMode::Subtract);
        QVERIFY(cov(s, 40, 50) == 0 && cov(s, 25, 35) == 255);
        s.setRectangularSelection(QRect(5, 5, 0, 0));
        QVERIFY(!s.document()->selection);
        s.setRectangularSelection(QRect(5, 5, 0, 0), SelectionMode::Subtract);
        QVERIFY(!s.document()->selection);
    }
    void dragBoxShiftAndCenter()
    {
        QCOMPARE(SelectionOps::dragBox({60, 71}, {20.2, 30.3}, false, false), QRectF(20, 30, 40, 41));
        QCOMPARE(SelectionOps::dragBox({10, 10}, {40, 20}, true, false), QRectF(10, 10, 30, 30));
        QCOMPARE(SelectionOps::dragBox({50, 50}, {60, 55}, false, true), QRectF(40, 45, 20, 10));
        QCOMPARE(SelectionOps::dragBox({50, 50}, {45, 58}, true, true), QRectF(42, 42, 16, 16));
    }
    void ellipseMarquee()
    {
        auto s = blank();
        QVERIFY(s.setEllipticalSelection(QRect(10, 20, 60, 40)));
        const QRect b = bounds(s);
        QVERIFY(std::abs(b.left() - 10) <= 1 && std::abs(b.right() + 1 - 70) <= 1);
        QCOMPARE(cov(s, 40, 40), 255); QCOMPARE(cov(s, 11, 21), 0);
    }
    void layerAndMaskSelections()
    {
        auto s = blank(200, 200);
        QImage ring = solid(50, 50, Qt::transparent);
        QPainter p(&ring); p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(10, 10, 30, 30, Qt::red); p.fillRect(20, 20, 10, 10, Qt::transparent); p.fillRect(0, 0, 1, 1, QColor(255, 0, 0, 64)); p.end();
        QVERIFY(s.insertImage(ring, "Ring"));
        Layer *l = s.activeLayer(); l->transform.origin = {50, 50}; l->transform.size = {100, 100};
        QVERIFY(s.loadLayerAsSelection());
        QCOMPARE(s.history().undoName(), QString("Load Layer Selection"));
        QCOMPARE(bounds(s), QRect(70, 70, 60, 60));
        QVERIFY(cov(s, 75, 75) == 255 && cov(s, 100, 100) == 0 && cov(s, 50, 50) == 0);
        s.setPolygonSelection(square(0, 0, 20));
        QVERIFY(s.loadLayerAsSelection(SelectionMode::Add));
        QVERIFY(cov(s, 10, 10) == 255 && cov(s, 75, 75) == 255);
        // Mask: black areas select.
        s.deselect();
        QVERIFY(s.addLayerMask(true, false));
        QImage mask(50, 50, QImage::Format_Grayscale8); mask.fill(255);
        QPainter mp(&mask); mp.fillRect(10, 15, 20, 20, Qt::black); mp.end();
        s.activeLayer()->mask = mask;
        QVERIFY(s.loadMaskAsSelection());
        QCOMPARE(s.history().undoName(), QString("Load Mask Selection"));
        QCOMPARE(bounds(s), QRect(70, 80, 40, 40));
    }
    void wandContiguousToleranceSample()
    {
        auto s = blank(10, 4);
        QImage stripes = solid(10, 4, Qt::red);
        for (int y = 0; y < 4; ++y) for (int x = 3; x < 6; ++x) { uchar *p = stripes.scanLine(y) + x * 4; p[0] = 0; p[2] = 255; }
        QVERIFY(s.insertImage(stripes, "S"));
        QVERIFY(s.magicWand(QPoint(1, 2), 32, 0, true, false));
        QCOMPARE(bounds(s), QRect(0, 0, 3, 4));
        QVERIFY(s.magicWand(QPoint(1, 2), 32, 0, false, false));
        QCOMPARE(cov(s, 7, 1), 255); QCOMPARE(cov(s, 4, 1), 0);
        QCOMPARE(s.history().undoName(), QString("Magic Wand"));
        // Hard pixel edges whatever the anti-alias option.
        QVERIFY(s.magicWand(QPoint(1, 2), 32, 0, true, false, SelectionMode::Replace, true));
        QCOMPARE(cov(s, 3, 1), 0); QCOMPARE(cov(s, 2, 1), 255);
        // Nothing to subtract from.
        s.deselect();
        QVERIFY(!s.magicWand(QPoint(1, 2), 32, 0, true, false, SelectionMode::Subtract));
    }
    void wandTolerancePerChannelIncludingAlpha()
    {
        EditorSession s; s.createDocument(4, 1);
        QImage row(4, 1, QImage::Format_RGBA8888_Premultiplied);
        const uchar c[4][4] = {{100, 100, 100, 255}, {132, 100, 100, 255}, {133, 100, 100, 255}, {100, 100, 100, 222}};
        for (int x = 0; x < 4; ++x) std::copy_n(c[x], 4, row.scanLine(0) + x * 4);
        QVERIFY(s.insertImage(row, "R"));
        const auto picked = [&](int t) { s.magicWand(QPoint(0, 0), t, 0, false, false); QSet<int> r; for (int x = 0; x < 4; ++x) if (cov(s, x, 0)) r << x; return r; };
        QCOMPARE(picked(0), (QSet<int>{0}));
        QCOMPARE(picked(32), (QSet<int>{0, 1}));
        QCOMPARE(picked(33), (QSet<int>{0, 1, 2, 3}));
    }
    void wandSampleSizeAverages()
    {
        EditorSession s; s.createDocument(5, 5);
        QImage dot = solid(5, 5, Qt::black); dot.setPixelColor(2, 2, Qt::white);
        QVERIFY(s.insertImage(dot, "D"));
        s.magicWand(QPoint(2, 2), 10, 0, false, false);
        QCOMPARE(bounds(s), QRect(2, 2, 1, 1));
        s.magicWand(QPoint(2, 2), 30, 1, false, false);
        QCOMPARE(cov(s, 2, 2), 0); QCOMPARE(cov(s, 0, 0), 255);
    }
    void wandReadsActiveLayerOrAllAndLayerWithoutMask()
    {
        EditorSession s; s.createDocument(20, 10);
        QImage halves = solid(20, 10, Qt::blue);
        for (int y = 0; y < 10; ++y) for (int x = 0; x < 10; ++x) { uchar *p = halves.scanLine(y) + x * 4; p[0] = 255; p[2] = 0; }
        QVERIFY(s.insertImage(halves, "H"));
        s.addBlankLayer();
        s.magicWand(QPoint(2, 2), 32, 0, true, false);
        QCOMPARE(bounds(s), QRect(0, 0, 20, 10));       // blank active layer: transparent everywhere
        s.magicWand(QPoint(2, 2), 32, 0, true, true);
        QCOMPARE(bounds(s), QRect(0, 0, 10, 10));
        const int n = s.history().undoCount();
        s.magicWand(QPoint(15, 5), 32, 0, true, true, SelectionMode::Add);
        QCOMPARE(bounds(s), QRect(0, 0, 20, 10)); QCOMPARE(s.history().undoCount(), n + 1);
        s.magicWand(QPoint(2, 2), 32, 0, true, true, SelectionMode::Subtract);
        QCOMPARE(bounds(s), QRect(10, 0, 10, 10));
        s.undo(); QCOMPARE(bounds(s), QRect(0, 0, 20, 10));
        // A mask on the layer does not hide pixels from the wand.
        s.selectLayer(s.document()->layers.first().id);
        s.addLayerMask(false, false);
        s.magicWand(QPoint(2, 2), 32, 0, true, false);
        QCOMPARE(bounds(s), QRect(0, 0, 10, 10));
    }
    void fillClearAndMask()
    {
        EditorSession s; twoColor(s);
        s.setPolygonSelection(square(0, 0, 0));
        QVERIFY(s.setRectangularSelection(QRect(20, 10, 30, 20)));
        QVERIFY(s.fillSelection(Qt::green));
        QCOMPARE(s.history().undoName(), QString("Fill"));
        QCOMPARE(px(render(s), 30, 20), QColor(0, 255, 0));
        QCOMPARE(px(render(s), 5, 5), QColor(255, 0, 0));
        QVERIFY(s.clearSelectedPixels());
        QCOMPARE(s.history().undoName(), QString("Clear"));
        QCOMPARE(px(render(s), 30, 20).alpha(), 0);
        s.undo(); QCOMPARE(px(render(s), 30, 20), QColor(0, 255, 0));
        s.deselect();
        QVERIFY(s.fillSelection(Qt::white));
        QCOMPARE(px(render(s), 5, 5), QColor(255, 255, 255));
        // Empty selection edits nothing.
        s.setRectangularSelection(QRect(10, 10, 10, 10));
        s.setRectangularSelection(QRect(0, 0, 100, 40), SelectionMode::Subtract);
        const QImage before = s.activeLayer()->image;
        QVERIFY(!s.fillSelection(Qt::black)); QVERIFY(!s.clearSelectedPixels());
        QCOMPARE(s.activeLayer()->image, before);
        // Mask target: fill hides, clear reveals.
        s.deselect();
        QVERIFY(s.addLayerMask(true, false));
        s.selectMaskTarget(true);
        s.setRectangularSelection(QRect(20, 10, 30, 20));
        QVERIFY(s.fillSelection(Qt::black));
        QCOMPARE(s.history().undoName(), QString("Fill Mask"));
        QCOMPARE(px(render(s), 30, 20).alpha(), 0);
        QVERIFY(s.clearSelectedPixels());
        QCOMPARE(px(render(s), 30, 20).alpha(), 255);
    }
    void clipFollowsScaledLayers()
    {
        EditorSession s; s.createDocument(100, 40, true);
        QVERIFY(s.insertImage(solid(50, 20, Qt::blue), "Blue"));
        Layer *l = s.activeLayer(); l->transform.origin = {0, 0}; l->transform.size = {100, 40};
        s.setPolygonSelection(QPolygonF({QPointF(0, 0), QPointF(100, 0), QPointF(0, 40)}));
        QVERIFY(s.clearSelectedPixels());
        const QImage r = render(s);
        QCOMPARE(px(r, 10, 10).alpha(), 0); QCOMPARE(px(r, 90, 35).alpha(), 255);
    }
    void pixelMoveAndDuplicate()
    {
        EditorSession s; twoColor(s);
        s.setRectangularSelection(QRect(10, 10, 10, 10));
        const int count = s.history().undoCount();
        QVERIFY(s.nudgeSelectedPixels(QPoint(60, 5)));
        QCOMPARE(s.history().undoCount(), count + 1); QCOMPARE(s.history().undoName(), QString("Move Pixels"));
        QCOMPARE(bounds(s), QRect(70, 15, 10, 10));
        const QImage r = render(s);
        QCOMPARE(px(r, 15, 15).alpha(), 0);
        QCOMPARE(px(r, 75, 20), QColor(255, 0, 0)); QCOMPARE(px(r, 85, 20), QColor(0, 0, 255)); QCOMPARE(px(r, 5, 5), QColor(255, 0, 0));
        s.undo();
        QCOMPARE(bounds(s), QRect(10, 10, 10, 10)); QCOMPARE(px(render(s), 15, 15), QColor(255, 0, 0));
        // Duplicate keeps the source.
        QVERIFY(s.beginSelectionTransform(true, "Duplicate Pixels"));
        s.activeLayer()->transform.origin += QPointF(60, 5);
        QVERIFY(s.commitSelectionTransform());
        QCOMPARE(s.history().undoName(), QString("Duplicate Pixels"));
        QVERIFY(px(render(s), 15, 15).red() > 250 && px(render(s), 75, 20).red() > 250);
        s.undo(); QVERIFY(px(render(s), 75, 20).blue() > 250);
        // Masks refuse.
        s.addLayerMask(true, false);
        QVERIFY(s.isMaskSelected()); QVERIFY(!s.nudgeSelectedPixels(QPoint(1, 0)));
    }
    void transformSelectionEscapeAndUnchanged()
    {
        EditorSession s; twoColor(s);
        s.setRectangularSelection(QRect(10, 10, 10, 10));
        const Document before = *s.document();
        QVERIFY(s.beginSelectionTransform());
        s.activeLayer()->transform.origin += QPointF(30, 0);
        QVERIFY(s.cancelSelectionTransform());
        QCOMPARE(*s.document(), before);
        const int count = s.history().undoCount();
        s.setEllipticalSelection(QRect(10, 6, 30, 25));
        const Document ellipse = *s.document();
        QVERIFY(s.beginSelectionTransform());
        QVERIFY(s.commitSelectionTransform());
        QCOMPARE(*s.document(), ellipse);
        QCOMPARE(s.history().undoCount(), count + 1);
        // Moving past the layer edge grows the layer.
        s.setRectangularSelection(QRect(0, 0, 10, 10));
        QVERIFY(s.beginSelectionTransform());
        Layer *f = s.activeLayer(); f->transform.size = {20, 20}; f->transform.origin = {90, 30};
        QVERIFY(s.commitSelectionTransform());
        QCOMPARE(s.history().undoName(), QString("Transform Selection"));
        QCOMPARE(s.activeLayer()->transform.size, QSizeF(110, 50));
        QCOMPARE(px(render(s), 95, 35), QColor(255, 0, 0)); QCOMPARE(px(render(s), 5, 5).alpha(), 0);
        QCOMPARE(bounds(s), QRect(90, 30, 10, 10)); // clipped to the canvas
    }
    void copyPasteCutLayerViaCopy()
    {
        EditorSession s; twoColor(s);
        const QUuid source = *s.document()->activeLayerId;
        s.setRectangularSelection(QRect(40, 10, 20, 20));
        const auto copied = s.copiedPixels(false);
        QVERIFY(copied); QCOMPARE(copied->second, QPoint(40, 10)); QCOMPARE(copied->first.size(), QSize(20, 20));
        QCOMPARE(px(copied->first, 5, 5), QColor(255, 0, 0)); QCOMPARE(px(copied->first, 15, 5), QColor(0, 0, 255));
        QVERIFY(s.insertPixelLayer(copied->first, copied->second, QString(), "Paste"));
        QCOMPARE(s.history().undoName(), QString("Paste"));
        QCOMPARE(s.activeLayer()->name, QString("Layer 2"));
        QVERIFY(!s.document()->selection);
        QCOMPARE(s.activeLayer()->transform.origin, QPointF(40, 10));
        QVERIFY(*s.document()->activeLayerId != source);
        s.undo(); QCOMPARE(int(s.document()->layers.size()), 1);
        // Cut leaves a hole; paste restores.
        s.selectLayer(source);
        s.setRectangularSelection(QRect(10, 10, 10, 10));
        const auto cut = s.copiedPixels(false); QVERIFY(cut);
        QVERIFY(s.clearSelectedPixels());
        QCOMPARE(px(render(s), 15, 15).alpha(), 0);
        s.insertPixelLayer(cut->first, cut->second, QString(), "Paste");
        QCOMPARE(px(render(s), 15, 15), QColor(255, 0, 0));
    }
    void lassoShapedCopy()
    {
        EditorSession s; twoColor(s);
        s.setPolygonSelection(QPolygonF({QPointF(10, 5), QPointF(40, 5), QPointF(10, 35)}));
        const auto copied = s.copiedPixels(false); QVERIFY(copied);
        QCOMPARE(copied->second, QPoint(10, 5)); QCOMPARE(copied->first.size(), QSize(30, 30));
        QCOMPARE(px(copied->first, 5, 5), QColor(255, 0, 0));
        QCOMPARE(px(copied->first, 28, 28).alpha(), 0);
    }
    void copyMerged()
    {
        EditorSession s; twoColor(s);
        QVERIFY(s.insertImage(solid(20, 20, Qt::green), "Green"));
        Layer *g = s.activeLayer(); g->transform.origin = {60, 10}; g->transform.size = {20, 20};
        const QUuid green = g->id;
        s.setRectangularSelection(QRect(55, 5, 30, 30));
        auto active = s.copiedPixels(false); QVERIFY(active);
        QCOMPARE(px(active->first, 10, 10), QColor(0, 255, 0)); QCOMPARE(px(active->first, 3, 3).alpha(), 0);
        auto merged = s.copiedPixels(true); QVERIFY(merged);
        QCOMPARE(px(merged->first, 10, 10), QColor(0, 255, 0)); QCOMPARE(px(merged->first, 3, 3), QColor(0, 0, 255));
        s.selectLayer(green);
        s.document()->layers[s.document()->layers.size() - 1].visible = false;
        merged = s.copiedPixels(true); QVERIFY(merged);
        QCOMPARE(px(merged->first, 10, 10), QColor(0, 0, 255));
    }
    void contentAwareFillTexture()
    {
        EditorSession s; s.createDocument(80, 64);
        QImage img(80, 64, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 80; ++x) { const int v = (x / 4) % 2 == 0 ? 51 : 204; uchar *p = img.scanLine(y) + x * 4; p[0] = p[1] = p[2] = v; p[3] = 255; }
        for (int y = 26; y < 36; ++y) for (int x = 32; x < 44; ++x) { uchar *p = img.scanLine(y) + x * 4; p[0] = 255; p[1] = p[2] = 0; }
        QVERIFY(s.insertImage(img, "Stripes"));
        s.setRectangularSelection(QRect(32, 26, 12, 10));
        QVERIFY(s.contentAwareFill());
        QCOMPARE(s.history().undoName(), QString("Content-Aware Fill"));
        const QImage r = s.activeLayer()->image; int match = 0;
        for (int y = 26; y < 36; ++y) for (int x = 32; x < 44; ++x) if (std::abs(int(r.constScanLine(y)[x * 4]) - ((x / 4) % 2 == 0 ? 51 : 204)) <= 1) ++match;
        QVERIFY2(match >= 114, qPrintable(QString::number(match)));
    }
    void contentAwareFillReconstructsAndUndoes()
    {
        EditorSession s; s.createDocument(64, 48);
        QImage img = solid(64, 48, QColor(51, 153, 204));
        for (int y = 16; y < 26; ++y) for (int x = 20; x < 32; ++x) img.setPixelColor(x, y, Qt::red);
        QVERIFY(s.insertImage(img, "Object"));
        const QImage original = s.activeLayer()->image;
        s.setRectangularSelection(QRect(20, 16, 12, 10));
        const int n = s.history().undoCount();
        QVERIFY(s.contentAwareFill());
        QCOMPARE(s.history().undoCount(), n + 1);
        const QImage r = s.activeLayer()->image.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) { const QColor c = r.pixelColor(x, y); QVERIFY(std::abs(c.red() - 51) <= 1 && std::abs(c.green() - 153) <= 1 && std::abs(c.blue() - 204) <= 1 && c.alpha() == 255); }
        s.undo(); QCOMPARE(s.activeLayer()->image, original);
        // Nothing around to copy from: refused, layer untouched.
        s.selectAll();
        QVERIFY(!s.contentAwareFill());
        QCOMPARE(s.activeLayer()->image, original);
        s.deselect(); QVERIFY(!s.contentAwareFill());
    }
    void contentAwareFillReachesPastLayerEdge()
    {
        EditorSession s; s.createDocument(100, 100, true);
        QImage img = solid(40, 40, QColor(51, 153, 204));
        QVERIFY(s.insertImage(img, "Small", QPointF(50, 50)));
        const QRectF layerRect(s.activeLayer()->transform.origin, s.activeLayer()->transform.size);
        // Selection straddles the layer's right edge.
        const int edge = int(layerRect.right());
        s.setRectangularSelection(QRect(edge - 5, int(layerRect.top()) + 10, 15, 10));
        QVERIFY(s.contentAwareFill());
        const Layer *l = s.activeLayer();
        QVERIFY(l->transform.size.width() >= layerRect.width() + 9);
        QCOMPARE(px(render(s), edge + 5, int(layerRect.top()) + 15), QColor(51, 153, 204));
    }
    void fillRecolorsLiveText()
    {
        EditorSession s; s.createDocument(200, 80, true);
        QVERIFY(s.addText("Hi", QRectF(10, 10, 120, 50), "Sans", 40, false, false, false, 0, Qt::black, true));
        const QUuid id = *s.document()->activeLayerId;
        QVERIFY(s.fillSelection(Qt::red));
        const Layer *l = s.activeLayer();
        QCOMPARE(l->id, id);
        QVERIFY(l->text.has_value());
        QCOMPARE(l->text->red, 1.0); QCOMPARE(l->text->green, 0.0);
        QCOMPARE(s.history().undoName(), QString("Fill Text"));
    }
    void shortcutsAndDeselectNothingWhenAbsent()
    {
        auto s = blank();
        QVERIFY(!s.magicWand(QPoint(500, 500)));
        s.deselect(); QVERIFY(!s.history().canRedo());
    }
};

QTEST_MAIN(TestSelection)
#include "TestSelection.moc"
