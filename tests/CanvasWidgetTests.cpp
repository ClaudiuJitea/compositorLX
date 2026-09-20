#include "ui/CanvasWidget.h"
#include "core/EditorSession.h"

#include <QSignalSpy>
#include <QtTest>

using namespace compositor;

class CanvasWidgetTests final : public QObject {
    Q_OBJECT
private slots:
    void cropStaysPendingUntilApplyAndEscapeCancels();
    void cropRatioCreatesCenteredFrame();
    void toolAndHandleHoverCursorsMatchOperations();
    void hiddenTransformControlsLeaveOnlyMovementUntilPersistentEdit();
    void modifierHoverCursorsMatchMoveAndSelectionPresses();
    void everyToolAndOutsideHandReleaseRestoreTheArrow();
    void backgroundRenderPublishesNewestGeneration();
    void selectionOptionsReachCanvasSignals();
    void zoomToolKeepsDocumentPointUnderCursor();
    void blendModePreviewIsTemporaryAndRenders();
    void documentDropMappingRejectsMarginsAndTracksViewport();
    void arrowKeysNudgeMoveTargetsAndSelection();
    void shiftClickContinuesBrushFromPreviousEndpoint();
    void shapeClickAndEscapeDoNotCreateShapes();
    void hueTargetingOverridesTheActiveToolDuringDrag();
    void floatingPanelBlockAllowsSamplingButRejectsEdits();
    void cloneSourceCrossTracksTheSampledPosition();
};

static QPoint viewPoint(const CanvasWidget &canvas, const QSize &document, const QPointF &point)
{
    const QSizeF shown(document.width() * canvas.zoom(), document.height() * canvas.zoom());
    const QPointF origin((canvas.width() - shown.width()) / 2.0, (canvas.height() - shown.height()) / 2.0);
    return (origin + point * canvas.zoom()).toPoint();
}

void CanvasWidgetTests::cloneSourceCrossTracksTheSampledPosition()
{
    EditorSession session; session.createDocument(120, 100, true);
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(session.document());
    QCoreApplication::processEvents(); canvas.actualPixels(); canvas.setTool(CanvasWidget::Tool::Clone);
    bool aligned = true;
    connect(&canvas, &CanvasWidget::cloneSourceRequested, &canvas, [&](const QPointF &point) {
        session.setCloneSource(point); canvas.setCloneSource(point);
    });
    connect(&canvas, &CanvasWidget::cloneStrokeStarted, &canvas, [&](const QPointF &point) {
        QVERIFY(session.beginCloneStroke(point, 8, 1, 1, aligned));
        canvas.setCloneTracking(session.cloneSource(), aligned ? session.cloneOffset() : std::optional<QPointF>(*session.cloneSource() - point));
    });
    connect(&canvas, &CanvasWidget::brushStrokeContinued, &canvas, [&](const QPointF &point) { session.continueBrushStroke(point); });
    connect(&canvas, &CanvasWidget::brushStrokeFinished, &canvas, [&] { session.endBrushStroke(); });
    const auto at = [&](int x, int y) { return viewPoint(canvas, session.document()->canvasSize, QPointF(x, y)); };
    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::AltModifier, at(20, 20));
    const QPointF source = *canvas.documentPointAt(at(20, 20));
    QTest::mousePress(&canvas, Qt::LeftButton, {}, at(60, 60));
    QCOMPARE(canvas.cloneIndicatorPosition(), std::optional<QPointF>(source));
    QTest::mouseMove(&canvas, at(70, 68));
    QCOMPARE(canvas.cloneIndicatorPosition(), std::optional<QPointF>(source + QPointF(10, 8)));
    QTest::mouseRelease(&canvas, Qt::LeftButton, {}, at(70, 68));
    QTest::mouseMove(&canvas, at(80, 70));
    QCOMPARE(canvas.cloneIndicatorPosition(), std::optional<QPointF>(source + QPointF(20, 10)));
    QTest::mouseClick(&canvas, Qt::LeftButton, {}, at(80, 70));
    QCOMPARE(canvas.cloneIndicatorPosition(), std::optional<QPointF>(source + QPointF(20, 10)));
    aligned = false; canvas.setCloneAligned(false);
    QTest::mousePress(&canvas, Qt::LeftButton, {}, at(80, 70));
    QCOMPARE(canvas.cloneIndicatorPosition(), std::optional<QPointF>(source));
    QTest::mouseMove(&canvas, at(85, 74));
    QCOMPARE(canvas.cloneIndicatorPosition(), std::optional<QPointF>(source + QPointF(5, 4)));
    QTest::mouseRelease(&canvas, Qt::LeftButton, {}, at(85, 74));
    QCOMPARE(canvas.cloneIndicatorPosition(), std::optional<QPointF>(source));
    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::AltModifier, at(35, 15));
    QCOMPARE(canvas.cloneIndicatorPosition(), canvas.documentPointAt(at(35, 15)));
}

