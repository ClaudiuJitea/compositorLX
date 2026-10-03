// Transform, distort, crop, guides, canvas/image size, trim and viewport behaviour, ported from the macOS tests:
// TransformTests, TransformPressTests, DistortTests, CropTests, GuideTests, ImageTrimTests, CanvasSizeTests,
// ImageSizeTests, CanvasEntryTests, CursorTests, DownsampleTests, ResizeSnapTests (see MAC_BASELINE).
#include "core/Document.h"
#include "core/EditorSession.h"
#include "core/ImageTrim.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "rendering/DownsampleCache.h"
#include "rendering/LayerRenderer.h"
#include "ui/CanvasRulerWidget.h"
#include "ui/CanvasWidget.h"
#include "ui/MainWindow.h"

#include <QDoubleSpinBox>
#include <QtTest>
#include <cmath>

using namespace compositor;

static QImage solid(int w, int h, QColor c) { QImage i(w, h, QImage::Format_RGBA8888_Premultiplied); i.fill(c); return i; }
static int alphaAt(const QImage &image, int x, int y) { return image.convertToFormat(QImage::Format_RGBA8888).constScanLine(y)[x * 4 + 3]; }
static bool near(const QPointF &a, const QPointF &b, double eps = 1e-4) { return std::hypot(a.x() - b.x(), a.y() - b.y()) < eps; }
static QPointF layerPoint(const LayerTransform &t, QPointF unit) { return t.unitToDocument().map(unit); }

