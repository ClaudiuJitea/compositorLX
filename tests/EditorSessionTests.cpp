#include "core/EditorSession.h"
#include "rendering/LayerRenderer.h"
#include "rendering/RasterOperations.h"

#include <QElapsedTimer>
#include <QtTest>

using namespace compositor;

class EditorSessionTests final : public QObject {
    Q_OBJECT

private slots:
    void blankLayersInsertAboveSelection();
    void deletionChoosesNeighborAndPreservesCanvas();
    void renameAndVisibilityRoundTripThroughHistory();
    void reorderKeepsSelection();
    void nestedTransactionsUndoAsOne();
    void groupsOwnNewLayersAndDeleteTheirDescendants();
    void firstImageCreatesCanvasAndLaterImagesCenter();
    void selectionCommandsAreUndoable();
    void magicWandHonorsConnectivityAndTolerance();
    void pixelInvertKeepsAlphaAndIsUndoable();
    void pixelEditsRespectDocumentSelection();
    void brushPaintsAndErasesAsSingleUndoSteps();
    void brushRespectsSelection();
    void layerMasksAddToggleDeleteAndUndo();
    void selectionCreatesInverseLayerMask();
    void canvasResizeUsesNinePointAnchorAndIsUndoable();
    void canvasResizeCanFillOnlyTheExtension();
    void cropTranslatesContentAndSelection();
    void imageResizeScalesPixelsTransformsAndResolution();
    void layerAndCanvasFlipsMirrorPlacementAndSelection();
    void mergeDownAndFlattenBakeTheComposite();
    void selectionExpansionContractionAndMoveAreUndoable();
    void fillAndClearRespectSelectionAndMaskTarget();
    void ellipseAndPolygonSelectionsCombine();
    void clippingMasksPreserveBaseAlphaAndSupportHiddenSources();
    void copyAndPastePreserveSelectedPixelPosition();
    void levelsExposureGradientMapAndGrainPreserveAlphaAndUndo();
    void gaussianAndMotionBlurMatchDirectionAndSelection();
    void gradientAndShapeToolsCreateUndoablePixels();
    void softBrushOpacityCapAndMaskPaintingMatchBrushSettings();
    void contentAwareFillReconstructsSelectedObjectAndUndoes();
    void cloneStampKeepsAlignedSourceAndUsesBrushCoverage();
    void spotHealingUsesBrushCoverageAndUndoes();
    void blurBrushSoftensOnlyItsStrokeCoverage();
    void recoveredDocumentsStartDirtyWhileOpenedDocumentsStartSaved();
    void hueSaturationMatchesRangesAlphaSelectionAndUndo();
    void hueBandsWrapAndRejectCrossedHandles();
    void curvesUseShapePreservingChannelsSelectionAndUndo();
    void smudgeAndLiquifyWarpPixelsAsUndoableStrokes();
    void liveAdjustmentLayersRenderRemainEditableAndUndo();
    void levelsHistogramWeightsAlphaSelectionAndAutomaticModes();
    void independentAndFolderMasksUseDocumentPlacement();
    void allBlendModesUseCorrectColorAndAlphaMath();
    void floatingSelectionTransformsMergeCancelAndUndo();
    void gradientModesOpacityTransparencyAndMasks();
    void multiLayerNumericTransformsScaleRotateAndUndo();
    void noiseAndLensValidateSourceControlRangesAndUndo();
    void freeCornerDistortionBakesPixelsMasksAndUndo();
    void brushUsesCentripetalCurvesAndFinalizesTail();
    void layerDropOperationsCarryFoldersAndCopyPixels();
    void removeBackgroundComposesMasksSelectionAndUndo();
    void fourKBrushPerformanceWhenRequested();
    void selectionAntialiasingControlsDiagonalCoverage();
    void magicWandAntialiasingControlsBoundaryCoverage();
    void selectedLayerOpacitySkipsFoldersAndIsOneUndo();
    void selectedFolderTransformsVisibleDescendantsAsAGroup();
    void commandArrowNudgesSelectedPixelsInOneUndoStep();
    void duplicatePixelTransformPreservesTheSource();
    void cancellingBrushOrWarpRestoresWithoutHistory();
    void deletingLiveMaskSourceCanBakeDependentAppearance();
};

static EditorSession threeLayers()
{
    EditorSession session;
    session.createDocument(800, 600);
    for (int i = 0; i < 3; ++i) session.addBlankLayer();
    return session;
}

void EditorSessionTests::blankLayersInsertAboveSelection()
{
    EditorSession session = threeLayers();
    const QUuid first = session.document()->layers.constFirst().id;
    session.selectLayer(first);
    session.addBlankLayer();
    QCOMPARE(session.document()->layers.size(), 4);
    QCOMPARE(session.document()->layers.at(1).name, QStringLiteral("Layer 4"));
    QCOMPARE(session.document()->activeLayerId, std::optional<QUuid>(session.document()->layers.at(1).id));
    QVERIFY(session.document()->layers.at(1).image.isNull());
    QCOMPARE(session.document()->layers.at(1).transform.size, QSizeF(800, 600));
}

void EditorSessionTests::deletionChoosesNeighborAndPreservesCanvas()
{
    EditorSession session = threeLayers();
    const QUuid middle = session.document()->layers.at(1).id;
    session.selectLayer(middle);
    session.deleteActiveLayer();
    QCOMPARE(session.document()->layers.size(), 2);
    QVERIFY(session.document()->activeLayerId.has_value());
    session.deleteActiveLayer();
    session.deleteActiveLayer();
    QVERIFY(session.document()->layers.isEmpty());
    QVERIFY(!session.document()->activeLayerId.has_value());
    QCOMPARE(session.document()->canvasSize, QSize(800, 600));
}

void EditorSessionTests::renameAndVisibilityRoundTripThroughHistory()
{
    EditorSession session = threeLayers();
    const QUuid id = *session.document()->activeLayerId;
    session.renameLayer(id, QStringLiteral("  Foreground \n"));
    session.renameLayer(id, QStringLiteral(" \n "));
    QCOMPARE(session.activeLayer()->name, QStringLiteral("Foreground"));
    session.toggleLayerVisibility(id);
    QVERIFY(!session.activeLayer()->visible);
    session.undo();
    QVERIFY(session.activeLayer()->visible);
    session.redo();
    QVERIFY(!session.activeLayer()->visible);
}

void EditorSessionTests::selectedLayerOpacitySkipsFoldersAndIsOneUndo()
{
    EditorSession session; session.createDocument(20, 20, true);
    const QUuid first = *session.document()->activeLayerId;
    session.addBlankLayer(); const QUuid second = *session.document()->activeLayerId;
    session.addGroup(); const QUuid folder = *session.document()->activeLayerId;
    session.selectLayers({first, second, folder}, second);
    const int before = session.history().undoCount();
    session.setSelectedLayersOpacity(.45);
    QCOMPARE(session.history().undoCount(), before + 1);
    for (const Layer &layer : session.document()->layers) {
        if (layer.id == first || layer.id == second) QCOMPARE(layer.opacity, .45);
        if (layer.id == folder) QCOMPARE(layer.opacity, 1.0);
    }
    session.undo();
    for (const Layer &layer : session.document()->layers) QCOMPARE(layer.opacity, 1.0);
}

void EditorSessionTests::selectedFolderTransformsVisibleDescendantsAsAGroup()
{
    EditorSession session; QImage pixels(10, 10, QImage::Format_RGBA8888); pixels.fill(Qt::red);
    QVERIFY(session.insertImage(pixels, QStringLiteral("Child"))); const QUuid child = session.activeLayer()->id;
    session.activeLayer()->transform.origin = QPointF(10, 15);
    session.addGroup(); const QUuid folder = session.activeLayer()->id;
    session.selectLayer(child); QVERIFY(session.placeLayer(child, folder)); session.selectLayer(folder);
    QCOMPARE(session.selectedLayersBounds(), QRectF(10, 15, 10, 10));
    QVERIFY(session.transformSelectedLayers(QRectF(30, 25, 20, 30)));
    const Layer *moved = nullptr; for (const Layer &layer : session.document()->layers) if (layer.id == child) moved = &layer;
    QVERIFY(moved); QCOMPARE(moved->transform.origin, QPointF(30, 25)); QCOMPARE(moved->transform.size, QSizeF(20, 30));
    session.undo();
    for (const Layer &layer : session.document()->layers) if (layer.id == child) QCOMPARE(layer.transform.origin, QPointF(10, 15));
}

void EditorSessionTests::commandArrowNudgesSelectedPixelsInOneUndoStep()
{
    QImage image(12, 6, QImage::Format_RGBA8888); image.fill(Qt::transparent);
    image.setPixelColor(2, 2, Qt::red);
    EditorSession session; QVERIFY(session.insertImage(image, QStringLiteral("Pixel")));
    QVERIFY(session.setRectangularSelection(QRect(2, 2, 1, 1)));
    const Document before = *session.document(); const int undoCount = session.history().undoCount();
    QVERIFY(session.nudgeSelectedPixels(QPoint(3, 0)));
    QCOMPARE(session.history().undoCount(), undoCount + 1);
    QImage rendered = LayerRenderer::flattened(*session.document());
    QCOMPARE(rendered.pixelColor(2, 2).alpha(), 0); QVERIFY(rendered.pixelColor(5, 2).red() > 245);
    QVERIFY(session.document()->selection && session.document()->selection->pixelColor(5, 2).value() == 255);
    session.undo(); QCOMPARE(*session.document(), before);
}

void EditorSessionTests::duplicatePixelTransformPreservesTheSource()
{
    QImage image(12, 6, QImage::Format_RGBA8888); image.fill(Qt::transparent); image.setPixelColor(2, 2, Qt::red);
    EditorSession session; QVERIFY(session.insertImage(image, QStringLiteral("Pixel")));
    QVERIFY(session.setRectangularSelection(QRect(2, 2, 1, 1))); const Document before = *session.document();
    QVERIFY(session.beginSelectionTransform(true)); session.activeLayer()->transform.origin += QPointF(3, 0); QVERIFY(session.commitSelectionTransform());
    const QImage rendered = LayerRenderer::flattened(*session.document());
    QVERIFY(rendered.pixelColor(2, 2).red() > 245); QVERIFY(rendered.pixelColor(5, 2).red() > 245);
    QCOMPARE(session.history().undoName(), QStringLiteral("Duplicate Pixels")); session.undo(); QCOMPARE(*session.document(), before);
}