void CanvasWidgetTests::cropStaysPendingUntilApplyAndEscapeCancels()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 50);
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas(); canvas.setTool(CanvasWidget::Tool::Crop);
    QSignalSpy crop(&canvas, &CanvasWidget::cropRequested);
    QSignalSpy pending(&canvas, &CanvasWidget::pendingCropChanged);
    QTest::mousePress(&canvas, Qt::LeftButton, {}, viewPoint(canvas, document->canvasSize, QPointF(10, 10)));
    QTest::mouseMove(&canvas, viewPoint(canvas, document->canvasSize, QPointF(80, 40)));
    QTest::mouseRelease(&canvas, Qt::LeftButton, {}, viewPoint(canvas, document->canvasSize, QPointF(80, 40)));
    QCOMPARE(crop.count(), 0);
    QVERIFY(!pending.isEmpty()); QCOMPARE(pending.constLast().at(0).toBool(), true);
    QTest::keyClick(&canvas, Qt::Key_Return);
    QCOMPARE(crop.count(), 1);
    const QRect result = qvariant_cast<QRect>(crop.takeFirst().at(0));
    QCOMPARE(result, QRect(10, 10, 70, 30));

    QTest::mousePress(&canvas, Qt::LeftButton, {}, viewPoint(canvas, document->canvasSize, QPointF(5, 5)));
    QTest::mouseMove(&canvas, viewPoint(canvas, document->canvasSize, QPointF(30, 25)));
    QTest::mouseRelease(&canvas, Qt::LeftButton, {}, viewPoint(canvas, document->canvasSize, QPointF(30, 25)));
    QTest::keyClick(&canvas, Qt::Key_Escape);
    QCOMPARE(crop.count(), 0);
    QCOMPARE(pending.constLast().at(0).toBool(), false);
}

void CanvasWidgetTests::cropRatioCreatesCenteredFrame()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 50);
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas(); canvas.setTool(CanvasWidget::Tool::Crop);
    QSignalSpy crop(&canvas, &CanvasWidget::cropRequested);
    canvas.setCropRatio(1);
    QTest::keyClick(&canvas, Qt::Key_Return);
    QCOMPARE(crop.count(), 1);
    QCOMPARE(qvariant_cast<QRect>(crop.takeFirst().at(0)), QRect(0, -25, 100, 100));
}