class TestCanvasOps : public QObject
{
    Q_OBJECT
    static EditorSession paintedSession(int w = 400, int h = 300, QSize layer = QSize(100, 60))
    {
        EditorSession session; session.createDocument(w, h);
        session.insertImage(solid(layer.width(), layer.height(), Qt::red), QStringLiteral("Red"));
        return session;
    }
private slots:
    // TransformTests.rotatedResizeKeepsOppositeAnchorAtEveryHandle
    void rotatedResizeKeepsOppositeAnchor()
    {
        LayerTransform original; original.origin = {31, -19}; original.size = {200, 100}; original.rotation = 37;
        const QPoint signs[] = {{-1,-1},{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0}};
        for (const QPoint &s : signs) {
            const QPointF handle(0.5 + s.x() * 0.5, 0.5 + s.y() * 0.5), opposite(1 - handle.x(), 1 - handle.y());
            const QPointF start = layerPoint(original, handle);
            for (bool locked : {true, false}) {
                const LayerTransform changed = resizedByHandle(original, s, start, start + QPointF(34, 17), false, locked);
                QVERIFY(near(layerPoint(original, opposite), layerPoint(changed, opposite)));
                if (locked) QVERIFY(std::abs(changed.size.width() / changed.size.height() - 2) < 1e-4);
                QVERIFY(changed.isValid());
            }
        }
    }
    // Dragging a handle past the other side turns the layer over (mac TransformDrag.updated)
    void resizePastTheOppositeSideFlips()
    {
        LayerTransform original; original.origin = {100, 100}; original.size = {100, 50};
        const LayerTransform r = resizedByHandle(original, QPoint(1, 0), QPointF(200, 125), QPointF(60, 125), false, false);
        QVERIFY(r.flipX); QVERIFY(!r.flipY);
        QCOMPARE(r.size.width(), 40.0); QCOMPARE(r.size.height(), 50.0);
        QCOMPARE(r.origin.x(), 60.0);
    }
    // TransformTests.moveRotateAndShiftConstraints (resize half) and scalePercentSetsBothSidesAboutTheCenter
    void scalePercentAndRounding()
    {
        LayerTransform t; t.origin = {10, 20}; t.size = {400, 600}; t.rotation = 30;
        const QSizeF pixels(200, 200);
        QCOMPARE(t.scalePercent(pixels), 200.0);
        const LayerTransform s = t.scaled(50, pixels);
        QCOMPARE(s.size, QSizeF(100, 100));
        QVERIFY(near(s.center(), t.center(), 1e-3));
        QCOMPARE(s.rotation, 30.0);
        QCOMPARE(s.scalePercent(pixels), 50.0);
        LayerTransform f; f.origin = {1.4, 2.6}; f.size = {3.2, 0.2}; f.rotation = 10.6;
        const LayerTransform r = f.rounded();
        QCOMPARE(r.origin, QPointF(1, 3)); QCOMPARE(r.size, QSizeF(3, 1)); QCOMPARE(r.rotation, 11.0);
    }
    // following()/placing keep a mirror a mirror: a placed mask flipped with its layer must not become a 180 turn
    void followingKeepsReflections()
    {
        LayerTransform mask; mask.origin = {10, 10}; mask.size = {40, 20};
        LayerTransform from; from.origin = {0, 0}; from.size = {100, 100};
        const LayerTransform to = from.mirrored(true, 50);
        const LayerTransform moved = mask.following(from, to);
        // The mask's top-left corner (unit 0,0) lands where mirroring puts it: x 100-10 = 90.
        QVERIFY(near(layerPoint(moved, {0, 0}), QPointF(90, 10), 1e-6));
        QVERIFY(near(layerPoint(moved, {1, 1}), QPointF(50, 30), 1e-6));
    }
    // flipLayers + undo; masks follow
    void flipLayerAndUndo()
    {
        EditorSession s = paintedSession();
        const LayerTransform before = s.activeLayer()->transform;
        QVERIFY(s.flipLayers(true));
        QVERIFY(s.activeLayer()->transform.flipX);
        s.undo(); QCOMPARE(s.activeLayer()->transform, before);
    }
    // CanvasSizeTests.everyAnchorPreservesSourceAndTransform
    void everyAnchorOffsetsLayers()
    {
        for (int delta : {5, -5}) for (int anchor = 0; anchor <= 8; ++anchor) {
            EditorSession s; s.createDocument(64, 32);
            s.insertImage(solid(20, 10, Qt::blue), QStringLiteral("L"));
            Layer &l = s.document()->layers[0]; l.transform.rotation = 37; l.transform.flipX = true;
            const LayerTransform t = l.transform; const QImage img = l.image;
            QVERIFY(s.resizeCanvas(QSize(64 + delta, 32 + delta), anchor));
            const Layer &o = s.document()->layers[0];
            const int expected[] = {0, delta == 5 ? 2 : -3, delta};
            QCOMPARE(o.transform.origin.x(), t.origin.x() + expected[anchor % 3]);
            QCOMPARE(o.transform.origin.y(), t.origin.y() + expected[anchor / 3]);
            QCOMPARE(o.transform.size, t.size); QCOMPARE(o.transform.rotation, 37.0); QVERIFY(o.transform.flipX);
            QCOMPARE(o.image.cacheKey(), img.cacheKey());
            s.undo(); QCOMPARE(s.document()->canvasSize, QSize(64, 32));
        }
    }
    // CanvasSizeTests.coloredExtension...
    void coloredExtensionKeepsOldTransparencyAndRoundTrips()
    {
        EditorSession s; s.createDocument(4, 4); s.addBlankLayer();
        const Document before = *s.document();
        QVERIFY(s.resizeCanvas(QSize(8, 2), 4, QColor(255, 0, 0)));
        QCOMPARE(s.document()->layers.size(), 2);
        const QImage out = LayerRenderer::flattened(*s.document());
        QCOMPARE(out.size(), QSize(8, 2));
        QCOMPARE(alphaAt(out, 0, 0), 255); QCOMPARE(out.convertToFormat(QImage::Format_RGBA8888).constScanLine(0)[0], uchar(255));
        QCOMPARE(alphaAt(out, 3, 0), 0);
        QCOMPARE(alphaAt(out, 7, 1), 255);
        s.undo(); QVERIFY(*s.document() == before);
        s.redo(); QCOMPARE(s.document()->canvasSize, QSize(8, 2));
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("c.comp"));
        ProjectWriter::save(*s.document(), path);
        const Document loaded = ProjectReader::load(path);
        const QImage re = LayerRenderer::flattened(loaded);
        QCOMPARE(alphaAt(re, 3, 0), 0); QCOMPARE(alphaAt(re, 0, 0), 255);
    }
    // CanvasSizeTests.transparentResizeIsAllocationFree
    void hugeCanvasNeedsNoPixelsUnlessFilled()
    {
        EditorSession s; s.createDocument(4, 4);
        QVERIFY(s.resizeCanvas(QSize(30000, 30000), 4));
        QVERIFY(s.document()->layers.isEmpty());
        s.undo();
        QVERIFY(!s.resizeCanvas(QSize(30000, 30000), 4, QColor(Qt::white)));
        QCOMPARE(s.document()->canvasSize, QSize(4, 4));
        QVERIFY(s.resizeCanvas(QSize(2, 2), 4, QColor(Qt::white)));
        QVERIFY(s.document()->layers.isEmpty());
    }
    // ImageSizeTests
    void imageSizeKeepsIdentityResolutionAndUndo()
    {
        EditorSession s; s.createDocument(64, 32);
        s.insertImage(solid(64, 32, Qt::red), QStringLiteral("L")); s.document()->layers[0].transform.origin = {0, 0};
        const Document original = *s.document(); const QUuid id = s.activeLayer()->id;
        QVERIFY(s.resizeImage(QSize(128, 96), 300, Sampling::Nearest));
        QCOMPARE(s.document()->canvasSize, QSize(128, 96)); QCOMPARE(s.document()->resolution, 300.0);
        QCOMPARE(s.activeLayer()->id, id);
        QCOMPARE(s.activeLayer()->image.size(), QSize(128, 96));
        QVERIFY(qRed(s.activeLayer()->image.pixel(0, 0)) > 240);
        s.undo(); QVERIFY(*s.document() == original);
        s.redo(); QCOMPARE(s.document()->resolution, 300.0);
        QVERIFY(!s.resizeImage(QSize(30000, 30000), 72));
    }
    void resolutionOnlyKeepsPixelsAndSurvivesSave()
    {
        EditorSession s; s.createDocument(32, 16); s.addBlankLayer();
        const LayerTransform t = s.activeLayer()->transform;
        QVERIFY(s.resizeImage(QSize(32, 16), 300));
        QCOMPARE(s.activeLayer()->transform, t);
        QTemporaryDir dir; const QString path = dir.filePath(QStringLiteral("r.comp"));
        ProjectWriter::save(*s.document(), path);
        QCOMPARE(ProjectReader::load(path).resolution, 300.0);
        s.undo(); QCOMPARE(s.document()->resolution, 72.0);
    }
    void rotatedHiddenLayerScalesInDocumentAxes()
    {
        EditorSession s; s.createDocument(64, 32);
        s.insertImage(solid(64, 32, Qt::red), QStringLiteral("L"));
        Layer &l = s.document()->layers[0]; l.visible = false;
        l.transform.origin = {-16, 4}; l.transform.size = {64, 32}; l.transform.rotation = 90;
        QVERIFY(s.resizeImage(QSize(128, 96), 72, Sampling::Nearest));
        const Layer &o = s.document()->layers[0];
        QVERIFY(!o.visible); QCOMPARE(o.transform.rotation, 0.0);
        QVERIFY(std::abs(o.transform.size.width() - 64) <= 1); QVERIFY(std::abs(o.transform.size.height() - 192) <= 1);
        QVERIFY(o.transform.origin.y() < 0);
    }
    // Image Size scales a placed mask's placement and keeps its pixels
    void imageSizeScalesPlacedMaskPlacement()
    {
        EditorSession s; s.createDocument(100, 100);
        s.insertImage(solid(100, 100, Qt::red), QStringLiteral("L"));
        Layer &l = s.document()->layers[0]; l.transform.origin = {0, 0};
        l.mask = QImage(10, 10, QImage::Format_Grayscale8); l.mask.fill(255);
        LayerTransform p; p.origin = {20, 20}; p.size = {40, 40}; l.maskPlacement = p;
        QVERIFY(s.resizeImage(QSize(200, 200), 72));
        const Layer &o = s.document()->layers[0];
        QCOMPARE(o.mask.size(), QSize(10, 10));
        QVERIFY(o.maskPlacement); QVERIFY(near(o.maskPlacement->origin, QPointF(40, 40), 1e-6)); QCOMPARE(o.maskPlacement->size, QSizeF(80, 80));
    }
    // Flip Canvas mirrors a mask placed apart
    void flipCanvasMirrorsPlacedMasks()
    {
        EditorSession s; s.createDocument(100, 40);
        s.insertImage(solid(20, 20, Qt::red), QStringLiteral("L"));
        Layer &l = s.document()->layers[0];
        l.mask = QImage(4, 4, QImage::Format_Grayscale8); l.mask.fill(255);
        LayerTransform p; p.origin = {10, 5}; p.size = {20, 10}; l.maskPlacement = p;
        QVERIFY(s.flipCanvas(true));
        QCOMPARE(s.document()->layers[0].maskPlacement->origin.x(), 70.0);
        QVERIFY(s.document()->layers[0].maskPlacement->flipX);
    }
    // DistortTests
    void distortWarpsLayerIntoShapeOneUndoStep()
    {
        EditorSession s; s.createDocument(100, 60);
        s.insertImage(solid(20, 20, Qt::red), QStringLiteral("Red"));
        s.document()->layers[0].transform.origin = {10, 10};
        const std::array<QPointF, 4> shape{QPointF(10, 10), QPointF(60, 10), QPointF(30, 30), QPointF(10, 30)};
        const int count = s.history().undoCount();
        QVERIFY(s.distortSelectedLayers(shape));
        QCOMPARE(s.history().undoCount(), count + 1);
        const LayerTransform t = s.activeLayer()->transform;
        QCOMPARE(t.origin, QPointF(10, 10));
        // 50 x 20 shape bounds; the alpha trim may shave a faint anti-aliased column off the pointed corner
        QVERIFY(std::abs(t.size.width() - 50) <= 1); QCOMPARE(t.size.height(), 20.0); QCOMPARE(t.rotation, 0.0);
        const QImage out = LayerRenderer::flattened(*s.document());
        QCOMPARE(alphaAt(out, 50, 12), 255); QCOMPARE(alphaAt(out, 15, 25), 255);
        QCOMPARE(alphaAt(out, 50, 28), 0); QCOMPARE(alphaAt(out, 80, 12), 0);
        s.undo(); QCOMPARE(s.activeLayer()->transform.size, QSizeF(20, 20));
    }
    void distortedLayerIsTrimmedToVisiblePixels()
    {
        EditorSession s; s.createDocument(100, 60);
        QImage img = solid(40, 20, Qt::transparent);
        for (int y = 5; y < 15; ++y) for (int x = 15; x < 25; ++x) img.setPixelColor(x, y, Qt::red);
        s.insertImage(img, QStringLiteral("Stroke")); s.document()->layers[0].transform.origin = {10, 10};
        QVERIFY(s.distortSelectedLayers({QPointF(10, 10), QPointF(60, 10), QPointF(50, 30), QPointF(10, 30)}));
        const LayerTransform t = s.activeLayer()->transform;
        QVERIFY(t.size.width() < 20 && t.size.height() <= 12);
        QVERIFY(t.origin.x() >= 20 && t.origin.y() >= 14);
        QCOMPARE(alphaAt(LayerRenderer::flattened(*s.document()), 30, 20), 255);
    }
    // A folded (bow-tie) shape is a supported distortion since mac 1.1; one with a collapsed corner is refused
    void foldedShapeDistortsAndDegenerateIsRefused()
    {
        const QPointF a(10, 10), b(60, 10), c(30, 30), d(10, 30);
        EditorSession s; s.createDocument(100, 60);
        s.insertImage(solid(20, 20, Qt::red), QStringLiteral("Red")); s.document()->layers[0].transform.origin = {10, 10};
        QVERIFY(!s.distortSelectedLayers({a, a, c, d}));
        QVERIFY(s.distortSelectedLayers({a, c, b, d}));
        QVERIFY(alphaAt(LayerRenderer::flattened(*s.document()), 15, 20) == 255 || alphaAt(LayerRenderer::flattened(*s.document()), 25, 20) == 255);
    }
    // A rotated layer distorts from its own corners, not from the box around them
    void rotatedLayerDistortKeepsDisplayedCorners()
    {
        EditorSession s; s.createDocument(200, 200);
        s.insertImage(solid(40, 40, Qt::red), QStringLiteral("R"));
        Layer &l = s.document()->layers[0]; l.transform.origin = {80, 80}; l.transform.rotation = 45;
        const std::array<QPointF, 4> corners{QPointF(100, 70), QPointF(130, 100), QPointF(100, 130), QPointF(70, 100)};
        QVERIFY(s.distortSelectedLayers(corners));
        const LayerTransform t = s.activeLayer()->transform;
        QVERIFY(std::abs(t.origin.x() - 70) <= 1 && std::abs(t.size.width() - 60) <= 2);
    }
    // Two layers distort together
    void groupDistortWarpsEverySelectedLayer()
    {
        EditorSession s; s.createDocument(200, 100);
        s.insertImage(solid(20, 20, Qt::red), QStringLiteral("A")); s.document()->layers[0].transform.origin = {10, 10};
        s.insertImage(solid(20, 20, Qt::blue), QStringLiteral("B")); s.document()->layers[1].transform.origin = {50, 10};
        s.selectLayers({s.document()->layers[0].id, s.document()->layers[1].id}, s.document()->layers[1].id);
        QVERIFY(s.distortSelectedLayers({QPointF(10, 10), QPointF(130, 10), QPointF(130, 30), QPointF(10, 30)}));
        QVERIFY(s.document()->layers[1].transform.origin.x() > 50);
        QVERIFY(s.document()->layers[0].transform.size.width() > 20);
    }
    // CropTests
    void cropTranslatesWithoutResamplingAndUndoRestores()
    {
        EditorSession s; s.createDocument(64, 32); s.insertImage(solid(64, 32, Qt::green), QStringLiteral("L"));
        s.document()->layers[0].transform.origin = {0, 0};
        const Document before = *s.document(); const qint64 key = s.activeLayer()->image.cacheKey();
        QVERIFY(s.crop(QRect(8, 4, 32, 16)));
        QCOMPARE(s.document()->canvasSize, QSize(32, 16));
        QCOMPARE(s.document()->layers[0].transform.origin, QPointF(-8, -4));
        QCOMPARE(s.document()->layers[0].image.cacheKey(), key);
        s.undo(); QVERIFY(*s.document() == before);
        s.redo(); QCOMPARE(s.document()->canvasSize.width(), 32);
    }
    void sameSizeOffsetCropAndExpansionUseExactBounds()
    {
        EditorSession s; s.createDocument(100, 50); s.addBlankLayer();
        QVERIFY(s.crop(QRect(-20, 10, 100, 50)));
        QCOMPARE(s.document()->layers[0].transform.origin, QPointF(20, -10));
        QVERIFY(s.crop(QRect(-10, -10, 140, 80)));
        QCOMPARE(s.document()->canvasSize, QSize(140, 80));
        QCOMPARE(s.document()->layers[0].transform.origin, QPointF(30, 0));
        const QImage out = LayerRenderer::flattened(*s.document());
        QCOMPARE(out.size(), QSize(140, 80)); QCOMPARE(alphaAt(out, 0, 0), 0);
    }
    // CropTests.snapTargetsAreTheCanvasAndLayerBounds + GuideTests.snapTargetsFollowViewMenu
    void snapTargetsFollowViewMenu()
    {
        EditorSession s = paintedSession();
        auto set = [](const QVector<double> &v) { return QSet<double>(v.cbegin(), v.cend()); };
        QCOMPARE(set(s.cropSnapTargets().xs), (QSet<double>{0, 400, 150, 250}));
        QCOMPARE(set(s.cropSnapTargets().ys), (QSet<double>{0, 300, 120, 180}));
        const auto move = s.transformSnapTargets({});
        for (double v : {0.0, 200.0, 400.0, 150.0, 250.0}) QVERIFY(move.xs.contains(v));
        s.setSnapEnabled(false); QVERIFY(s.cropSnapTargets().xs.isEmpty() && s.cropSnapTargets().ys.isEmpty());
        s.setSnapEnabled(true); s.setSnapToLayers(false);
        QCOMPARE(set(s.cropSnapTargets().xs), (QSet<double>{0, 400}));
        s.setSnapToDocumentBounds(false); QVERIFY(s.cropSnapTargets().xs.isEmpty());
        s.setSnapToGuides(true); s.setShowsGuides(true);
        s.addGuide({QUuid::createUuid(), CanvasGuide::Axis::Vertical, 33});
        QCOMPARE(s.cropSnapTargets().xs, QVector<double>{33});
        s.setShowsGuides(false); QVERIFY(s.cropSnapTargets().xs.isEmpty());
        s.setShowsGuides(true); s.setShowsGrid(true); s.setSnapToGrid(true);
        QVERIFY(s.cropSnapTargets().xs.contains(64) && s.cropSnapTargets().xs.contains(8));
    }
    // GuideTests
    void guidesUndoClearLockAndRoundTrip()
    {
        EditorSession s; s.createDocument(200, 100);
        const CanvasGuide v{QUuid::createUuid(), CanvasGuide::Axis::Vertical, 40}, h{QUuid::createUuid(), CanvasGuide::Axis::Horizontal, 25};
        s.addGuide(v); s.addGuide(h);
        QCOMPARE(s.document()->guides.size(), 2); QVERIFY(s.canClearGuides());
        s.undo(); QCOMPARE(s.document()->guides, QVector<CanvasGuide>{v});
        s.clearGuides(); QVERIFY(s.document()->guides.isEmpty());
        s.undo(); QCOMPARE(s.document()->guides, QVector<CanvasGuide>{v});
        s.setLocksGuides(true);
        s.addGuide(h); QCOMPARE(s.document()->guides.size(), 1);
        s.beginGuideMove(v); QVERIFY(!s.guideDrag());
        s.clearGuides(); QVERIFY(s.document()->guides.isEmpty());
        s.setLocksGuides(false);
        s.undo(); s.addGuide(h);
        QTemporaryDir dir; const QString path = dir.filePath(QStringLiteral("g.comp"));
        ProjectWriter::save(*s.document(), path);
        QCOMPARE(ProjectReader::load(path).guides, s.document()->guides);
    }
    void canvasAndImageSizeMoveGuides()
    {
        EditorSession s; s.createDocument(100, 50);
        s.addGuide({QUuid::createUuid(), CanvasGuide::Axis::Vertical, 20}); s.addGuide({QUuid::createUuid(), CanvasGuide::Axis::Horizontal, 10});
        QVERIFY(s.resizeCanvas(QSize(140, 80), 8));
        QCOMPARE(s.document()->guides[0].position, 60.0); QCOMPARE(s.document()->guides[1].position, 40.0);
        s.undo();
        QVERIFY(s.resizeImage(QSize(200, 100), 72));
        QCOMPARE(s.document()->guides[0].position, 40.0); QCOMPARE(s.document()->guides[1].position, 20.0);
    }
    void flipCanvasMirrorsGuides()
    {
        EditorSession s; s.createDocument(100, 40);
        s.addGuide({QUuid::createUuid(), CanvasGuide::Axis::Vertical, 20}); s.addGuide({QUuid::createUuid(), CanvasGuide::Axis::Horizontal, 10});
        s.flipCanvas(true);
        QCOMPARE(s.document()->guides[0].position, 80.0); QCOMPARE(s.document()->guides[1].position, 10.0);
        s.flipCanvas(false);
        QCOMPARE(s.document()->guides[0].position, 80.0); QCOMPARE(s.document()->guides[1].position, 30.0);
    }
    void layoutGridLinesLimitsAndAppearance()
    {
        LayoutGrid grid;
        const auto lines = grid.lines(64);
        QCOMPARE(lines.first(), 0.0); QCOMPARE(lines.last(), 64.0); QVERIFY(lines.contains(8));
        QVERIFY(grid.isMajor(0) && grid.isMajor(64) && !grid.isMajor(8));
        LayoutGrid g(100, 4);
        QCOMPARE(g.lines(200), (QVector<double>{0, 25, 50, 75, 100, 125, 150, 175, 200}));
        LayoutGrid thirds(100, 3); int majors = 0; for (double v : thirds.lines(300)) majors += thirds.isMajor(v); QCOMPARE(majors, 4);
        QCOMPARE(LayoutGrid(50, 1).lines(120), (QVector<double>{0, 50, 100}));
        QVERIFY(LayoutGrid(0, 0) == LayoutGrid(2, 1));
        QCOMPARE(LayoutGrid(10, 40).subdivisions, 10);
        QCOMPARE(LayoutGrid(1000000, 1000).spacing, 4096); QCOMPARE(LayoutGrid(1000000, 1000).subdivisions, 64);
        GridAppearance a; QCOMPARE(a.preset, GridAppearance::Preset::LightGray);
        QVERIFY(std::abs(a.majorAlpha() - 0.45) < 1e-3 && std::abs(a.subdivisionAlpha() - 0.28) < 1e-3);
        a.opacity = 0; QCOMPARE(a.majorAlpha(), 0.01);
        a.opacity = 100; QVERIFY(a.subdivisionAlpha() < 1);
    }
    void gridSnapFollowsGridSettings()
    {
        EditorSession s = paintedSession();
        s.setSnapToLayers(false); s.setSnapToDocumentBounds(false); s.setShowsGrid(true); s.setSnapToGrid(true);
        const LayoutGrid saved = s.layoutGrid(); s.setLayoutGrid(LayoutGrid(100, 2));
        const QVector<double> xs = s.cropSnapTargets().xs;
        const double snapped = s.snappedGuidePosition(52, CanvasGuide::Axis::Vertical);
        s.setLayoutGrid(saved);   // the grid is shared by every session: put it back before asserting
        QCOMPARE(QSet<double>(xs.cbegin(), xs.cend()), (QSet<double>{0, 50, 100, 150, 200, 250, 300, 350, 400}));
        QCOMPARE(snapped, 50.0);
    }
    void rulerStepUsesNicePixelIntervals()
    {
        QCOMPARE(CanvasRulerWidget::majorStep(1), 100.0); QCOMPARE(CanvasRulerWidget::majorStep(8), 10.0);
        QCOMPARE(CanvasRulerWidget::label(0), QStringLiteral("0")); QCOMPARE(CanvasRulerWidget::label(250), QStringLiteral("250"));
    }
    void drawnPointsSnapToTargets()
    {
        EditorSession s = paintedSession();
        s.setSnapToLayers(false); s.setShowsGrid(true); s.setSnapToGrid(true);
        std::optional<double> gx, gy;
        QCOMPARE(s.snappedPoint(QPointF(62, 20), 3, &gx, &gy), QPointF(64, 20));
        QCOMPARE(s.snappedPoint(QPointF(397.5, 9), 3, &gx, &gy), QPointF(400, 8));
        QVERIFY(gx && *gx == 400 && gy && *gy == 8);
        s.setSnapEnabled(false);
        QCOMPARE(s.snappedPoint(QPointF(62, 20), 3), QPointF(62, 20));
    }
    // ResizeSnapTests
    void resizeSnapsDraggedEdgeToOtherLayers()
    {
        EditorSession s; s.createDocument(300, 200);
        s.insertImage(solid(20, 20, Qt::black), QStringLiteral("Other")); s.document()->layers[0].transform.origin = {150, 150}; s.document()->layers[0].transform.size = {40, 40};
        s.insertImage(solid(20, 20, Qt::black), QStringLiteral("Resized"));
        Layer &r = s.document()->layers[1]; r.transform.origin = {10, 10}; r.transform.size = {100, 100};
        const LayerTransform start = r.transform; const QSet<QUuid> moving{r.id};
        QPointF p = s.snappedResizePoint(QPointF(147, 60), start, QPointF(110, 60), QPoint(1, 0), false, false, moving, 5);
        LayerTransform out = resizedByHandle(start, QPoint(1, 0), QPointF(110, 60), p, false, false).rounded();
        QCOMPARE(out.origin.x() + out.size.width(), 150.0); QCOMPARE(out.size.height(), 100.0);
        p = s.snappedResizePoint(QPointF(130, 60), start, QPointF(110, 60), QPoint(1, 0), false, false, moving, 5);
        out = resizedByHandle(start, QPoint(1, 0), QPointF(110, 60), p, false, false).rounded();
        QCOMPARE(out.origin.x() + out.size.width(), 130.0);
        // a turned layer does not snap
        LayerTransform turned = start; turned.rotation = 20;
        QCOMPARE(s.snappedResizePoint(QPointF(147, 60), turned, QPointF(110, 60), QPoint(1, 0), false, false, moving, 5), QPointF(147, 60));
    }
    // ImageTrimTests
    void trimRectsAndSession()
    {
        QImage img = solid(20, 20, Qt::transparent);
        for (int y = 4; y < 15; ++y) for (int x = 2; x < 17; ++x) img.setPixelColor(x, y, Qt::red);
        TrimOptions all; QCOMPARE(*ImageTrim::calculateTrimRect(img, all), QRect(2, 4, 15, 11));
        TrimOptions tb; tb.left = tb.right = false; QCOMPARE(*ImageTrim::calculateTrimRect(img, tb), QRect(0, 4, 20, 11));
        TrimOptions lr; lr.top = lr.bottom = false; QCOMPARE(*ImageTrim::calculateTrimRect(img, lr), QRect(2, 0, 15, 20));
        QImage framed = solid(16, 16, Qt::blue);
        for (int y = 3; y < 13; ++y) for (int x = 3; x < 13; ++x) framed.setPixelColor(x, y, Qt::yellow);
        TrimOptions tl; tl.basedOn = TrimBasedOn::TopLeftPixelColor; QCOMPARE(*ImageTrim::calculateTrimRect(framed, tl), QRect(3, 3, 10, 10));
        tl.bottom = tl.left = tl.right = false; QCOMPARE(*ImageTrim::calculateTrimRect(framed, tl), QRect(0, 3, 16, 13));
        QImage br = solid(16, 16, QColor(0, 255, 255));
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) if (x >= 12 || y >= 12) br.setPixelColor(x, y, Qt::magenta);
        TrimOptions bo; bo.basedOn = TrimBasedOn::BottomRightPixelColor; QCOMPARE(*ImageTrim::calculateTrimRect(br, bo), QRect(0, 0, 12, 12));
        QVERIFY(!ImageTrim::calculateTrimRect(solid(8, 8, QColor(100, 150, 200)), TrimOptions{TrimBasedOn::TopLeftPixelColor}));
        QVERIFY(!ImageTrim::calculateTrimRect(solid(8, 8, Qt::transparent), TrimOptions{}));
        QCOMPARE(ImageTrim::trimImage(solid(10, 10, Qt::transparent).copy(), TrimOptions{}).has_value(), false);

        EditorSession s; s.createDocument(100, 100); s.insertImage(solid(40, 40, QColor(200, 50, 50)), QStringLiteral("Sq"));
        s.document()->layers[0].transform.origin = {20, 30};
        QVERIFY(s.trim(TrimOptions{}));
        QCOMPARE(s.document()->canvasSize, QSize(40, 40));
        QVERIFY(std::abs(s.activeLayer()->transform.origin.x()) < 1e-3 && std::abs(s.activeLayer()->transform.origin.y()) < 1e-3);
        s.undo(); QCOMPARE(s.document()->canvasSize, QSize(100, 100)); QCOMPARE(s.activeLayer()->transform.origin, QPointF(20, 30));
    }
    // DownsampleTests
    void downsampleLevelsAndSharpness()
    {
        QImage src(1024, 512, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 512; ++y) for (int x = 0; x < 1024; ++x) src.setPixel(x, y, x % 2 ? 0xffffffff : 0xff000000);
        QCOMPARE(DownsampleCache::shared().image(src, 0.6).cacheKey(), src.cacheKey());
        const QImage eighth = DownsampleCache::shared().image(src, 0.125);
        QCOMPARE(eighth.size(), QSize(128, 64));
        QCOMPARE(DownsampleCache::shared().image(src, 0.3).width(), 512);
        QImage edge(4096, 64, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 4096; ++x) edge.setPixel(x, y, x < 2048 ? 0xff000000 : 0xffffffff);
        const QImage small = DownsampleCache::shared().image(edge, 0.125);
        int soft = 0; for (int x = 0; x < small.width(); ++x) { const int v = qRed(small.pixel(x, 4)); if (v > 40 && v < 215) ++soft; }
        QVERIFY2(soft <= 3, qPrintable(QString::number(soft)));
    }
    // CanvasEntryTests: stepping zoom levels (1 -> 1.25 -> 1.5 -> back 1.25)
    void keyboardZoomLevels()
    {
        QCOMPARE(CanvasWidget::keyboardZoomTarget(1.0, 1), 1.25);
        QCOMPARE(CanvasWidget::keyboardZoomTarget(1.25, 1), 1.5);
        QCOMPARE(CanvasWidget::keyboardZoomTarget(1.5, -1), 1.25);
        QCOMPARE(CanvasWidget::keyboardZoomTarget(16, 1), 16.0);
        QCOMPARE(CanvasWidget::keyboardZoomTarget(0.125, -1), 0.125);
        QCOMPARE(CanvasWidget::keyboardZoomTarget(0.7, 1), 1.0);
    }
    // Window-level behaviour
    void commandTWithoutSelectionTransformsLayerAndEscapeCancels()
    {
        MainWindow w; w.resize(900, 700); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        EditorSession &s = w.session(); s.createDocument(400, 300);
        s.insertImage(solid(100, 100, Qt::red), QStringLiteral("Red")); w.syncDocumentViews();
        const LayerTransform before = s.activeLayer()->transform;
        QAction *t = w.findChild<QAction *>(QStringLiteral("commandTransform")); QVERIFY(t);
        QCOMPARE(t->text(), QStringLiteral("Transform Layer"));
        t->trigger();
        QCOMPARE(w.canvas()->tool(), CanvasWidget::Tool::Move);
        const int undoBefore = s.history().undoCount();
        QTest::keyClick(w.canvas(), Qt::Key_Escape);                 // Escape cancels the pending transform
        QCOMPARE(s.activeLayer()->transform, before);
        QCOMPARE(s.history().undoCount(), undoBefore);
        t->trigger();
        auto *x = w.findChild<QDoubleSpinBox *>(QStringLiteral("transformX")); QVERIFY(x);
        x->setValue(5); QMetaObject::invokeMethod(x, "editingFinished");   // typed values apply as one undo step
        QCOMPARE(s.activeLayer()->transform.origin.x(), 5.0);
        QCOMPARE(s.history().undoCount(), undoBefore + 1);
        s.undo(); QCOMPARE(s.activeLayer()->transform, before);
    }
    void spaceHeldPansAndZoomToolDoubles()
    {
        MainWindow w; w.resize(900, 700); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        EditorSession &s = w.session(); s.createDocument(400, 300); s.addBlankLayer(); w.syncDocumentViews();
        CanvasWidget *c = w.canvas(); c->setZoom(1.0); c->setPanOffset({});
        c->setFocus();
        QTest::keyPress(c, Qt::Key_Space);
        QCOMPARE(c->cursor().shape(), Qt::OpenHandCursor);
        QTest::mousePress(c, Qt::LeftButton, {}, QPoint(300, 300));
        QMouseEvent mv(QEvent::MouseMove, QPointF(340, 330), QPointF(340, 330), Qt::LeftButton, Qt::LeftButton, {});
        QCoreApplication::sendEvent(c, &mv);
        QTest::mouseRelease(c, Qt::LeftButton, {}, QPoint(340, 330));
        QCOMPARE(c->panOffset(), QPointF(40, 30));
        QTest::keyRelease(c, Qt::Key_Space);
        c->setTool(CanvasWidget::Tool::Zoom);
        QTest::mouseClick(c, Qt::LeftButton, {}, c->rect().center());
        QCOMPARE(c->zoom(), 2.0);
        QTest::mouseClick(c, Qt::LeftButton, Qt::AltModifier, c->rect().center());
        QCOMPARE(c->zoom(), 1.0);
    }
    void hiddenControlsAndOptionCursors()
    {
        MainWindow w; w.resize(900, 700); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        EditorSession &s = w.session(); s.createDocument(400, 300);
        s.insertImage(solid(100, 100, Qt::red), QStringLiteral("Red")); w.syncDocumentViews();
        CanvasWidget *c = w.canvas(); c->setZoom(1.0); c->setPanOffset({}); c->setTool(CanvasWidget::Tool::Move);
        const QPoint origin = c->canvasRect().topLeft().toPoint();
        auto move = [&](QPoint docPoint, Qt::KeyboardModifiers m) {
            const QPointF p = origin + docPoint;
            QMouseEvent e(QEvent::MouseMove, p, p, Qt::NoButton, Qt::NoButton, m); QCoreApplication::sendEvent(c, &e);
        };
        move({200, 150}, Qt::NoModifier); QCOMPARE(c->cursor().shape(), Qt::SizeAllCursor);
        move({200, 150}, Qt::AltModifier); QCOMPARE(c->cursor().shape(), Qt::DragCopyCursor);
        move({150, 100}, Qt::NoModifier); QVERIFY(c->cursor().shape() == Qt::SizeFDiagCursor);
        move({150, 100}, Qt::ControlModifier); QCOMPARE(c->cursor().shape(), Qt::CrossCursor);
        c->setShowTransformControls(false);
        move({150, 100}, Qt::NoModifier); QCOMPARE(c->cursor().shape(), Qt::SizeAllCursor);
    }
    // TransformPressTests: dragging outside the layer moves it (Control drags freely, skipping snapping)
    void draggingOutsideTheLayerMovesIt()
    {
        MainWindow w; w.resize(900, 700); w.show(); QVERIFY(QTest::qWaitForWindowExposed(&w));
        EditorSession &s = w.session(); s.createDocument(400, 300);
        s.insertImage(solid(100, 100, Qt::red), QStringLiteral("Red")); w.syncDocumentViews();
        CanvasWidget *c = w.canvas(); c->setZoom(1.0); c->setPanOffset({}); c->setTool(CanvasWidget::Tool::Move);
        const QPoint o = c->canvasRect().topLeft().toPoint();
        const QPoint a = o + QPoint(20, 20), b = o + QPoint(40, 30);
        QTest::mousePress(c, Qt::LeftButton, Qt::ControlModifier, a);
        QMouseEvent mv(QEvent::MouseMove, QPointF(b), QPointF(b), Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
        QCoreApplication::sendEvent(c, &mv);
        QTest::mouseRelease(c, Qt::LeftButton, Qt::ControlModifier, b);
        const QPointF origin = s.activeLayer()->transform.origin;
        QVERIFY2(near(origin, QPointF(170, 100 + 10), 0.5) || near(origin, QPointF(170, 110), 0.5), qPrintable(QString("%1,%2").arg(origin.x()).arg(origin.y())));
        s.undo(); QCOMPARE(s.activeLayer()->transform.origin, QPointF(150, 100));
    }
};

QTEST_MAIN(TestCanvasOps)
#include "TestCanvasOps.moc"