void EditorSessionTests::cancellingBrushOrWarpRestoresWithoutHistory()
{
    QImage image(30, 20, QImage::Format_RGBA8888); image.fill(Qt::transparent);
    EditorSession brush; QVERIFY(brush.insertImage(image, QStringLiteral("Blank"))); const Document before = *brush.document(); const int count = brush.history().undoCount();
    QVERIFY(brush.beginBrushStroke(QPointF(5, 5), Qt::red, 8)); brush.continueBrushStroke(QPointF(20, 10));
    QVERIFY(brush.cancelBrushStroke()); QCOMPARE(*brush.document(), before); QCOMPARE(brush.history().undoCount(), count); QVERIFY(!brush.isPainting());

    QImage split(30, 20, QImage::Format_RGBA8888); split.fill(Qt::red); QPainter painter(&split); painter.fillRect(QRect(15, 0, 15, 20), Qt::blue); painter.end();
    EditorSession warp; QVERIFY(warp.insertImage(split, QStringLiteral("Split"))); const Document warpBefore = *warp.document(); const int warpCount = warp.history().undoCount();
    QVERIFY(warp.beginWarpStroke(QPointF(12, 10), 0, 12, 1, 1)); warp.continueBrushStroke(QPointF(20, 10));
    QVERIFY(warp.cancelBrushStroke()); QCOMPARE(*warp.document(), warpBefore); QCOMPARE(warp.history().undoCount(), warpCount); QVERIFY(!warp.isPainting());
}

void EditorSessionTests::deletingLiveMaskSourceCanBakeDependentAppearance()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(10, 6);
    Layer source; source.id = QUuid::createUuid(); source.name = QStringLiteral("Mask source"); source.transform.size = document->canvasSize;
    source.image = QImage(document->canvasSize, QImage::Format_RGBA8888_Premultiplied); source.image.fill(Qt::transparent);
    QPainter sourcePainter(&source.image); sourcePainter.fillRect(0, 0, 5, 6, Qt::white); sourcePainter.end();
    Layer target; target.id = QUuid::createUuid(); target.name = QStringLiteral("Target"); target.transform.size = document->canvasSize;
    target.image = QImage(document->canvasSize, QImage::Format_RGBA8888_Premultiplied); target.image.fill(Qt::blue); target.maskSourceId = source.id;
    document->layers = {source, target}; document->activeLayerId = source.id;
    EditorSession session; session.setDocument(document); session.selectLayer(source.id);
    QCOMPARE(session.selectedDeletionLiveMaskDependents(), QSet<QUuid>{target.id});
    session.deleteSelectedLayers(true);
    QCOMPARE(session.document()->layers.size(), 1); QCOMPARE(session.activeLayer()->id, target.id); QVERIFY(!session.activeLayer()->maskSourceId);
    const QImage baked = session.activeLayer()->image;
    QVERIFY(baked.pixelColor(2, 2).blue() > 240); QCOMPARE(baked.pixelColor(8, 2).alpha(), 0);
    session.undo(); QCOMPARE(session.document()->layers.size(), 2); QCOMPARE(session.document()->layers.at(1).maskSourceId, std::optional<QUuid>(source.id));
}

void EditorSessionTests::reorderKeepsSelection()
{
    EditorSession session = threeLayers();
    const QUuid active = *session.document()->activeLayerId;
    QVERIFY(session.reorderLayers({0}, 3));
    QCOMPARE(session.document()->activeLayerId, std::optional<QUuid>(active));
    QCOMPARE(session.document()->layers.at(0).name, QStringLiteral("Layer 3"));
    QVERIFY(!session.canMoveActiveLayer(-1));
    session.moveActiveLayer(1);
    QCOMPARE(session.document()->layers.at(1).name, QStringLiteral("Layer 3"));
}

void EditorSessionTests::nestedTransactionsUndoAsOne()
{
    EditorSession session;
    session.createDocument(100, 200);
    const int count = session.history().undoCount();
    session.beginEdit(QStringLiteral("Layer Setup"));
    session.addBlankLayer();
    session.addBlankLayer();
    QVERIFY(!session.canUndo());
    session.endEdit();
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(session.history().undoName(), QStringLiteral("Layer Setup"));
    session.undo();
    QVERIFY(session.document()->layers.isEmpty());
    session.redo();
    QCOMPARE(session.document()->layers.size(), 2);
}

void EditorSessionTests::groupsOwnNewLayersAndDeleteTheirDescendants()
{
    EditorSession session;
    session.createDocument(100, 100);
    session.addGroup();
    const QUuid folder = *session.document()->activeLayerId;
    session.addBlankLayer();
    const QUuid child = session.activeLayer()->id;
    QCOMPARE(session.activeLayer()->parentId, std::optional<QUuid>(folder));
    QCOMPARE(session.descendantIds(folder).size(), 1);
    session.selectLayer(folder);
    session.deleteActiveLayer();
    QVERIFY(session.document()->layers.isEmpty());
    session.undo();
    QCOMPARE(session.document()->layers.size(), 2);
    session.selectLayer(child); QVERIFY(session.moveActiveLayerOutOfGroup()); QVERIFY(!session.activeLayer()->parentId.has_value());
    session.undo(); QCOMPARE(session.activeLayer()->parentId, std::optional<QUuid>(folder));
}

void EditorSessionTests::firstImageCreatesCanvasAndLaterImagesCenter()
{
    EditorSession session;
    QImage first(40, 20, QImage::Format_RGBA8888); first.fill(Qt::red);
    QVERIFY(session.insertImage(first, QStringLiteral("First")));
    QCOMPARE(session.document()->canvasSize, QSize(40, 20));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(0, 0));
    QImage second(10, 6, QImage::Format_RGBA8888); second.fill(Qt::blue);
    QVERIFY(session.insertImage(second, QStringLiteral("Second"), QPointF(8, 9)));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(3, 6));
    QCOMPARE(session.history().undoName(), QStringLiteral("Import Image"));
    session.undo();
    QCOMPARE(session.document()->layers.size(), 1);
}

void EditorSessionTests::selectionCommandsAreUndoable()
{
    EditorSession session; session.createDocument(20, 10);
    QVERIFY(session.setRectangularSelection(QRect(2, 3, 5, 4)));
    QVERIFY(session.document()->selection.has_value());
    QCOMPARE(qGray(session.document()->selection->pixel(3, 4)), 255);
    QCOMPARE(qGray(session.document()->selection->pixel(0, 0)), 0);
    session.invertSelection();
    QCOMPARE(qGray(session.document()->selection->pixel(3, 4)), 0);
    QCOMPARE(qGray(session.document()->selection->pixel(0, 0)), 255);
    session.undo();
    QCOMPARE(qGray(session.document()->selection->pixel(3, 4)), 255);
    session.deselect();
    QVERIFY(!session.document()->selection.has_value());
    session.undo();
    QVERIFY(session.document()->selection.has_value());
}

void EditorSessionTests::magicWandHonorsConnectivityAndTolerance()
{
    EditorSession session;
    QImage image(10, 4, QImage::Format_RGBA8888);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 10; ++x) image.setPixelColor(x, y, x < 3 || x >= 6 ? Qt::red : Qt::blue);
    QVERIFY(session.insertImage(image, QStringLiteral("Stripes")));
    QVERIFY(session.magicWand(QPoint(1, 2), 0, 0, true, false));
    int selected = 0;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 10; ++x) selected += qGray(session.document()->selection->pixel(x, y)) > 0;
    QCOMPARE(selected, 12);
    QVERIFY(session.magicWand(QPoint(1, 2), 0, 0, false, false));
    selected = 0;
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 10; ++x) selected += qGray(session.document()->selection->pixel(x, y)) > 0;
    QCOMPARE(selected, 28);
}

void EditorSessionTests::magicWandAntialiasingControlsBoundaryCoverage()
{
    EditorSession session;
    QImage image(20, 20, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
        image.setPixelColor(x, y, x <= y ? QColor(Qt::red) : QColor(Qt::blue));
    QVERIFY(session.insertImage(image, QStringLiteral("Diagonal")));
    QVERIFY(session.magicWand(QPoint(2, 17), 0, 0, true, false, SelectionMode::Replace, false));
    bool hardHasPartial = false;
    for (int y = 0; y < 20; ++y) for (int x = 0; x < 20; ++x) {
        const int coverage = qGray(session.document()->selection->pixel(x, y));
        hardHasPartial |= coverage > 0 && coverage < 255;
    }
    QVERIFY(!hardHasPartial);

    QVERIFY(session.magicWand(QPoint(2, 17), 0, 0, true, false, SelectionMode::Replace, true));
    bool softHasPartial = false;
    for (int y = 0; y < 20; ++y) for (int x = 0; x < 20; ++x) {
        const int coverage = qGray(session.document()->selection->pixel(x, y));
        softHasPartial |= coverage > 0 && coverage < 255;
    }
    QVERIFY(softHasPartial);
}

void EditorSessionTests::pixelInvertKeepsAlphaAndIsUndoable()
{
    EditorSession session;
    QImage image(1, 1, QImage::Format_RGBA8888); image.setPixelColor(0, 0, QColor(10, 30, 50, 128));
    QVERIFY(session.insertImage(image, QStringLiteral("Pixel")));
    const QRgb before = session.activeLayer()->image.convertToFormat(QImage::Format_RGBA8888).pixel(0, 0);
    QVERIFY(session.invertActiveLayerPixels());
    const QColor inverted = session.activeLayer()->image.pixelColor(0, 0);
    QCOMPARE(inverted.alpha(), 128);
    QVERIFY(inverted.red() > 230 && inverted.blue() > 190);
    session.undo();
    QCOMPARE(session.activeLayer()->image.convertToFormat(QImage::Format_RGBA8888).pixel(0, 0), before);
}

void EditorSessionTests::pixelEditsRespectDocumentSelection()
{
    EditorSession session;
    QImage image(4, 2, QImage::Format_RGBA8888); image.fill(QColor(10, 20, 30, 255));
    QVERIFY(session.insertImage(image, QStringLiteral("Pixels")));
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 2, 2)));
    QVERIFY(session.invertActiveLayerPixels());
    const QImage result = session.activeLayer()->image;
    QVERIFY(result.pixelColor(0, 0).red() > 240);
    QCOMPARE(result.pixelColor(3, 0).red(), 10);
    session.deselect();
    QVERIFY(session.invertActiveLayerPixels());
    QVERIFY(session.activeLayer()->image.pixelColor(3, 0).red() > 240);
}

void EditorSessionTests::brushPaintsAndErasesAsSingleUndoSteps()
{
    EditorSession session; session.createDocument(40, 30, true);
    const int before = session.history().undoCount();
    QVERIFY(session.beginBrushStroke(QPointF(10, 10), Qt::red, 8, 1, 1, false));
    session.continueBrushStroke(QPointF(20, 10));
    QVERIFY(session.endBrushStroke());
    QCOMPARE(session.history().undoCount(), before + 1);
    QVERIFY(!session.activeLayer()->image.isNull());
    QVERIFY(session.activeLayer()->image.pixelColor(10, 10).alpha() > 0);
    QVERIFY(session.activeLayer()->image.pixelColor(15, 10).red() > 200);
    session.undo();
    QVERIFY(session.activeLayer()->image.isNull());
    session.redo();
    const int alpha = session.activeLayer()->image.pixelColor(15, 10).alpha();
    QVERIFY(session.beginBrushStroke(QPointF(15, 10), Qt::black, 8, 1, 1, true));
    QVERIFY(session.endBrushStroke());
    QVERIFY(session.activeLayer()->image.pixelColor(15, 10).alpha() < alpha);
}