void CanvasWidgetTests::toolAndHandleHoverCursorsMatchOperations()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 50);
    Layer layer; layer.id = QUuid::createUuid(); layer.name = QStringLiteral("Layer"); layer.transform.size = document->canvasSize;
    layer.image = QImage(document->canvasSize, QImage::Format_RGBA8888_Premultiplied); layer.image.fill(Qt::red);
    document->layers = {layer}; document->activeLayerId = layer.id;
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas(); canvas.setSelectedLayerIds({layer.id});
    canvas.setTool(CanvasWidget::Tool::Brush); QCOMPARE(canvas.cursor().shape(), Qt::BlankCursor);
    QEvent leave(QEvent::Leave); QApplication::sendEvent(&canvas, &leave); QCOMPARE(canvas.cursor().shape(), Qt::ArrowCursor);
    const QPoint brushPoint = viewPoint(canvas, document->canvasSize, QPointF(40, 20));
    QMouseEvent brushMove(QEvent::MouseMove, brushPoint, canvas.mapToGlobal(brushPoint), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &brushMove); QCOMPARE(canvas.cursor().shape(), Qt::BlankCursor);
    canvas.setTool(CanvasWidget::Tool::Hand); QCOMPARE(canvas.cursor().shape(), Qt::OpenHandCursor);
    canvas.setTool(CanvasWidget::Tool::Zoom); QCOMPARE(canvas.cursor().shape(), Qt::BitmapCursor);
    canvas.setTool(CanvasWidget::Tool::Eyedropper); QCOMPARE(canvas.cursor().shape(), Qt::BitmapCursor);
    canvas.setTool(CanvasWidget::Tool::Crop);
    const QPoint cropPoint = viewPoint(canvas, document->canvasSize, QPointF(0, 0));
    QMouseEvent cropMove(QEvent::MouseMove, cropPoint, canvas.mapToGlobal(cropPoint), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &cropMove);
    QCOMPARE(canvas.cursor().shape(), Qt::SizeFDiagCursor);
    canvas.setTool(CanvasWidget::Tool::Move);
    const QPoint movePoint = viewPoint(canvas, document->canvasSize, QPointF(50, 25));
    QMouseEvent moveEvent(QEvent::MouseMove, movePoint, canvas.mapToGlobal(movePoint), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &moveEvent);
    QCOMPARE(canvas.cursor().shape(), Qt::SizeAllCursor);
}

void CanvasWidgetTests::hiddenTransformControlsLeaveOnlyMovementUntilPersistentEdit()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 50);
    Layer layer; layer.id = QUuid::createUuid(); layer.name = QStringLiteral("Layer");
    layer.transform.origin = QPointF(25, 10); layer.transform.size = QSizeF(50, 25);
    layer.image = QImage(50, 25, QImage::Format_RGBA8888_Premultiplied); layer.image.fill(Qt::red);
    document->layers = {layer}; document->activeLayerId = layer.id;
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas();
    canvas.setSelectedLayerIds({layer.id}); canvas.setTool(CanvasWidget::Tool::Move);
    const QPoint corner = viewPoint(canvas, document->canvasSize, layer.transform.origin + QPointF(1, 1));
    const auto hover = [&](Qt::KeyboardModifiers modifiers) {
        QMouseEvent move(QEvent::MouseMove, corner, canvas.mapToGlobal(corner), Qt::NoButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(&canvas, &move);
    };

    hover(Qt::NoModifier); QCOMPARE(canvas.cursor().shape(), Qt::SizeFDiagCursor);
    canvas.setShowTransformControls(false);
    hover(Qt::NoModifier); QCOMPARE(canvas.cursor().shape(), Qt::SizeAllCursor);
    hover(Qt::ControlModifier); QCOMPARE(canvas.cursor().shape(), Qt::SizeAllCursor);
    canvas.setPersistentTransform(true);
    hover(Qt::NoModifier); QCOMPARE(canvas.cursor().shape(), Qt::SizeFDiagCursor);
}

void CanvasWidgetTests::modifierHoverCursorsMatchMoveAndSelectionPresses()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 100);
    Layer layer; layer.id = QUuid::createUuid(); layer.name = QStringLiteral("Layer");
    layer.transform.origin = QPointF(40, 40); layer.transform.size = QSizeF(20, 20);
    layer.image = QImage(20, 20, QImage::Format_RGBA8888_Premultiplied); layer.image.fill(Qt::red);
    document->layers = {layer}; document->activeLayerId = layer.id;
    CanvasWidget canvas; canvas.resize(400, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas();
    canvas.setSelectedLayerIds({layer.id}); canvas.setTool(CanvasWidget::Tool::Move);
    const auto hover = [&](const QPointF &documentPoint, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const QPoint point = viewPoint(canvas, document->canvasSize, documentPoint);
        QMouseEvent move(QEvent::MouseMove, point, canvas.mapToGlobal(point), Qt::NoButton, Qt::NoButton, modifiers);
        QApplication::sendEvent(&canvas, &move);
    };

    hover(QPointF(10, 10)); QCOMPARE(canvas.cursor().shape(), Qt::SizeAllCursor);
    hover(QPointF(10, 10), Qt::AltModifier); QCOMPARE(canvas.cursor().shape(), Qt::DragCopyCursor);
    hover(QPointF(41, 41), Qt::ControlModifier); QCOMPARE(canvas.cursor().shape(), Qt::CrossCursor);
    hover(QPointF(50, 40 - 28.0 / canvas.zoom())); QCOMPARE(canvas.cursor().shape(), Qt::CrossCursor);

    const QPoint outside = viewPoint(canvas, document->canvasSize, QPointF(10, 10));
    const QPoint moved = viewPoint(canvas, document->canvasSize, QPointF(30, 20));
    QTest::mousePress(&canvas, Qt::LeftButton, {}, outside); QTest::mouseMove(&canvas, moved); QTest::mouseRelease(&canvas, Qt::LeftButton, {}, moved);
    QCOMPARE(document->layers[0].transform.origin, QPointF(60, 50));
    QSignalSpy duplicate(&canvas, &CanvasWidget::duplicateLayerForTransformRequested);
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::AltModifier, outside);
    QTest::mouseMove(&canvas, moved, 1); QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::AltModifier, moved);
    QCOMPARE(duplicate.count(), 1);

    document->selection = QImage(document->canvasSize, QImage::Format_Grayscale8); document->selection->fill(0);
    for (int y = 45; y < 55; ++y) std::fill_n(document->selection->scanLine(y) + 45, 10, uchar(255));
    canvas.setTool(CanvasWidget::Tool::Marquee);
    hover(QPointF(50, 50), Qt::ControlModifier); QCOMPARE(canvas.cursor().shape(), Qt::SizeAllCursor);
    hover(QPointF(50, 50), Qt::ControlModifier | Qt::AltModifier); QCOMPARE(canvas.cursor().shape(), Qt::DragCopyCursor);
    hover(QPointF(10, 10), Qt::ControlModifier | Qt::AltModifier); QCOMPARE(canvas.cursor().shape(), Qt::CrossCursor);

    document->layers[0].image = {};
    canvas.setTool(CanvasWidget::Tool::Move);
    hover(QPointF(50, 50)); QCOMPARE(canvas.cursor().shape(), Qt::ArrowCursor);
    QSignalSpy started(&canvas, &CanvasWidget::layerTransformStarted);
    QTest::mousePress(&canvas, Qt::LeftButton, {}, viewPoint(canvas, document->canvasSize, QPointF(50, 50)));
    QCOMPARE(started.count(), 0);
}

void CanvasWidgetTests::everyToolAndOutsideHandReleaseRestoreTheArrow()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 50);
    Layer layer; layer.id = QUuid::createUuid(); layer.transform.size = document->canvasSize;
    layer.image = QImage(document->canvasSize, QImage::Format_RGBA8888_Premultiplied); layer.image.fill(Qt::red);
    document->layers = {layer}; document->activeLayerId = layer.id;
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas(); canvas.setSelectedLayerIds({layer.id});
    const std::array tools{CanvasWidget::Tool::Move, CanvasWidget::Tool::Marquee, CanvasWidget::Tool::Lasso, CanvasWidget::Tool::Wand,
        CanvasWidget::Tool::Crop, CanvasWidget::Tool::Brush, CanvasWidget::Tool::Eraser, CanvasWidget::Tool::Healing,
        CanvasWidget::Tool::Clone, CanvasWidget::Tool::Blur, CanvasWidget::Tool::Gradient, CanvasWidget::Tool::Shape,
        CanvasWidget::Tool::Eyedropper, CanvasWidget::Tool::Hand, CanvasWidget::Tool::Zoom, CanvasWidget::Tool::Other};
    for (const CanvasWidget::Tool tool : tools) {
        canvas.setTool(tool); QEvent leave(QEvent::Leave); QApplication::sendEvent(&canvas, &leave);
        QCOMPARE(canvas.cursor().shape(), Qt::ArrowCursor);
    }

    canvas.setTool(CanvasWidget::Tool::Hand);
    const QPoint inside = canvas.rect().center(); QTest::mousePress(&canvas, Qt::LeftButton, {}, inside);
    QCOMPARE(canvas.cursor().shape(), Qt::ClosedHandCursor);
    const QPointF outside(-20, inside.y());
    QMouseEvent release(QEvent::MouseButtonRelease, outside, canvas.mapToGlobal(outside.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release);
    QCOMPARE(canvas.cursor().shape(), Qt::ArrowCursor);
}