void EditorSessionTests::brushRespectsSelection()
{
    EditorSession session; session.createDocument(20, 10, true);
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 10, 10)));
    QVERIFY(session.beginBrushStroke(QPointF(2, 5), Qt::red, 8));
    session.continueBrushStroke(QPointF(18, 5));
    QVERIFY(session.endBrushStroke());
    QVERIFY(session.activeLayer()->image.pixelColor(5, 5).alpha() > 0);
    QCOMPARE(session.activeLayer()->image.pixelColor(15, 5).alpha(), 0);
}

void EditorSessionTests::layerMasksAddToggleDeleteAndUndo()
{
    EditorSession session;
    QImage image(2, 2, QImage::Format_RGBA8888); image.fill(Qt::red);
    QVERIFY(session.insertImage(image, QStringLiteral("Red")));
    QVERIFY(session.addLayerMask(false));
    QVERIFY(session.isMaskSelected());
    QCOMPARE(session.activeLayer()->mask.size(), QSize(1, 1));
    QCOMPARE(qGray(session.activeLayer()->mask.pixel(0, 0)), 0);
    QCOMPARE(LayerRenderer::flattened(*session.document()).pixelColor(0, 0).alpha(), 0);
    QVERIFY(session.toggleLayerMask());
    QCOMPARE(LayerRenderer::flattened(*session.document()).pixelColor(0, 0).alpha(), 255);
    QVERIFY(session.deleteLayerMask());
    QVERIFY(session.activeLayer()->mask.isNull());
    session.undo(); QVERIFY(!session.activeLayer()->mask.isNull() && !session.activeLayer()->maskEnabled);
    session.undo(); QVERIFY(session.activeLayer()->maskEnabled);
    session.undo(); QVERIFY(session.activeLayer()->mask.isNull());
}

void EditorSessionTests::selectionCreatesInverseLayerMask()
{
    EditorSession session;
    QImage image(4, 2, QImage::Format_RGBA8888); image.fill(Qt::red);
    QVERIFY(session.insertImage(image, QStringLiteral("Red")));
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 2, 2)));
    QVERIFY(session.addLayerMask(true, true));
    QVERIFY(!session.document()->selection.has_value());
    QCOMPARE(qGray(session.activeLayer()->mask.pixel(0, 0)), 0);
    QCOMPARE(qGray(session.activeLayer()->mask.pixel(3, 0)), 255);
    QVERIFY(session.invertLayerMask());
    QCOMPARE(qGray(session.activeLayer()->mask.pixel(0, 0)), 255);
    QVERIFY(session.loadMaskAsSelection());
    QVERIFY(session.document()->selection.has_value());
    QCOMPARE(qGray(session.document()->selection->pixel(0, 0)), 0);
    QCOMPARE(qGray(session.document()->selection->pixel(3, 0)), 255);
    QVERIFY(session.loadLayerAsSelection());
    QCOMPARE(qGray(session.document()->selection->pixel(0, 0)), 255);
    QCOMPARE(qGray(session.document()->selection->pixel(3, 0)), 255);
}

void EditorSessionTests::canvasResizeUsesNinePointAnchorAndIsUndoable()
{
    EditorSession session; session.createDocument(64, 32, true);
    QVERIFY(session.resizeCanvas(QSize(69, 37), 4));
    QCOMPARE(session.document()->canvasSize, QSize(69, 37));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(2, 2));
    QCOMPARE(session.history().undoName(), QStringLiteral("Canvas Size"));
    session.undo();
    QCOMPARE(session.document()->canvasSize, QSize(64, 32));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(0, 0));

    QVERIFY(session.resizeCanvas(QSize(59, 27), 8));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(-5, -5));
}

void EditorSessionTests::canvasResizeCanFillOnlyTheExtension()
{
    EditorSession session; session.createDocument(4, 4, true);
    QVERIFY(session.resizeCanvas(QSize(8, 4), 4, QColor(Qt::red)));
    QCOMPARE(session.document()->layers.size(), 2);
    const QImage result = LayerRenderer::flattened(*session.document());
    QCOMPARE(result.pixelColor(0, 1), QColor(Qt::red));
    QCOMPARE(result.pixelColor(7, 1), QColor(Qt::red));
    QCOMPARE(result.pixelColor(3, 1).alpha(), 0);
}

void EditorSessionTests::cropTranslatesContentAndSelection()
{
    EditorSession session; session.createDocument(100, 50, true);
    QVERIFY(session.setRectangularSelection(QRect(10, 6, 12, 8)));
    QVERIFY(session.crop(QRect(8, 4, 32, 16)));
    QCOMPARE(session.document()->canvasSize, QSize(32, 16));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(-8, -4));
    QVERIFY(session.document()->selection.has_value());
    QCOMPARE(qGray(session.document()->selection->pixel(2, 2)), 255);
    QCOMPARE(qGray(session.document()->selection->pixel(0, 0)), 0);
    session.undo();
    QCOMPARE(session.document()->canvasSize, QSize(100, 50));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(0, 0));
}

void EditorSessionTests::imageResizeScalesPixelsTransformsAndResolution()
{
    EditorSession session;
    QImage image(4, 2, QImage::Format_RGBA8888); image.fill(Qt::blue);
    QVERIFY(session.insertImage(image, QStringLiteral("Blue")));
    QVERIFY(session.resizeImage(QSize(8, 6), 300, Sampling::Nearest));
    QCOMPARE(session.document()->canvasSize, QSize(8, 6));
    QCOMPARE(session.document()->resolution, 300.0);
    QCOMPARE(session.activeLayer()->image.size(), QSize(8, 6));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(0, 0));
    QCOMPARE(session.activeLayer()->transform.size, QSizeF(8, 6));
    QCOMPARE(session.activeLayer()->transform.sampling, Sampling::Nearest);
    QCOMPARE(session.history().undoName(), QStringLiteral("Image Size"));
    session.undo();
    QCOMPARE(session.document()->canvasSize, QSize(4, 2));
    QCOMPARE(session.activeLayer()->image.size(), QSize(4, 2));
}

void EditorSessionTests::layerAndCanvasFlipsMirrorPlacementAndSelection()
{
    EditorSession session;
    QImage image(2, 2, QImage::Format_RGBA8888); image.fill(Qt::red);
    QVERIFY(session.insertImage(image, QStringLiteral("Red")));
    session.document()->canvasSize = QSize(10, 8);
    session.activeLayer()->transform.origin = QPointF(1, 2);
    session.activeLayer()->transform.rotation = 30;
    QVERIFY(session.flipLayers(true));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(1, 2));
    QVERIFY(session.activeLayer()->transform.flipX);
    QCOMPARE(session.activeLayer()->transform.rotation, -30.0);
    QVERIFY(session.setRectangularSelection(QRect(1, 1, 2, 2)));
    QVERIFY(session.flipCanvas(true));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(7, 2));
    QVERIFY(!session.activeLayer()->transform.flipX);
    QCOMPARE(qGray(session.document()->selection->pixel(7, 1)), 255);
    QCOMPARE(session.history().undoName(), QStringLiteral("Flip Canvas Horizontal"));
}

void EditorSessionTests::mergeDownAndFlattenBakeTheComposite()
{
    EditorSession session;
    QImage red(4, 4, QImage::Format_RGBA8888); red.fill(Qt::red);
    QImage blue(2, 2, QImage::Format_RGBA8888); blue.fill(Qt::blue);
    QVERIFY(session.insertImage(red, QStringLiteral("Red")));
    QVERIFY(session.insertImage(blue, QStringLiteral("Blue"), QPointF(2, 2)));
    QVERIFY(session.canMergeLayers()); QCOMPARE(session.mergeTitle(), QStringLiteral("Merge Down"));
    QVERIFY(session.mergeLayers());
    QCOMPARE(session.document()->layers.size(), 1);
    QCOMPARE(session.activeLayer()->name, QStringLiteral("Red"));
    const QImage merged = LayerRenderer::flattened(*session.document());
    QCOMPARE(merged.pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(merged.pixelColor(2, 2), QColor(Qt::blue));
    QCOMPARE(session.history().undoName(), QStringLiteral("Merge Down"));
    session.undo(); QCOMPARE(session.document()->layers.size(), 2);
    session.selectLayers({session.document()->layers.at(0).id, session.document()->layers.at(1).id}, session.document()->layers.at(1).id);
    QVERIFY(session.canMergeLayers()); QCOMPARE(session.mergeTitle(), QStringLiteral("Merge Layers"));
    session.selectLayer(session.document()->layers.at(1).id);
    QVERIFY(session.flattenImage());
    QCOMPARE(session.document()->layers.size(), 1);
    QCOMPARE(session.activeLayer()->image.size(), QSize(4, 4));
    QCOMPARE(session.history().undoName(), QStringLiteral("Flatten Image"));
}

void EditorSessionTests::selectionExpansionContractionAndMoveAreUndoable()
{
    EditorSession session; session.createDocument(100, 100, true);
    QVERIFY(session.setRectangularSelection(QRect(40, 40, 20, 20)));
    QVERIFY(session.expandSelection(5));
    QCOMPARE(qGray(session.document()->selection->pixel(35, 50)), 255);
    QCOMPARE(qGray(session.document()->selection->pixel(34, 50)), 0);
    QCOMPARE(session.history().undoName(), QStringLiteral("Expand Selection"));
    QVERIFY(session.contractSelection(8));
    QCOMPARE(qGray(session.document()->selection->pixel(43, 50)), 255);
    QCOMPARE(qGray(session.document()->selection->pixel(42, 50)), 0);
    QVERIFY(session.moveSelection(QPoint(10, -5)));
    QCOMPARE(qGray(session.document()->selection->pixel(53, 45)), 255);
    QCOMPARE(session.history().undoName(), QStringLiteral("Move Selection"));
    session.undo(); QCOMPARE(qGray(session.document()->selection->pixel(43, 50)), 255);
}

void EditorSessionTests::fillAndClearRespectSelectionAndMaskTarget()
{
    EditorSession session; session.createDocument(10, 6, true);
    QVERIFY(session.setRectangularSelection(QRect(2, 1, 4, 3)));
    QVERIFY(session.fillSelection(Qt::red));
    QCOMPARE(session.activeLayer()->image.pixelColor(3, 2), QColor(Qt::red));
    QVERIFY(session.activeLayer()->image.pixelColor(0, 0).alpha() < 20);
    QVERIFY(session.clearSelectedPixels());
    QCOMPARE(session.activeLayer()->image.pixelColor(3, 2).alpha(), 0);
    session.deselect(); QVERIFY(session.fillSelection(Qt::blue));
    QVERIFY(session.addLayerMask(true, false)); session.selectMaskTarget(true);
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 5, 6)));
    QVERIFY(session.fillSelection(Qt::black));
    const QImage result = LayerRenderer::flattened(*session.document());
    QCOMPARE(result.pixelColor(2, 2).alpha(), 0);
    QCOMPARE(result.pixelColor(8, 2), QColor(Qt::blue));
    QVERIFY(session.clearSelectedPixels());
    QCOMPARE(LayerRenderer::flattened(*session.document()).pixelColor(2, 2), QColor(Qt::blue));
}