void CanvasWidgetTests::backgroundRenderPublishesNewestGeneration()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(64, 64);
    Layer layer; layer.id = QUuid::createUuid(); layer.name = QStringLiteral("Layer"); layer.transform.size = document->canvasSize;
    layer.image = QImage(document->canvasSize, QImage::Format_RGBA8888_Premultiplied); layer.image.fill(Qt::red);
    document->layers = {layer}; document->activeLayerId = layer.id;
    CanvasWidget canvas; canvas.resize(240, 180); canvas.show(); canvas.setDocument(document); canvas.fitCanvas();
    QCOMPARE(canvas.grab().toImage().pixelColor(canvas.rect().center()), QColor(Qt::red));

    document->layers[0].image.fill(Qt::green); canvas.invalidateDocument();
    document->layers[0].image.fill(Qt::blue); canvas.invalidateDocument();
    QTRY_COMPARE_WITH_TIMEOUT(canvas.grab().toImage().pixelColor(canvas.rect().center()), QColor(Qt::blue), 3000);
}

void CanvasWidgetTests::selectionOptionsReachCanvasSignals()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 100);
    CanvasWidget canvas; canvas.resize(400, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas();
    canvas.setTool(CanvasWidget::Tool::Marquee); canvas.setMarqueeElliptical(true); canvas.setSelectionMode(1); canvas.setSelectionAntialiased(false);
    QSignalSpy ellipse(&canvas, &CanvasWidget::ellipticalSelectionRequested);
    QTest::mousePress(&canvas, Qt::LeftButton, {}, viewPoint(canvas, document->canvasSize, QPointF(10, 10)));
    QTest::mouseMove(&canvas, viewPoint(canvas, document->canvasSize, QPointF(70, 60)));
    QTest::mouseRelease(&canvas, Qt::LeftButton, {}, viewPoint(canvas, document->canvasSize, QPointF(70, 60)));
    QCOMPARE(ellipse.count(), 1); QCOMPARE(ellipse.constFirst().at(1).toInt(), 1); QCOMPARE(ellipse.constFirst().at(2).toBool(), false);
}

void CanvasWidgetTests::zoomToolKeepsDocumentPointUnderCursor()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 100);
    CanvasWidget canvas; canvas.resize(400, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas();
    const QPoint anchor = viewPoint(canvas, document->canvasSize, QPointF(20, 30));
    QSignalSpy sampled(&canvas, &CanvasWidget::colorSampleRequested);

    canvas.setTool(CanvasWidget::Tool::Eyedropper);
    QTest::mouseClick(&canvas, Qt::LeftButton, {}, anchor);
    QCOMPARE(sampled.count(), 1);
    const QPoint before = sampled.takeFirst().at(0).toPoint();

    canvas.setTool(CanvasWidget::Tool::Zoom);
    QTest::mouseClick(&canvas, Qt::LeftButton, {}, anchor);
    canvas.setTool(CanvasWidget::Tool::Eyedropper);
    QTest::mouseClick(&canvas, Qt::LeftButton, {}, anchor);
    QCOMPARE(sampled.count(), 1);
    QCOMPARE(sampled.takeFirst().at(0).toPoint(), before);
}

void CanvasWidgetTests::documentDropMappingRejectsMarginsAndTracksViewport()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 50);
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas();
    QVERIFY(!canvas.documentPointAt(QPointF(2, 2)));
    const QPoint shown = viewPoint(canvas, document->canvasSize, QPointF(23, 17));
    const auto mapped = canvas.documentPointAt(shown); QVERIFY(mapped);
    QVERIFY(QLineF(*mapped, QPointF(23, 17)).length() < .5);
    QWheelEvent pan(QPointF(250, 200), canvas.mapToGlobal(QPoint(250, 200)), QPoint(30, -20), QPoint(),
                    Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &pan);
    const auto after = canvas.documentPointAt(shown + QPoint(30, -20)); QVERIFY(after);
    QVERIFY(QLineF(*after, QPointF(23, 17)).length() < .5);
}

void CanvasWidgetTests::arrowKeysNudgeMoveTargetsAndSelection()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 100);
    Layer layer; layer.id = QUuid::createUuid(); layer.name = QStringLiteral("Pixels"); layer.transform.size = QSizeF(20, 20);
    layer.image = QImage(20, 20, QImage::Format_RGBA8888_Premultiplied); layer.image.fill(Qt::red);
    document->layers = {layer}; document->activeLayerId = layer.id;
    CanvasWidget canvas; canvas.resize(400, 400); canvas.show(); canvas.setDocument(document); canvas.setSelectedLayerIds({layer.id}); canvas.setTool(CanvasWidget::Tool::Move);
    QSignalSpy started(&canvas, &CanvasWidget::layerTransformStarted); QSignalSpy finished(&canvas, &CanvasWidget::layerTransformFinished);
    QTest::keyClick(&canvas, Qt::Key_Right); QCOMPARE(document->layers[0].transform.origin, QPointF(1, 0)); QCOMPARE(started.count(), 1); QCOMPARE(finished.count(), 1);
    QTest::keyClick(&canvas, Qt::Key_Down, Qt::ShiftModifier); QCOMPARE(document->layers[0].transform.origin, QPointF(1, 10));

    document->selection = QImage(document->canvasSize, QImage::Format_Grayscale8); document->selection->fill(0); document->selection->setPixel(5, 5, 255);
    canvas.setTool(CanvasWidget::Tool::Marquee); QSignalSpy selection(&canvas, &CanvasWidget::selectionMoveRequested);
    QTest::keyClick(&canvas, Qt::Key_Left); QCOMPARE(selection.count(), 1); QCOMPARE(selection.takeFirst().at(0).toPoint(), QPoint(-1, 0));
    QSignalSpy pixels(&canvas, &CanvasWidget::selectedPixelsNudgeRequested);
    QTest::keyClick(&canvas, Qt::Key_Right, Qt::ControlModifier); QCOMPARE(pixels.count(), 1); QCOMPARE(pixels.takeFirst().at(0).toPoint(), QPoint(1, 0));
    QCOMPARE(selection.count(), 0);
    QSignalSpy pixelDrag(&canvas, &CanvasWidget::selectedPixelsDragStarted);
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::ControlModifier | Qt::AltModifier,
                      viewPoint(canvas, document->canvasSize, QPointF(5, 5)));
    QCOMPARE(pixelDrag.count(), 1); QCOMPARE(pixelDrag.takeFirst().at(0).toBool(), true);
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::ControlModifier | Qt::AltModifier,
                        viewPoint(canvas, document->canvasSize, QPointF(5, 5)));

    Layer folder; folder.id = QUuid::createUuid(); folder.name = QStringLiteral("Folder"); folder.group = true;
    document->layers[0].parentId = folder.id; document->layers.push_back(folder); document->activeLayerId = folder.id;
    canvas.setSelectedLayerIds({folder.id}); canvas.setTool(CanvasWidget::Tool::Move);
    QTest::keyClick(&canvas, Qt::Key_Right); QCOMPARE(document->layers[0].transform.origin, QPointF(2, 10));
}