void EditorSessionTests::ellipseAndPolygonSelectionsCombine()
{
    EditorSession session; session.createDocument(30, 20);
    QVERIFY(session.setEllipticalSelection(QRect(2, 2, 10, 10)));
    QVERIFY(qGray(session.document()->selection->pixel(7, 7)) > 240);
    QCOMPARE(qGray(session.document()->selection->pixel(2, 2)), 0);
    const QPolygonF triangle{QPointF(15, 2), QPointF(27, 10), QPointF(15, 18)};
    QVERIFY(session.setPolygonSelection(triangle, SelectionMode::Add));
    QVERIFY(qGray(session.document()->selection->pixel(18, 10)) > 240);
    QVERIFY(session.setPolygonSelection(triangle, SelectionMode::Subtract));
    QCOMPARE(qGray(session.document()->selection->pixel(18, 10)), 0);
}

void EditorSessionTests::clippingMasksPreserveBaseAlphaAndSupportHiddenSources()
{
    EditorSession session;
    QImage base(4, 1, QImage::Format_RGBA8888); base.fill(Qt::transparent);
    const int alpha[] = {255, 128, 32, 0};
    for (int x = 0; x < 4; ++x) base.setPixelColor(x, 0, QColor(255, 0, 0, alpha[x]));
    QImage white(4, 1, QImage::Format_RGBA8888); white.fill(Qt::white);
    QVERIFY(session.insertImage(base, QStringLiteral("Base")));
    const QUuid baseId = session.activeLayer()->id;
    QVERIFY(session.insertImage(white, QStringLiteral("Clipped")));
    const QUuid clippedId = session.activeLayer()->id;
    QVERIFY(session.toggleClippingMask(clippedId));
    QCOMPARE(session.activeLayer()->maskSourceId, std::optional<QUuid>(baseId));
    const QImage result = LayerRenderer::flattened(*session.document());
    for (int x = 0; x < 4; ++x) QCOMPARE(result.pixelColor(x, 0).alpha(), alpha[x]);
    QVERIFY(session.toggleClippingMask(clippedId));
    QVERIFY(!session.activeLayer()->maskSourceId.has_value());

    session.selectLayer(baseId);
    QVERIFY(session.linkMask(clippedId, baseId));
    session.document()->layers[1].visible = false;
    const QImage hiddenSource = LayerRenderer::flattened(*session.document());
    for (int x = 0; x < 4; ++x) QCOMPARE(hiddenSource.pixelColor(x, 0).alpha(), alpha[x]);
    QVERIFY(!session.canLinkMask(baseId, clippedId));
    session.selectLayer(clippedId); session.deleteActiveLayer();
    QVERIFY(!session.activeLayer()->maskSourceId.has_value());
}

void EditorSessionTests::copyAndPastePreserveSelectedPixelPosition()
{
    EditorSession session;
    QImage image(10, 8, QImage::Format_RGBA8888); image.fill(Qt::red);
    QVERIFY(session.insertImage(image, QStringLiteral("Red")));
    QVERIFY(session.setRectangularSelection(QRect(3, 2, 4, 3)));
    const auto copied = session.copiedPixels(); QVERIFY(copied.has_value());
    QCOMPARE(copied->first.size(), QSize(4, 3)); QCOMPARE(copied->second, QPoint(3, 2));
    QVERIFY(session.insertPixelLayer(copied->first, copied->second, QStringLiteral("Layer 1"), QStringLiteral("Paste")));
    QCOMPARE(session.document()->layers.size(), 2);
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(3, 2));
    QVERIFY(!session.document()->selection.has_value());
    QCOMPARE(session.history().undoName(), QStringLiteral("Paste"));
}

void EditorSessionTests::levelsExposureGradientMapAndGrainPreserveAlphaAndUndo()
{
    QImage pixels(4, 1, QImage::Format_RGBA8888); pixels.fill(Qt::transparent);
    pixels.setPixelColor(0, 0, QColor(64, 64, 64)); pixels.setPixelColor(1, 0, QColor(128, 128, 128));
    pixels.setPixelColor(2, 0, QColor(64, 32, 0, 128));
    LevelsSettings levels; levels.ranges[0] = LevelRange{64, 1, 128, 0, 255};
    const QImage leveled = RasterOperations::levels(pixels, levels);
    QCOMPARE(leveled.pixelColor(0, 0).red(), 0); QCOMPARE(leveled.pixelColor(1, 0).red(), 255);
    QCOMPARE(leveled.pixelColor(2, 0).alpha(), 128);
    const QImage exposed = RasterOperations::exposure(pixels, 1, 0, 1);
    QVERIFY(std::abs(exposed.pixelColor(1, 0).red() - 176) <= 2);
    QCOMPARE(exposed.pixelColor(2, 0).alpha(), 128);
    const QImage mapped = RasterOperations::gradientMap(pixels, Qt::red, Qt::blue, false);
    QCOMPARE(mapped.pixelColor(0, 0).alpha(), 255); QVERIFY(mapped.pixelColor(0, 0).red() > mapped.pixelColor(0, 0).blue());

    EditorSession session; QVERIFY(session.insertImage(pixels, QStringLiteral("Adjust")));
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 1, 1)));
    QVERIFY(session.applyExposure(1, 0, 1));
    QCOMPARE(session.activeLayer()->image.pixelColor(1, 0).red(), 128);
    QCOMPARE(session.history().undoName(), QStringLiteral("Exposure"));
    session.undo(); QCOMPARE(session.activeLayer()->image.pixelColor(0, 0).red(), 64);
    session.deselect(); QVERIFY(session.applyGrain(60, 2, 40, 7));
    QCOMPARE(session.activeLayer()->image.pixelColor(3, 0).alpha(), 0);
}

void EditorSessionTests::gaussianAndMotionBlurMatchDirectionAndSelection()
{
    QImage half(40, 20, QImage::Format_RGBA8888); half.fill(Qt::transparent);
    QPainter painter(&half); painter.fillRect(QRect(0, 0, 20, 20), Qt::white); painter.end();
    const QImage blurred = RasterOperations::gaussianBlur(half, 3);
    QCOMPARE(blurred.pixelColor(0, 10).alpha(), 255);
    QVERIFY(blurred.pixelColor(20, 10).alpha() > 20 && blurred.pixelColor(20, 10).alpha() < 235);
    QCOMPARE(blurred.pixelColor(38, 10).alpha(), 0);

    QImage dot(41, 41, QImage::Format_RGBA8888); dot.fill(Qt::transparent); dot.setPixelColor(20, 20, Qt::white);
    const QImage horizontal = RasterOperations::motionBlur(dot, 0, 16);
    QVERIFY(horizontal.pixelColor(24, 20).alpha() > 0); QVERIFY(horizontal.pixelColor(16, 20).alpha() > 0);
    QCOMPARE(horizontal.pixelColor(20, 24).alpha(), 0);
    const QImage vertical = RasterOperations::motionBlur(dot, 90, 16);
    QVERIFY(vertical.pixelColor(20, 24).alpha() > 0); QCOMPARE(vertical.pixelColor(24, 20).alpha(), 0);
    const QImage diagonal = RasterOperations::motionBlur(dot, 45, 16);
    QVERIFY(diagonal.pixelColor(23, 17).alpha() > 0); QVERIFY(diagonal.pixelColor(17, 23).alpha() > 0);
    QCOMPARE(diagonal.pixelColor(17, 17).alpha(), 0);

    EditorSession session; QVERIFY(session.insertImage(half, QStringLiteral("Half")));
    QVERIFY(session.setRectangularSelection(QRect(18, 0, 4, 20)));
    QVERIFY(session.applyGaussianBlur(3));
    QCOMPARE(session.activeLayer()->image.pixelColor(10, 10).alpha(), 255);
    QCOMPARE(session.history().undoName(), QStringLiteral("Gaussian Blur"));
}

void EditorSessionTests::gradientAndShapeToolsCreateUndoablePixels()
{
    EditorSession session; session.createDocument(20, 10, true);
    QVERIFY(session.applyGradient(QPointF(0, 5), QPointF(19, 5), Qt::red, Qt::blue));
    QVERIFY(session.activeLayer()->image.pixelColor(1, 5).red() > session.activeLayer()->image.pixelColor(1, 5).blue());
    QVERIFY(session.activeLayer()->image.pixelColor(18, 5).blue() > session.activeLayer()->image.pixelColor(18, 5).red());
    QCOMPARE(session.history().undoName(), QStringLiteral("Gradient"));
    const int count = session.document()->layers.size();
    QVERIFY(session.addShape(QRectF(3, 2, 8, 5), Qt::green, Qt::white, 2, true));
    QCOMPARE(session.document()->layers.size(), count + 1);
    QCOMPARE(session.activeLayer()->shape.value(QStringLiteral("kind")).toString(), QStringLiteral("Ellipse"));
    QCOMPARE(session.history().undoName(), QStringLiteral("Ellipse"));
    session.undo(); QCOMPARE(session.document()->layers.size(), count);
    session.redo(); QCOMPARE(session.activeLayer()->shape.value(QStringLiteral("kind")).toString(), QStringLiteral("Ellipse"));

    QVERIFY(session.addShape(QRectF(1, 1, 10, 8), Qt::red, Qt::transparent, 0, false, 3));
    QCOMPARE(session.activeLayer()->name, QStringLiteral("Rectangle 1"));
    QVERIFY(session.activeLayer()->image.pixelColor(0, 0).alpha() < 20);
    QVERIFY(session.activeLayer()->image.pixelColor(5, 0).alpha() > 200);
    session.activeLayer()->transform.size = QSizeF(20, 16);
    session.redrawSelectedShapes();
    QCOMPARE(session.activeLayer()->image.size(), QSize(20, 16));
    QVERIFY(session.activeLayer()->image.pixelColor(0, 0).alpha() < 20);
}

void EditorSessionTests::softBrushOpacityCapAndMaskPaintingMatchBrushSettings()
{
    EditorSession soft; soft.createDocument(80, 80, true);
    QVERIFY(soft.beginBrushStroke(QPointF(40, 40), Qt::red, 40, 0, 1)); QVERIFY(soft.endBrushStroke());
    const QImage feather = soft.activeLayer()->image;
    QVERIFY(feather.pixelColor(40, 40).alpha() > 230);
    QVERIFY(feather.pixelColor(50, 40).alpha() > 95 && feather.pixelColor(50, 40).alpha() < 140);
    QVERIFY(feather.pixelColor(57, 40).alpha() < 40); QCOMPARE(feather.pixelColor(64, 40).alpha(), 0);

    EditorSession capped; capped.createDocument(200, 80, true);
    QVERIFY(capped.beginBrushStroke(QPointF(20, 40), Qt::red, 20, 1, .5));
    for (int x : {180, 20, 180, 20, 100}) capped.continueBrushStroke(QPointF(x, 40));
    QVERIFY(capped.endBrushStroke());
    QVERIFY(std::abs(capped.activeLayer()->image.pixelColor(100, 40).alpha() - 128) <= 1);

    EditorSession mask;
    QImage red(80, 80, QImage::Format_RGBA8888); red.fill(Qt::red); QVERIFY(mask.insertImage(red, QStringLiteral("Red")));
    QVERIFY(mask.addLayerMask(true, false)); mask.selectMaskTarget(true);
    QVERIFY(mask.beginBrushStroke(QPointF(40, 40), Qt::black, 20, 1, .5, false)); QVERIFY(mask.endBrushStroke());
    QVERIFY(std::abs(LayerRenderer::flattened(*mask.document()).pixelColor(40, 40).alpha() - 128) <= 1);
    QCOMPARE(mask.history().undoName(), QStringLiteral("Paint Mask"));
    QVERIFY(mask.beginBrushStroke(QPointF(40, 40), Qt::black, 20, 1, 1, true)); QVERIFY(mask.endBrushStroke());
    QCOMPARE(LayerRenderer::flattened(*mask.document()).pixelColor(40, 40).alpha(), 255);
}