void CanvasWidgetTests::shiftClickContinuesBrushFromPreviousEndpoint()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 50);
    Layer layer; layer.id = QUuid::createUuid(); layer.name = QStringLiteral("Pixels"); layer.transform.size = document->canvasSize;
    layer.image = QImage(document->canvasSize, QImage::Format_RGBA8888_Premultiplied); layer.image.fill(Qt::transparent);
    document->layers = {layer}; document->activeLayerId = layer.id;
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(document); canvas.setTool(CanvasWidget::Tool::Brush);
    QSignalSpy started(&canvas, &CanvasWidget::brushStrokeStarted); QSignalSpy continued(&canvas, &CanvasWidget::brushStrokeContinued); QSignalSpy finished(&canvas, &CanvasWidget::brushStrokeFinished);
    const QPoint first = viewPoint(canvas, document->canvasSize, QPointF(10, 12));
    const QPoint second = viewPoint(canvas, document->canvasSize, QPointF(70, 35));
    QTest::mouseClick(&canvas, Qt::LeftButton, {}, first); QCOMPARE(started.count(), 1); QCOMPARE(finished.count(), 1);
    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::ShiftModifier, second);
    QCOMPARE(started.count(), 2); QVERIFY(QLineF(started.at(1).at(0).toPointF(), QPointF(10, 12)).length() < .1);
    QCOMPARE(continued.count(), 1); QVERIFY(QLineF(continued.at(0).at(0).toPointF(), QPointF(70, 35)).length() < .1); QCOMPARE(finished.count(), 2);
    QSignalSpy cancelled(&canvas, &CanvasWidget::brushStrokeCancelRequested);
    QTest::mousePress(&canvas, Qt::LeftButton, {}, first); QTest::keyClick(&canvas, Qt::Key_Escape); QCOMPARE(cancelled.count(), 1);
    QTest::mouseRelease(&canvas, Qt::LeftButton, {}, first); QCOMPARE(finished.count(), 2);
}

void CanvasWidgetTests::shapeClickAndEscapeDoNotCreateShapes()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 60);
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(document); canvas.setTool(CanvasWidget::Tool::Shape);
    QSignalSpy shapes(&canvas, &CanvasWidget::shapeRequested); const QPoint start = viewPoint(canvas, document->canvasSize, QPointF(20, 20));
    QTest::mouseClick(&canvas, Qt::LeftButton, {}, start); QCOMPARE(shapes.count(), 0);
    QTest::mousePress(&canvas, Qt::LeftButton, {}, start); QTest::mouseMove(&canvas, viewPoint(canvas, document->canvasSize, QPointF(70, 45)));
    QTest::keyClick(&canvas, Qt::Key_Escape); QTest::mouseRelease(&canvas, Qt::LeftButton, {}, viewPoint(canvas, document->canvasSize, QPointF(70, 45)));
    QCOMPARE(shapes.count(), 0);
}

void CanvasWidgetTests::hueTargetingOverridesTheActiveToolDuringDrag()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 50);
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas(); canvas.setTool(CanvasWidget::Tool::Brush);
    canvas.setHueTargeting(true); QSignalSpy started(&canvas, &CanvasWidget::hueTargetStarted); QSignalSpy dragged(&canvas, &CanvasWidget::hueTargetDragged); QSignalSpy finished(&canvas, &CanvasWidget::hueTargetFinished);
    const QPoint first = viewPoint(canvas, document->canvasSize, QPointF(20, 20)); const QPoint second = first + QPoint(30, 0);
    QTest::mousePress(&canvas, Qt::LeftButton, {}, first); QCOMPARE(started.count(), 1); QVERIFY((started.constFirst().at(0).toPoint() - QPoint(20, 20)).manhattanLength() <= 2);
    QTest::mouseMove(&canvas, second); QCOMPARE(dragged.count(), 1); QCOMPARE(dragged.constFirst().at(0).toDouble(), 30.0); QCOMPARE(dragged.constFirst().at(1).toBool(), false);
    QTest::mouseRelease(&canvas, Qt::LeftButton, {}, second); QCOMPARE(finished.count(), 1);
    canvas.setHueTargeting(false); QCOMPARE(canvas.tool(), CanvasWidget::Tool::Brush);
}