void EditorSessionTests::contentAwareFillReconstructsSelectedObjectAndUndoes()
{
    QImage image(64, 48, QImage::Format_RGBA8888); image.fill(QColor(51, 153, 204));
    QPainter painter(&image); painter.fillRect(QRect(20, 16, 12, 10), Qt::red); painter.end();
    EditorSession session; QVERIFY(session.insertImage(image, QStringLiteral("Object")));
    QVERIFY(session.setRectangularSelection(QRect(20, 16, 12, 10)));
    const int count = session.history().undoCount();
    QVERIFY(session.contentAwareFill());
    QCOMPARE(session.history().undoCount(), count + 1);
    QCOMPARE(session.history().undoName(), QStringLiteral("Content-Aware Fill"));
    const QColor filled = session.activeLayer()->image.pixelColor(25, 20);
    QVERIFY(std::abs(filled.red() - 51) <= 2 && std::abs(filled.green() - 153) <= 2 && std::abs(filled.blue() - 204) <= 2);
    session.undo(); QCOMPARE(session.activeLayer()->image.pixelColor(25, 20), QColor(Qt::red));
    session.selectAll(); QVERIFY(!session.contentAwareFill());
}

void EditorSessionTests::cloneStampKeepsAlignedSourceAndUsesBrushCoverage()
{
    QImage image(40, 20, QImage::Format_RGBA8888); image.fill(Qt::blue);
    QPainter painter(&image); painter.fillRect(QRect(0, 0, 20, 20), Qt::red); painter.end();
    EditorSession session; QVERIFY(session.insertImage(image, QStringLiteral("Two Colors")));
    session.setCloneSource(QPointF(5, 10));
    QVERIFY(session.beginCloneStroke(QPointF(25, 10), 8, 1, 1));
    session.continueBrushStroke(QPointF(30, 10)); QVERIFY(session.endBrushStroke());
    QVERIFY(session.activeLayer()->image.pixelColor(25, 10).red() > 240);
    QVERIFY(session.activeLayer()->image.pixelColor(30, 10).red() > 240);
    QCOMPARE(session.activeLayer()->image.pixelColor(38, 10), QColor(Qt::blue));
    QCOMPARE(session.history().undoName(), QStringLiteral("Clone Stamp"));
    session.undo(); QCOMPARE(session.activeLayer()->image.pixelColor(25, 10), QColor(Qt::blue));
    QVERIFY(session.beginCloneStroke(QPointF(28, 5), 4, 1, 1)); QVERIFY(session.endBrushStroke());
    QVERIFY(session.activeLayer()->image.pixelColor(28, 5).red() > 240);

    EditorSession allLayers;QImage lower(30,10,QImage::Format_RGBA8888);lower.fill(Qt::red);QVERIFY(allLayers.insertImage(lower,QStringLiteral("Lower")));allLayers.addBlankLayer();allLayers.setCloneSource(QPointF(4,5));QVERIFY(allLayers.beginCloneStroke(QPointF(20,5),4,1,1,true,true));QVERIFY(allLayers.endBrushStroke());QVERIFY(allLayers.activeLayer()->image.pixelColor(20,5).red()>240);

    QImage ramp(24,8,QImage::Format_RGBA8888);for(int y=0;y<ramp.height();++y)for(int x=0;x<ramp.width();++x)ramp.setPixelColor(x,y,QColor(x*10,x*10,x*10));EditorSession unaligned;QVERIFY(unaligned.insertImage(ramp,QStringLiteral("Ramp")));unaligned.setCloneSource(QPointF(2,4));QVERIFY(unaligned.beginCloneStroke(QPointF(10,4),2,1,1,false,false));QVERIFY(unaligned.endBrushStroke());QVERIFY(unaligned.beginCloneStroke(QPointF(18,4),2,1,1,false,false));QVERIFY(unaligned.endBrushStroke());QVERIFY(std::abs(unaligned.activeLayer()->image.pixelColor(18,4).red()-20)<=2);
}

void EditorSessionTests::spotHealingUsesBrushCoverageAndUndoes()
{
    QImage image(64, 48, QImage::Format_RGBA8888); image.fill(QColor(51, 153, 204));
    QPainter painter(&image); painter.fillRect(QRect(26, 20, 10, 8), Qt::red); painter.end();
    EditorSession session; QVERIFY(session.insertImage(image, QStringLiteral("Spot")));
    QVERIFY(session.beginHealingStroke(QPointF(31, 24), 14, 1, 1, 0, 7));
    QVERIFY(session.endBrushStroke());
    const QColor healed = session.activeLayer()->image.pixelColor(31, 24);
    QVERIFY(healed.red() < 200 && healed.blue() > 100);
    QCOMPARE(session.history().undoName(), QStringLiteral("Spot Healing"));
    session.undo(); QCOMPARE(session.activeLayer()->image.pixelColor(31, 24), QColor(Qt::red));
}

void EditorSessionTests::blurBrushSoftensOnlyItsStrokeCoverage()
{
    QImage image(40, 20, QImage::Format_RGBA8888); image.fill(Qt::blue);
    QPainter painter(&image); painter.fillRect(QRect(0, 0, 20, 20), Qt::red); painter.end();
    EditorSession session; QVERIFY(session.insertImage(image, QStringLiteral("Edge")));
    QVERIFY(session.beginBlurStroke(QPointF(20, 10), 16, 1, 1)); QVERIFY(session.endBrushStroke());
    const QColor edge = session.activeLayer()->image.pixelColor(19, 10);
    QVERIFY(edge.red() > 0 && edge.blue() > 0);
    QCOMPARE(session.activeLayer()->image.pixelColor(2, 2), QColor(Qt::red));
    QCOMPARE(session.activeLayer()->image.pixelColor(37, 2), QColor(Qt::blue));
    QCOMPARE(session.history().undoName(), QStringLiteral("Blur"));
    session.undo(); QCOMPARE(session.activeLayer()->image.pixelColor(19, 10), QColor(Qt::red));
}

void EditorSessionTests::recoveredDocumentsStartDirtyWhileOpenedDocumentsStartSaved()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(10, 10);
    EditorSession opened; opened.setDocument(document, true); QVERIFY(!opened.isModified());
    EditorSession recovered; recovered.setDocument(std::make_shared<Document>(*document), false); QVERIFY(recovered.isModified());
    recovered.markSaved(); QVERIFY(!recovered.isModified());
}

void EditorSessionTests::hueSaturationMatchesRangesAlphaSelectionAndUndo()
{
    QImage colors(40, 20, QImage::Format_RGBA8888); colors.fill(Qt::transparent);
    QPainter painter(&colors); painter.fillRect(QRect(0, 0, 20, 20), Qt::red); painter.fillRect(QRect(20, 0, 20, 20), Qt::blue); painter.end();
    EditorSession session; QVERIFY(session.insertImage(colors, QStringLiteral("Colors")));
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 10, 20)));
    const int count = session.history().undoCount();
    QVERIFY(session.applyHueSaturation(HueSaturationSettings(120)));
    QCOMPARE(session.history().undoCount(), count + 1); QCOMPARE(session.history().undoName(), QStringLiteral("Hue/Saturation"));
    const QColor green = session.activeLayer()->image.pixelColor(5, 5), untouched = session.activeLayer()->image.pixelColor(15, 5);
    QVERIFY(green.green() > 245 && green.red() < 10 && green.blue() < 10); QCOMPARE(untouched, QColor(Qt::red));
    session.undo(); QCOMPARE(session.activeLayer()->image.pixelColor(5, 5), QColor(Qt::red));
    session.deselect();

    HueSaturationSettings targeted(60, 0, 0, false, ColorRange::Reds);
    targeted.adjustments[size_t(ColorRange::Blues)].saturation = -100;
    QVERIFY(session.applyHueSaturation(targeted));
    const QColor yellow = session.activeLayer()->image.pixelColor(5, 5), gray = session.activeLayer()->image.pixelColor(30, 5);
    QVERIFY(yellow.red() > 245 && yellow.green() > 245 && yellow.blue() < 10);
    QVERIFY(std::abs(gray.red() - gray.green()) <= 1 && std::abs(gray.green() - gray.blue()) <= 1);

    QImage translucent(1, 1, QImage::Format_RGBA8888); translucent.fill(QColor(0, 0, 255, 128));
    const QImage colorized = RasterOperations::hueSaturation(translucent, HueSaturationSettings(240, 100, 0, true));
    QCOMPARE(colorized.pixelColor(0, 0).alpha(), 128); QVERIFY(colorized.pixelColor(0, 0).blue() > colorized.pixelColor(0, 0).red());
}

void EditorSessionTests::hueBandsWrapAndRejectCrossedHandles()
{
    const HueBand reds = HueBand::defaultFor(ColorRange::Reds);
    QCOMPARE(reds.weight(0), 1.0); QCOMPARE(reds.weight(345), 1.0); QCOMPARE(reds.weight(15), 1.0);
    QVERIFY(std::abs(reds.weight(330) - .5) < .001); QVERIFY(std::abs(reds.weight(30) - .5) < .001);
    QCOMPARE(reds.weight(315), 0.0); QCOMPARE(reds.weight(45), 0.0); QCOMPARE(reds.weight(180), 0.0);
    HueBand green = HueBand::defaultFor(ColorRange::Greens), original = green;
    QVERIFY(!green.setHandle(1, 200)); QCOMPARE(green, original);
    QVERIFY(green.setHandle(1, 110)); QCOMPARE(green.rangeStart, 110.0);
}

void EditorSessionTests::curvesUseShapePreservingChannelsSelectionAndUndo()
{
    CurvesSettings identity; QVERIFY(identity.isValid());
    QCOMPARE(identity.value(64, 0), 64.0); QCOMPARE(identity.value(192, 3), 192.0);
    CurvesSettings shaped; shaped.channels[0] = {{0, 0}, {64, 190}, {180, 200}, {255, 255}};
    for (int x = 0; x <= 255; ++x) QVERIFY(shaped.value(x, 0) >= 0 && shaped.value(x, 0) <= 255);

    QImage image(20, 10, QImage::Format_RGBA8888); image.fill(QColor(64, 128, 192, 128));
    EditorSession session; QVERIFY(session.insertImage(image, QStringLiteral("Curve"))); QVERIFY(session.setRectangularSelection(QRect(0, 0, 10, 10)));
    const QColor original = session.activeLayer()->image.pixelColor(15, 5);
    CurvesSettings white; white.channels[0] = {{0, 255}, {255, 255}};
    const int count = session.history().undoCount(); QVERIFY(session.applyCurves(white));
    QCOMPARE(session.history().undoCount(), count + 1); QCOMPARE(session.history().undoName(), QStringLiteral("Curves"));
    const QColor adjusted = session.activeLayer()->image.pixelColor(5, 5), untouched = session.activeLayer()->image.pixelColor(15, 5);
    QCOMPARE(adjusted.alpha(), 128); QVERIFY(adjusted.red() > 250 && adjusted.green() > 250 && adjusted.blue() > 250);
    QCOMPARE(untouched, original);
    session.undo(); QCOMPARE(session.activeLayer()->image.pixelColor(5, 5), original);

    CurvesSettings invalid; invalid.channels[0] = {{0, 0}, {128, 100}, {127, 200}, {255, 255}}; QVERIFY(!invalid.isValid()); QVERIFY(!session.applyCurves(invalid));
}