void CanvasWidgetTests::floatingPanelBlockAllowsSamplingButRejectsEdits()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(100, 50);
    Layer layer; layer.id = QUuid::createUuid(); layer.transform.size = document->canvasSize; layer.image = QImage(document->canvasSize, QImage::Format_RGBA8888_Premultiplied); layer.image.fill(Qt::red); document->layers = {layer}; document->activeLayerId = layer.id;
    CanvasWidget canvas; canvas.resize(500, 400); canvas.show(); canvas.setDocument(document); canvas.fitCanvas(); canvas.setSelectedLayerIds({layer.id}); canvas.setTool(CanvasWidget::Tool::Brush); canvas.setEditorInteractionBlocked(true); QCOMPARE(canvas.cursor().shape(), Qt::ArrowCursor);
    const QPoint center = viewPoint(canvas, document->canvasSize, QPointF(50, 25)); QSignalSpy transforms(&canvas, &CanvasWidget::layerTransformStarted);
    canvas.setTool(CanvasWidget::Tool::Move); QTest::mousePress(&canvas, Qt::LeftButton, {}, center); QTest::mouseMove(&canvas, center + QPoint(30, 0)); QTest::mouseRelease(&canvas, Qt::LeftButton, {}, center + QPoint(30, 0)); QCOMPARE(transforms.count(), 0); QCOMPARE(document->layers[0].transform.origin, QPointF());
    QSignalSpy samples(&canvas, &CanvasWidget::colorSampleRequested); canvas.setTool(CanvasWidget::Tool::Eyedropper); QTest::mouseClick(&canvas, Qt::LeftButton, {}, center); QCOMPARE(samples.count(), 1);
    canvas.setTool(CanvasWidget::Tool::Brush); canvas.setEditorInteractionBlocked(false); QCOMPARE(canvas.cursor().shape(), Qt::BlankCursor);
}

void CanvasWidgetTests::blendModePreviewIsTemporaryAndRenders()
{
    auto document = std::make_shared<Document>(); document->id = QUuid::createUuid(); document->canvasSize = QSize(8, 8);
    Layer bottom; bottom.id = QUuid::createUuid(); bottom.name = QStringLiteral("Bottom"); bottom.transform.size = document->canvasSize;
    bottom.image = QImage(document->canvasSize, QImage::Format_RGBA8888_Premultiplied); bottom.image.fill(QColor::fromRgbF(.4, .4, .4));
    Layer top; top.id = QUuid::createUuid(); top.name = QStringLiteral("Top"); top.transform.size = document->canvasSize;
    top.image = QImage(document->canvasSize, QImage::Format_RGBA8888_Premultiplied); top.image.fill(QColor::fromRgbF(.8, .8, .8));
    document->layers = {bottom, top}; document->activeLayerId = top.id;
    CanvasWidget canvas; canvas.resize(300, 300); canvas.show(); canvas.setDocument(document); canvas.fitCanvas();
    QTRY_VERIFY_WITH_TIMEOUT(canvas.grab().toImage().pixelColor(canvas.rect().center()).red() > 150, 3000);
    const int normal = canvas.grab().toImage().pixelColor(canvas.rect().center()).red();

    canvas.setBlendModePreview(top.id, BlendMode::Multiply);
    QTRY_VERIFY_WITH_TIMEOUT(canvas.grab().toImage().pixelColor(canvas.rect().center()).red() < normal - 40, 3000);
    QCOMPARE(document->layers.constLast().blendMode, BlendMode::Normal);
    canvas.setBlendModePreview(std::nullopt, std::nullopt);
    QTRY_VERIFY_WITH_TIMEOUT(canvas.grab().toImage().pixelColor(canvas.rect().center()).red() >= normal - 2, 3000);
}

QTEST_MAIN(CanvasWidgetTests)
#include "CanvasWidgetTests.moc"