void EditorSessionTests::smudgeAndLiquifyWarpPixelsAsUndoableStrokes()
{
    QImage image(60, 30, QImage::Format_RGBA8888); image.fill(Qt::blue);
    QPainter painter(&image); painter.fillRect(QRect(0, 0, 30, 30), Qt::red); painter.end();
    EditorSession smudge; QVERIFY(smudge.insertImage(image, QStringLiteral("Edge")));
    QVERIFY(smudge.beginWarpStroke(QPointF(20, 15), 1, 14, .8, .8));
    smudge.continueBrushStroke(QPointF(40, 15)); QVERIFY(smudge.endBrushStroke());
    QVERIFY(smudge.activeLayer()->image.pixelColor(35, 15).red() > 40);
    QCOMPARE(smudge.history().undoName(), QStringLiteral("Smudge"));
    smudge.undo(); QCOMPARE(smudge.activeLayer()->image.pixelColor(35, 15), QColor(Qt::blue));

    EditorSession liquify; QVERIFY(liquify.insertImage(image, QStringLiteral("Edge")));
    QVERIFY(liquify.beginWarpStroke(QPointF(28, 15), 0, 18, .7, 1));
    liquify.continueBrushStroke(QPointF(40, 15)); QVERIFY(liquify.endBrushStroke());
    QVERIFY(liquify.activeLayer()->image != image.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    QCOMPARE(liquify.history().undoName(), QStringLiteral("Liquify"));
    liquify.undo(); QCOMPARE(liquify.activeLayer()->image.pixelColor(35, 15), QColor(Qt::blue));

    liquify.addLayerMask(); liquify.selectMaskTarget(true);
    QVERIFY(!liquify.beginWarpStroke(QPointF(20, 15), 0, 10, 1, 1));
}

void EditorSessionTests::liveAdjustmentLayersRenderRemainEditableAndUndo()
{
    QImage red(4, 4, QImage::Format_RGBA8888); red.fill(Qt::red);
    EditorSession session; QVERIFY(session.insertImage(red, QStringLiteral("Base")));
    QJsonObject hsv{{QStringLiteral("hue"), 120.0}, {QStringLiteral("saturation"), 0.0}, {QStringLiteral("lightness"), 0.0}, {QStringLiteral("colorize"), false}};
    QVERIFY(session.addAdjustment(QStringLiteral("Hue/Saturation"), hsv));
    QVERIFY(session.activeLayer()->image.isNull()); QCOMPARE(session.activeLayer()->adjustment.value(QStringLiteral("kind")).toString(), QStringLiteral("Hue/Saturation"));
    QColor rendered = LayerRenderer::flattened(*session.document()).pixelColor(1, 1);
    QVERIFY(rendered.green() > 245 && rendered.red() < 10);
    session.activeLayer()->opacity = 0; QCOMPARE(LayerRenderer::flattened(*session.document()).pixelColor(1, 1), QColor(Qt::red));
    session.activeLayer()->opacity = 1; session.activeLayer()->mask = QImage(1, 1, QImage::Format_Grayscale8); session.activeLayer()->mask.fill(0);
    QCOMPARE(LayerRenderer::flattened(*session.document()).pixelColor(1, 1), QColor(Qt::red));
    session.activeLayer()->mask.fill(255);
    QJsonObject changed = session.activeLayer()->adjustment; changed.insert(QStringLiteral("hue"), 240.0);
    QVERIFY(session.updateAdjustment(session.activeLayer()->id, changed));
    rendered = LayerRenderer::flattened(*session.document()).pixelColor(1, 1); QVERIFY(rendered.blue() > 245 && rendered.red() < 10);
    session.undo(); rendered = LayerRenderer::flattened(*session.document()).pixelColor(1, 1); QVERIFY(rendered.green() > 245);
    session.undo(); QCOMPARE(session.document()->layers.size(), 1);

    EditorSession clipped; QImage blue(4, 2, QImage::Format_RGBA8888); blue.fill(Qt::blue); QVERIFY(clipped.insertImage(blue, QStringLiteral("Background")));
    QImage green(4, 2, QImage::Format_RGBA8888); green.fill(Qt::transparent); QPainter paint(&green); paint.fillRect(QRect(0, 0, 2, 2), Qt::green); paint.end();
    QVERIFY(clipped.insertImage(green, QStringLiteral("Base")));
    QJsonArray ranges; ranges.append(QJsonObject{{QStringLiteral("black"), 0}, {QStringLiteral("gamma"), 1}, {QStringLiteral("white"), 255}, {QStringLiteral("outputBlack"), 0}, {QStringLiteral("outputWhite"), 0}});
    for (int i = 1; i < 4; ++i) ranges.append(QJsonObject{{QStringLiteral("black"), 0}, {QStringLiteral("gamma"), 1}, {QStringLiteral("white"), 255}, {QStringLiteral("outputBlack"), 0}, {QStringLiteral("outputWhite"), 255}});
    QVERIFY(clipped.addAdjustment(QStringLiteral("Levels"), QJsonObject{{QStringLiteral("levels"), QJsonObject{{QStringLiteral("ranges"), ranges}}}}));
    const QUuid adjustmentId = clipped.activeLayer()->id; QVERIFY(clipped.toggleClippingMask(adjustmentId));
    const QImage clippedResult = LayerRenderer::flattened(*clipped.document());
    QCOMPARE(clippedResult.pixelColor(0, 0), QColor(Qt::black)); QCOMPARE(clippedResult.pixelColor(3, 0), QColor(Qt::blue));

    QImage redBlue(2,1,QImage::Format_RGBA8888);redBlue.setPixelColor(0,0,Qt::red);redBlue.setPixelColor(1,0,Qt::blue);
    QJsonArray targeted;targeted.append(QStringLiteral("Reds"));targeted.append(QJsonObject{{QStringLiteral("hue"),60},{QStringLiteral("saturation"),0},{QStringLiteral("lightness"),0}});
    const QJsonObject rangeAware{
        {QStringLiteral("kind"),QStringLiteral("Hue/Saturation")},
        {QStringLiteral("hsvSettings"),QJsonObject{{QStringLiteral("range"),QStringLiteral("Reds")},
            {QStringLiteral("colorize"),false},{QStringLiteral("invertRange"),false},
            {QStringLiteral("adjustments"),targeted},{QStringLiteral("bands"),QJsonArray{}}}}};
    const QImage rangeResult=RasterOperations::adjustment(redBlue,rangeAware);QVERIFY(rangeResult.pixelColor(0,0).red()>245&&rangeResult.pixelColor(0,0).green()>245);QCOMPARE(rangeResult.pixelColor(1,0),QColor(Qt::blue));

    EditorSession grains; grains.createDocument(4, 4);
    QVERIFY(grains.addAdjustment(QStringLiteral("Grain")));
    const QJsonObject firstGrain = grains.activeLayer()->adjustment.value(QStringLiteral("grainSettings")).toObject();
    QVERIFY(firstGrain.contains(QStringLiteral("seed"))); QCOMPARE(firstGrain.value(QStringLiteral("amount")).toDouble(), 25.0);
    QVERIFY(grains.addAdjustment(QStringLiteral("Grain")));
    const QJsonObject secondGrain = grains.activeLayer()->adjustment.value(QStringLiteral("grainSettings")).toObject();
    QVERIFY(secondGrain.contains(QStringLiteral("seed")));
    QVERIFY(firstGrain.value(QStringLiteral("seed")).toDouble() != secondGrain.value(QStringLiteral("seed")).toDouble());
}

void EditorSessionTests::levelsHistogramWeightsAlphaSelectionAndAutomaticModes()
{
    QImage image(4,1,QImage::Format_RGBA8888);image.setPixelColor(0,0,QColor(10,20,30,255));image.setPixelColor(1,0,QColor(100,110,120,128));image.setPixelColor(2,0,QColor(200,210,220,255));image.setPixelColor(3,0,Qt::transparent);
    QImage coverage(4,1,QImage::Format_Grayscale8);coverage.fill(255);coverage.scanLine(0)[2]=0;
    const LevelsHistogram histogram=RasterOperations::levelsHistogram(image,coverage);
    QCOMPARE(histogram[1][10],1.0);QVERIFY(std::abs(histogram[1][100]-128/255.0)<.001);QCOMPARE(histogram[1][200],0.0);
    LevelsSettings contrast=RasterOperations::automaticLevels(histogram,0);QCOMPARE(contrast.ranges[0].black,10.0);QCOMPARE(contrast.ranges[0].white,120.0);
    LevelsSettings color=RasterOperations::automaticLevels(histogram,1);QCOMPARE(color.ranges[1].black,10.0);QCOMPARE(color.ranges[1].white,100.0);
    LevelsSettings neutral=RasterOperations::automaticLevels(histogram,2);QVERIFY(neutral.ranges[1].gamma>=.1&&neutral.ranges[1].gamma<=9.99);
    LevelsHistogram empty{};QCOMPARE(RasterOperations::automaticLevels(empty,0).ranges[0],LevelRange{});
    const QColor sampled(64,102,153);
    QImage pixel(1,1,QImage::Format_RGBA8888);pixel.fill(sampled);
    for(int mode=0;mode<3;++mode){const LevelsSettings calibrated=RasterOperations::sampledLevels(LevelsSettings{},sampled,mode);const QColor result=RasterOperations::levels(pixel,calibrated).pixelColor(0,0);const int target=mode==0?0:mode==1?128:255;QVERIFY(std::abs(result.red()-target)<=1);QVERIFY(std::abs(result.green()-target)<=1);QVERIFY(std::abs(result.blue()-target)<=1);QCOMPARE(calibrated.ranges[0],LevelRange{});}
    EditorSession sampler;sampler.createDocument(20,10);QImage samples(2,1,QImage::Format_RGBA8888);samples.setPixelColor(0,0,sampled);samples.setPixelColor(1,0,Qt::transparent);QVERIFY(sampler.insertImage(samples,QStringLiteral("Samples")));
    QCOMPARE(sampler.levelsSampleAt(QPointF(9.5,4.5)),std::optional<QColor>(sampled));QVERIFY(!sampler.levelsSampleAt(QPointF(10.5,4.5)).has_value());QVERIFY(!sampler.levelsSampleAt(QPointF(-1,0)).has_value());
}

void EditorSessionTests::independentAndFolderMasksUseDocumentPlacement()
{
    QImage red(4,1,QImage::Format_RGBA8888);red.fill(Qt::red);EditorSession session;QVERIFY(session.insertImage(red,QStringLiteral("Red")));
    session.activeLayer()->mask=QImage(4,1,QImage::Format_Grayscale8);uchar *mask=session.activeLayer()->mask.scanLine(0);mask[0]=0;mask[1]=mask[2]=mask[3]=255;
    LayerTransform moved=session.activeLayer()->transform;moved.origin.rx()+=1;session.activeLayer()->maskPlacement=moved;
    QImage rendered=LayerRenderer::flattened(*session.document());QCOMPARE(rendered.pixelColor(0,0).alpha(),255);QCOMPARE(rendered.pixelColor(1,0).alpha(),0);QCOMPARE(rendered.pixelColor(2,0).alpha(),255);
    QVERIFY(session.activeLayer()->maskLinked);QVERIFY(session.toggleMaskLink());QVERIFY(!session.activeLayer()->maskLinked);session.undo();QVERIFY(session.activeLayer()->maskLinked);
    const LayerTransform oldLayer=session.activeLayer()->transform;LayerTransform newLayer=oldLayer;newLayer.origin+=QPointF(9,4);const LayerTransform followed=moved.following(oldLayer,newLayer);QCOMPARE(followed.origin,moved.origin+QPointF(9,4));

    EditorSession folder;QVERIFY(folder.insertImage(red,QStringLiteral("Child")));folder.addGroup();const QUuid groupId=folder.activeLayer()->id;folder.selectLayer(folder.document()->layers.constFirst().id);QVERIFY(folder.placeLayer(folder.activeLayer()->id,groupId));
    folder.selectLayer(groupId);folder.activeLayer()->mask=QImage(4,1,QImage::Format_Grayscale8);uchar *folderMask=folder.activeLayer()->mask.scanLine(0);folderMask[0]=folderMask[1]=0;folderMask[2]=folderMask[3]=255;
    rendered=LayerRenderer::flattened(*folder.document());QCOMPARE(rendered.pixelColor(0,0).alpha(),0);QCOMPARE(rendered.pixelColor(3,0),QColor(Qt::red));
}

void EditorSessionTests::allBlendModesUseCorrectColorAndAlphaMath()
{
    const auto render=[](const QColor &bottom,const QColor &top,BlendMode mode,double opacity=1){EditorSession s;QImage a(1,1,QImage::Format_RGBA8888);a.fill(bottom);QImage b(1,1,QImage::Format_RGBA8888);b.fill(top);s.insertImage(a,QStringLiteral("Bottom"));s.insertImage(b,QStringLiteral("Top"));s.activeLayer()->blendMode=mode;s.activeLayer()->opacity=opacity;return LayerRenderer::flattened(*s.document()).pixelColor(0,0);};
    const std::array<QPair<BlendMode,int>,9> grayModes{{{BlendMode::Normal,204},{BlendMode::Multiply,82},{BlendMode::Screen,224},{BlendMode::Overlay,163},{BlendMode::Darken,102},{BlendMode::Lighten,204},{BlendMode::Difference,102},{BlendMode::ColorDodge,255},{BlendMode::ColorBurn,64}}};
    for(const auto &[mode,expected]:grayModes)QVERIFY(std::abs(render(QColor(102,102,102),QColor(204,204,204),mode).red()-expected)<=2);
    const QColor saturationResult=render(Qt::red,Qt::green,BlendMode::Saturation);QVERIFY(saturationResult.red()>245&&saturationResult.green()<10);
    const QColor colorResult=render(Qt::red,Qt::green,BlendMode::Color);QVERIFY(colorResult.green()>120&&colorResult.red()<10);
    const QColor luminosityResult=render(Qt::red,Qt::green,BlendMode::Luminosity);QVERIFY(luminosityResult.red()>245&&luminosityResult.green()>90);
    const QColor softDodge=render(QColor(102,102,102),QColor(204,204,204,128),BlendMode::ColorDodge);QVERIFY(softDodge.red()>170&&softDodge.red()<220);QCOMPARE(softDodge.alpha(),255);
}

void EditorSessionTests::floatingSelectionTransformsMergeCancelAndUndo()
{
    QImage image(20, 10, QImage::Format_RGBA8888); image.fill(Qt::blue);
    QPainter painter(&image); painter.fillRect(QRect(0, 0, 10, 10), Qt::red); painter.end();
    EditorSession session; QVERIFY(session.insertImage(image, QStringLiteral("Pixels")));
    QVERIFY(session.setRectangularSelection(QRect(2, 2, 4, 4)));
    const Document before = *session.document();
    const int undoCount = session.history().undoCount();
    QVERIFY(session.beginSelectionTransform());
    QVERIFY(session.hasFloatingSelection());
    QCOMPARE(session.document()->layers.size(), 2);
    QCOMPARE(session.activeLayer()->name, QStringLiteral("Floating Selection"));
    session.activeLayer()->transform.origin = QPointF(13, 3);
    QVERIFY(session.commitSelectionTransform());
    QVERIFY(!session.hasFloatingSelection());
    QCOMPARE(session.document()->layers.size(), 1);
    QCOMPARE(session.history().undoCount(), undoCount + 1);
    const QImage rendered = LayerRenderer::flattened(*session.document());
    QCOMPARE(rendered.pixelColor(3, 3).alpha(), 0);
    QVERIFY(rendered.pixelColor(14, 4).red() > 245 && rendered.pixelColor(14, 4).blue() < 10);
    QVERIFY(session.document()->selection.has_value());
    QCOMPARE(session.document()->selection->pixelColor(14, 4).value(), 255);
    session.undo();
    QCOMPARE(*session.document(), before);

    QVERIFY(session.beginSelectionTransform());
    session.activeLayer()->transform.origin += QPointF(5, 0);
    QVERIFY(session.cancelSelectionTransform());
    QCOMPARE(*session.document(), before);
    QCOMPARE(session.history().undoCount(), undoCount);
}

void EditorSessionTests::gradientModesOpacityTransparencyAndMasks()
{
    EditorSession radial; radial.createDocument(101, 101, true);
    QVERIFY(radial.applyGradient(QPointF(50.5, 50.5), QPointF(90.5, 50.5), Qt::black, Qt::white, true));
    QImage rendered = LayerRenderer::flattened(*radial.document());
    QVERIFY(rendered.pixelColor(50, 50).red() < 5);
    QVERIFY(std::abs(rendered.pixelColor(70, 50).red() - 128) < 8);
    QVERIFY(rendered.pixelColor(100, 50).red() > 250);
    QVERIFY(rendered.pixelColor(50, 70).red() > 120 && rendered.pixelColor(50, 70).red() < 136);

    QImage red(101, 1, QImage::Format_RGBA8888); red.fill(Qt::red);
    EditorSession transparent; QVERIFY(transparent.insertImage(red, QStringLiteral("Red")));
    QVERIFY(transparent.applyGradient(QPointF(.5, .5), QPointF(100.5, .5), Qt::black, Qt::white, false, true));
    rendered = LayerRenderer::flattened(*transparent.document());
    QVERIFY(rendered.pixelColor(0, 0).red() < 5);
    QVERIFY(rendered.pixelColor(50, 0).red() > 120 && rendered.pixelColor(50, 0).red() < 136);
    QVERIFY(rendered.pixelColor(100, 0).red() > 250);
    QCOMPARE(rendered.pixelColor(50, 0).alpha(), 255);

    EditorSession opacity; opacity.createDocument(101, 1, true);
    QVERIFY(opacity.applyGradient(QPointF(.5, .5), QPointF(100.5, .5), Qt::black, Qt::white, false, false, true, .5));
    rendered = LayerRenderer::flattened(*opacity.document());
    QVERIFY(rendered.pixelColor(0, 0).red() > 245 && std::abs(rendered.pixelColor(0, 0).alpha() - 128) < 3);
    QVERIFY(rendered.pixelColor(100, 0).red() < 5 && std::abs(rendered.pixelColor(100, 0).alpha() - 128) < 3);

    EditorSession mask; QImage black(101, 1, QImage::Format_RGBA8888); black.fill(Qt::black); QVERIFY(mask.insertImage(black, QStringLiteral("Black")));
    QVERIFY(mask.addLayerMask(true, false));
    QVERIFY(mask.applyGradient(QPointF(.5, .5), QPointF(100.5, .5), Qt::black, Qt::white));
    rendered = LayerRenderer::flattened(*mask.document());
    QVERIFY(rendered.pixelColor(0, 0).alpha() < 5);
    QVERIFY(std::abs(rendered.pixelColor(50, 0).alpha() - 128) < 8);
    QVERIFY(rendered.pixelColor(100, 0).alpha() > 250);
    QCOMPARE(mask.history().undoName(), QStringLiteral("Gradient Mask"));
}

void EditorSessionTests::multiLayerNumericTransformsScaleRotateAndUndo()
{
    EditorSession session; session.createDocument(200, 100);
    QImage image(10, 10, QImage::Format_RGBA8888); image.fill(Qt::red);
    QVERIFY(session.insertImage(image, QStringLiteral("First"))); const QUuid first = session.activeLayer()->id;
    session.activeLayer()->transform.origin = QPointF(10, 10);
    session.activeLayer()->mask = QImage(10, 10, QImage::Format_Grayscale8); session.activeLayer()->mask.fill(255);
    LayerTransform maskPlacement = session.activeLayer()->transform; maskPlacement.origin += QPointF(2, 1); session.activeLayer()->maskPlacement = maskPlacement;
    QVERIFY(session.insertImage(image, QStringLiteral("Second"))); const QUuid second = session.activeLayer()->id;
    session.activeLayer()->transform.origin = QPointF(30, 20);
    session.selectLayers({first, second}, second);
    QCOMPARE(session.selectedLayersBounds(), QRectF(10, 10, 30, 20));
    const Document before = *session.document(); const int count = session.history().undoCount();
    QVERIFY(session.transformSelectedLayers(QRectF(20, 30, 60, 40)));
    QCOMPARE(session.document()->layers[0].transform.origin, QPointF(20, 30));
    QCOMPARE(session.document()->layers[0].transform.size, QSizeF(20, 20));
    QCOMPARE(session.document()->layers[1].transform.origin, QPointF(60, 50));
    QCOMPARE(session.document()->layers[0].maskPlacement->origin, QPointF(24, 32));
    QCOMPARE(session.history().undoCount(), count + 1);
    session.undo(); QCOMPARE(*session.document(), before);
    session.selectLayers({first, second}, second);
    QVERIFY(session.transformSelectedLayers(QRectF(10, 10, 30, 20), 90));
    QCOMPARE(session.document()->layers[0].transform.origin, QPointF(25, 5));
    QCOMPARE(session.document()->layers[1].transform.origin, QPointF(15, 25));
    QCOMPARE(session.document()->layers[0].transform.rotation, 90.0);
}

void EditorSessionTests::noiseAndLensValidateSourceControlRangesAndUndo()
{
    QImage image(17, 17, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
        image.setPixelColor(x, y, QColor(x * 15, y * 15, (x + y) * 7, 255));

    EditorSession lens; QVERIFY(lens.insertImage(image, QStringLiteral("Pattern")));
    const Document lensBefore = *lens.document();
    QVERIFY(!lens.distortActiveLayer(100.01));
    QCOMPARE(*lens.document(), lensBefore);
    QVERIFY(lens.distortActiveLayer(100));
    QVERIFY(lens.activeLayer()->image != image);
    QCOMPARE(lens.history().undoName(), QStringLiteral("Lens Correction"));
    lens.undo(); QCOMPARE(*lens.document(), lensBefore);

    EditorSession noise; QVERIFY(noise.insertImage(image, QStringLiteral("Pattern")));
    const Document noiseBefore = *noise.document();
    QVERIFY(!noise.addNoiseToActiveLayer(400.01f, false, false, 123));
    QVERIFY(noise.addNoiseToActiveLayer(25, true, true, 123));
    QVERIFY(noise.activeLayer()->image != image);
    QCOMPARE(noise.history().undoName(), QStringLiteral("Add Noise"));
    noise.undo(); QCOMPARE(*noise.document(), noiseBefore);
}

void EditorSessionTests::freeCornerDistortionBakesPixelsMasksAndUndo()
{
    QImage image(10, 10, QImage::Format_RGBA8888); image.fill(Qt::red);
    EditorSession session; QVERIFY(session.insertImage(image, QStringLiteral("Square")));
    session.activeLayer()->mask = QImage(10, 10, QImage::Format_Grayscale8);
    session.activeLayer()->mask.fill(255);
    for (int y = 0; y < 10; ++y) memset(session.activeLayer()->mask.scanLine(y), 0, 5);
    const Document before = *session.document();
    const std::array<QPointF, 4> trapezoid{QPointF(0, 0), QPointF(20, 0), QPointF(15, 10), QPointF(5, 10)};
    QVERIFY(session.distortSelectedLayers(trapezoid));
    QCOMPARE(session.activeLayer()->transform.origin, QPointF(0, 0));
    QCOMPARE(session.activeLayer()->transform.rotation, 0.0);
    QVERIFY(!session.activeLayer()->transform.flipX && !session.activeLayer()->transform.flipY);
    QVERIFY(session.activeLayer()->image.width() >= 19);
    QCOMPARE(session.activeLayer()->mask.size(), session.activeLayer()->image.size());
    const QImage rendered = LayerRenderer::flattened(*session.document());
    QCOMPARE(rendered.pixelColor(1, 9).alpha(), 0);
    QVERIFY(rendered.pixelColor(12, 5).alpha() > 200);
    QCOMPARE(session.history().undoName(), QStringLiteral("Distort"));
    session.undo(); QCOMPARE(*session.document(), before);

    session.selectMaskTarget(true);
    std::array<QPointF, 4> maskShape{QPointF(0, 0), QPointF(10, 0), QPointF(10, 10), QPointF(0, 10)};
    maskShape[0] += QPointF(2, 1);
    QVERIFY(session.distortSelectedLayers(maskShape, true));
    QVERIFY(session.activeLayer()->maskPlacement.has_value());
    QCOMPARE(session.history().undoName(), QStringLiteral("Distort Layer Mask"));
    session.undo(); QCOMPARE(*session.document(), before);
}

void EditorSessionTests::brushUsesCentripetalCurvesAndFinalizesTail()
{
    EditorSession session; session.createDocument(64, 64, true);
    const QVector<QPointF> samples{QPointF(5, 50), QPointF(25, 5), QPointF(30, 55), QPointF(58, 10)};
    QVERIFY(session.beginBrushStroke(samples[0], Qt::red, 4, 1, 1));
    for (int i = 1; i < samples.size(); ++i) session.continueBrushStroke(samples[i]);
    QVERIFY(session.endBrushStroke());
    const QImage image = session.activeLayer()->image;
    const auto segmentDistance = [](const QPointF &point, const QPointF &a, const QPointF &b) {
        const QPointF delta = b - a; const double length = QPointF::dotProduct(delta, delta);
        const double u = length > 0 ? std::clamp(QPointF::dotProduct(point - a, delta) / length, 0.0, 1.0) : 0;
        return QLineF(point, a + delta * u).length();
    };
    bool curvedPixel = false;
    for (int y = 0; y < image.height() && !curvedPixel; ++y) for (int x = 0; x < image.width(); ++x) {
        if (image.pixelColor(x, y).alpha() < 128) continue;
        const QPointF point(x + .5, y + .5);
        double nearest = 1e9;
        for (int i = 1; i < samples.size(); ++i) nearest = std::min(nearest, segmentDistance(point, samples[i - 1], samples[i]));
        if (nearest > 2.5) { curvedPixel = true; break; }
    }
    QVERIFY(curvedPixel);
    QVERIFY(image.pixelColor(58, 10).alpha() > 0);
    QCOMPARE(session.history().undoName(), QStringLiteral("Brush Stroke"));
}

void EditorSessionTests::layerDropOperationsCarryFoldersAndCopyPixels()
{
    QImage pixel(2, 2, QImage::Format_RGBA8888); pixel.fill(Qt::red);
    EditorSession session; QVERIFY(session.insertImage(pixel, QStringLiteral("Bottom"))); const QUuid bottom = session.activeLayer()->id;
    session.addGroup(); const QUuid folder = session.activeLayer()->id;
    QVERIFY(session.insertImage(pixel, QStringLiteral("Child"))); const QUuid child = session.activeLayer()->id;
    QCOMPARE(session.activeLayer()->parentId, std::optional<QUuid>(folder));
    session.selectLayer(bottom); QVERIFY(session.insertImage(pixel, QStringLiteral("Outside")));
    const Document beforeMove = *session.document();
    QVERIFY(session.placeLayer(folder, std::nullopt, std::nullopt, true));
    QCOMPARE(session.document()->layers[0].id, folder);
    QCOMPARE(session.document()->layers[1].id, child);
    QCOMPARE(session.document()->layers[1].parentId, std::optional<QUuid>(folder));
    session.undo(); QCOMPARE(*session.document(), beforeMove);

    const int count = session.document()->layers.size();
    QVERIFY(session.duplicateLayer(bottom, folder));
    QCOMPARE(session.document()->layers.size(), count + 1);
    const Layer *copy = session.activeLayer(); QVERIFY(copy && copy->id != bottom);
    QCOMPARE(copy->parentId, std::optional<QUuid>(folder));
    QCOMPARE(copy->image, pixel.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    QCOMPARE(session.history().undoName(), QStringLiteral("Duplicate Layer"));
}

void EditorSessionTests::removeBackgroundComposesMasksSelectionAndUndo()
{
    EditorSession session; session.createDocument(8, 4, true);
    session.activeLayer()->image = QImage(8, 4, QImage::Format_RGBA8888_Premultiplied);
    session.activeLayer()->image.fill(Qt::red);
    session.activeLayer()->mask = QImage(8, 4, QImage::Format_Grayscale8);
    session.activeLayer()->mask.fill(128);
    QVERIFY(session.setRectangularSelection(QRect(0, 0, 4, 4)));
    const Document before = *session.document();
    QImage subject(8, 4, QImage::Format_Grayscale8); subject.fill(0);
    QString error;
    QVERIFY(session.removeBackground({}, subject, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(session.activeLayer()->maskEnabled);
    QVERIFY(session.isMaskSelected());
    QCOMPARE(session.history().undoName(), QStringLiteral("Remove Background"));
    for (int y = 0; y < 4; ++y) {
        const uchar *row = session.activeLayer()->mask.constScanLine(y);
        for (int x = 0; x < 4; ++x) QCOMPARE(int(row[x]), 0);
        for (int x = 4; x < 8; ++x) QCOMPARE(int(row[x]), 128);
    }
    session.undo(); QCOMPARE(*session.document(), before);

    QImage edge(9, 1, QImage::Format_Grayscale8);
    for (int x = 0; x < 9; ++x) edge.scanLine(0)[x] = uchar(x * 255 / 8);
    QImage guide(9, 1, QImage::Format_RGBA8888); guide.fill(Qt::white);
    SubjectRemovalSettings highContrast; highContrast.advanced = true; highContrast.matteContrast = 100;
    const QImage refined = SubjectRemoval::refined(edge, guide, highContrast);
    QCOMPARE(refined.constScanLine(0)[1], uchar(0));
    QCOMPARE(refined.constScanLine(0)[7], uchar(255));
}

void EditorSessionTests::fourKBrushPerformanceWhenRequested()
{
    if (qEnvironmentVariableIntValue("BRUSH_BENCHMARK") != 1) return;
    for (double diameter : {40.0, 800.0}) {
        EditorSession session; session.createDocument(4000, 4000, true);
        QVector<qint64> frames; frames.reserve(120);
        QVERIFY(session.beginBrushStroke(QPointF(700, 3200), Qt::white, diameter, 0, 1));
        for (int i = 1; i <= 120; ++i) {
            const QPointF point = i <= 60 ? QPointF(700, 3200 - i * 40) : QPointF(700 + (i - 60) * 40, 800);
            QElapsedTimer timer; timer.start(); session.continueBrushStroke(point); frames.push_back(timer.nsecsElapsed() / 1'000'000);
        }
        QElapsedTimer finish; finish.start(); QVERIFY(session.endBrushStroke()); const qint64 finishMs = finish.elapsed();
        std::sort(frames.begin(), frames.end());
        qInfo().nospace() << "BRUSH_BENCH diameter=" << diameter << " median=" << frames[frames.size()/2]
                          << "ms p95=" << frames[int(frames.size()*.95)] << "ms max=" << frames.constLast()
                          << "ms mouseUp=" << finishMs << "ms";
    }
}

void EditorSessionTests::selectionAntialiasingControlsDiagonalCoverage()
{
    const QPolygonF triangle{QPointF(0, 0), QPointF(100, 0), QPointF(0, 100)};
    EditorSession soft; soft.createDocument(100, 100, false); QVERIFY(soft.setPolygonSelection(triangle, SelectionMode::Replace, true));
    EditorSession hard; hard.createDocument(100, 100, false); QVERIFY(hard.setPolygonSelection(triangle, SelectionMode::Replace, false));
    bool hasSoftEdge = false;
    for (int x = 0; x < 100; ++x) {
        const int smooth = soft.document()->selection->constScanLine(99 - x)[x];
        const int crisp = hard.document()->selection->constScanLine(99 - x)[x];
        hasSoftEdge |= smooth > 0 && smooth < 255;
        QVERIFY(crisp == 0 || crisp == 255);
    }
    QVERIFY(hasSoftEdge);
}

QTEST_GUILESS_MAIN(EditorSessionTests)
#include "EditorSessionTests.moc"
