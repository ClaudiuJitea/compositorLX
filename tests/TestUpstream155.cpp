#include "core/EditorSession.h"
#include "io/GifIO.h"
#include "io/PdfImporter.h"
#include "io/ChannelPack.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "io/AdjustmentJson.h"
#include "io/PSDReader.h"
#include "rendering/RgbSplit.h"
#include "rendering/LayerRenderer.h"
#include "ui/MainWindow.h"
#include "ui/CanvasWidget.h"
#include "ui/InlineTextEditor.h"
#include "ui/LayerListModel.h"
#include "ui/ShortcutManager.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPdfWriter>
#include <QBuffer>
#include <QImageReader>
#include <QComboBox>
#include <QAction>
#include <QFontComboBox>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QTabletEvent>
#include <QPointingDevice>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QTimer>

using namespace compositor;
class TestUpstream155 : public QObject {
    Q_OBJECT
    static QImage solid(QColor c, QSize size = {32, 32}) { QImage image(size, QImage::Format_RGBA8888_Premultiplied); image.fill(c); return image; }
    static QByteArray gifBytes() { return QByteArray::fromBase64("R0lGODlhCAAIAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQIBAAAACwAAAAACAAIAAAIDwABCBxIsKDBgwgTKkwYEAAh+QQICAAAACwAAAAACAAIAIEAgAAAAAAAAAAAAAAIDwABCBxIsKDBgwgTKkwYEAAh+QQIDAAAACwAAAAACAAIAIEAAP8AAAAAAAAAAAAIDwABCBxIsKDBgwgTKkwYEAA7"); }
    static QString fixture(QTemporaryDir &dir) { QString path = dir.filePath("colors.gif"); QFile f(path); if (f.open(QIODevice::WriteOnly)) f.write(gifBytes()); return path; }
private slots:
    void initTestCase() { QCoreApplication::setOrganizationName("compositor-tests"); QCoreApplication::setApplicationName("upstream155"); }
    void gifDecodeAndTiming() {
        QString error; auto a = GifIO::read(gifBytes(), &error); QVERIFY2(a.has_value(), qPrintable(error));
        QCOMPARE(a->frames.size(), 3); QCOMPARE(a->source->size, QSize(8, 8));
        QCOMPARE(a->delay(0), 40); QCOMPARE(a->delay(1), 80); QCOMPARE(a->delay(2), 120);
        QCOMPARE(a->frame(0).pixelColor(4,4), QColor(Qt::red)); QCOMPARE(a->frame(2).pixelColor(4,4), QColor(Qt::blue));
    }
    void gifEditsUndoAndProjectRoundTrip() {
        QTemporaryDir dir; EditorSession s; QVERIFY(s.insertGif(fixture(dir))); s.markSaved();
        QVERIFY(s.selectGifFrame(1)); QVERIFY(!s.isModified());
        s.beginEdit("Paint frame"); s.activeLayer()->image = solid(Qt::yellow, {8,8}); s.endEdit();
        QVERIFY(s.activeLayer()->gif->pixels.contains(1)); QVERIFY(s.isModified());
        QVERIFY(s.selectGifFrame(0)); QCOMPARE(s.activeLayer()->image.pixelColor(3,3), QColor(Qt::red));
        QVERIFY(s.selectGifFrame(1)); QCOMPARE(s.activeLayer()->image.pixelColor(3,3), QColor(Qt::yellow));
        QVERIFY(s.duplicateGifFrame()); QCOMPARE(s.activeLayer()->gif->frames.size(), 4); const int copied = s.activeLayer()->gif->selected;
        QVERIFY(copied >= 3); QVERIFY(s.setGifDelay(160));
        const QString package = dir.filePath("test.comp"); ProjectWriter::save(*s.document(), package);
        auto loaded = ProjectReader::load(package); QCOMPARE(loaded.formatVersion, 17);
        const auto &a = *loaded.layers.last().gif; QCOMPARE(a.frames.size(), 4); QCOMPARE(a.delay(copied), 160); QCOMPARE(a.frame(1).pixelColor(3,3), QColor(Qt::yellow));
        QVERIFY(s.deleteGifFrame()); QCOMPARE(s.activeLayer()->gif->frames.size(), 3); s.undo(); QCOMPARE(s.activeLayer()->gif->frames.size(), 4);
        s.undo(); QCOMPARE(s.activeLayer()->gif->delay(copied), 80);
    }
    void gifExportCompositesAndPreservesDelays() {
        QTemporaryDir dir; EditorSession s; QVERIFY(s.insertGif(fixture(dir))); QString error;
        const QString out = dir.filePath("out.gif"); QVERIFY2(GifIO::write(*s.document(), out, {16,16}, true, true, &error), qPrintable(error));
        auto a = GifIO::readFile(out, &error); QVERIFY2(a.has_value(), qPrintable(error)); QCOMPARE(a->frames.size(), 3);
        QCOMPARE(a->source->size, QSize(16,16)); QCOMPARE(a->delay(0), 40); QCOMPARE(a->delay(1), 80); QCOMPARE(a->delay(2), 120);
        QVERIFY(a->frame(2).pixelColor(5,5).blue() > 240);
    }
    void gifExportMergesDifferentLayerClocksAndHoldsLastFrame() {
        QTemporaryDir dir; EditorSession s; s.createDocument(16,8);
        const auto path = fixture(dir); QVERIFY(s.insertGif(path, QPointF(4,4))); QVERIFY(s.insertGif(path, QPointF(12,4))); QVERIFY(s.setGifDelay(60,true));
        const QString out = dir.filePath("merged.gif"); QVERIFY(GifIO::write(*s.document(),out,{16,8},false,true));
        const auto a = GifIO::readFile(out); QVERIFY(a); QCOMPARE(a->frames.size(),4);
        QCOMPARE(a->delay(0),40); QCOMPARE(a->delay(1),20); QCOMPARE(a->delay(2),60); QCOMPARE(a->delay(3),120);
        const auto middle=a->frame(1); QVERIFY(middle.pixelColor(3,4).green()>100); QVERIFY(middle.pixelColor(12,4).red()>240);
        const auto last=a->frame(3); QVERIFY(last.pixelColor(3,4).blue()>240); QVERIFY(last.pixelColor(12,4).blue()>240);
    }
    void gifShortTimelineKeepsItsTotalDuration() {
        QTemporaryDir dir; EditorSession s; QVERIFY(s.insertGif(fixture(dir)));
        QVERIFY(s.setGifDelay(20, true));
        const QString out = dir.filePath("short.gif"); QVERIFY(GifIO::write(*s.document(), out, {8,8}, false, true));
        const auto a = GifIO::readFile(out); QVERIFY(a); QCOMPARE(a->frames.size(), 3);
        int total = 0; for (const auto &f : a->frames) total += a->delay(f.index);
        QCOMPARE(total, 60);
    }
    void gifRejectsUnsafeTimeline() {
        auto a = GifIO::read(gifBytes()); QVERIFY(a);
        QString error; auto meta = GifIO::metadata(*a, "x"); meta.insert("selectedFrameIndex", 100); QVERIFY(!GifIO::restoreMetadata(*a, meta, &error));
        a = GifIO::read(gifBytes()); meta = GifIO::metadata(*a, "x"); meta.insert("frameOrder", QJsonArray{QJsonObject{{"index", 0},{"sourceIndex", 0}},QJsonObject{{"index", 0},{"sourceIndex", 1}}}); QVERIFY(!GifIO::restoreMetadata(*a, meta, &error));
    }
    void gifGrowthKeepsOtherFramesAligned() {
        QTemporaryDir dir; EditorSession s; s.createDocument(40,20); QVERIFY(s.insertGif(fixture(dir))); const auto original = s.activeLayer()->gif->frame(1);
        QVERIFY(s.beginBrushStroke({14,10}, Qt::yellow, 6)); QVERIFY(s.endBrushStroke());
        QVERIFY(!s.activeLayer()->gif->geometry.isEmpty());
        const auto size = s.activeLayer()->image.size(); QVERIFY(s.selectGifFrame(1)); QCOMPARE(s.activeLayer()->image.size(), size);
        QVERIFY(s.activeLayer()->image.width() > original.width());
    }
    void filterLayersRenderPersistAndUndo() {
        QTemporaryDir dir;
        for (const QString &kind : {QStringLiteral("Progressive Blur"),QStringLiteral("Bloom / Glow"),QStringLiteral("RGB Split")}) {
            EditorSession s; s.insertImage(solid(Qt::black), "Base"); for (int y=0;y<32;++y) for(int x=16;x<32;++x) s.activeLayer()->image.setPixelColor(x,y,Qt::white);
            const QImage before = LayerRenderer::flattened(*s.document());
            QVERIFY(s.addAdjustment(kind)); QCOMPARE(s.document()->formatVersion, 16); const QImage after = LayerRenderer::flattened(*s.document()); QVERIFY2(before != after, qPrintable(kind));
            const QString path = dir.filePath(kind.left(4).replace('/','_') + ".comp"); ProjectWriter::save(*s.document(), path);
            const auto doc = ProjectReader::load(path); QCOMPARE(doc.layers.last().adjustment.value("kind").toString(), kind); QCOMPARE(LayerRenderer::flattened(doc), after);
            s.undo(); QCOMPARE(LayerRenderer::flattened(*s.document()), before);
        }
    }
    void filterSettingsRejectInvalidValues() {
        for (const QJsonValue &value : {QJsonValue(-1), QJsonValue(201), QJsonValue("10"), QJsonValue(QJsonValue::Null)})
            QVERIFY(!adjustmentJsonError({{"kind","RGB Split"},{"filterLayerSettings",QJsonObject{{"rgbSplitDistance",value}}}}).isEmpty());
        QVERIFY(adjustmentJsonError({{"kind","RGB Split"},{"filterLayerSettings",QJsonObject{{"rgbSplitDistance",0}}}}).isEmpty());
    }
    void rgbSplitIdentityDispersionAndRepeat() {
        const QImage original = solid(Qt::white); RgbSplitSettings settings; settings.distance = 0; QCOMPARE(settings.apply(original), original);
        QImage edge = solid(Qt::black); for (int y = 0; y < 32; ++y) for(int x=16;x<32;++x) edge.setPixelColor(x,y,Qt::white);
        settings.distance = 8; const auto split = settings.apply(edge, 15); QVERIFY(split.pixelColor(15,16).red() != split.pixelColor(15,16).blue());
        EditorSession s; QVERIFY(s.insertImage(edge,"Edge")); QVERIFY(s.applyRgbSplit(settings)); QVERIFY(s.hasLastFilter()); s.undo(); QCOMPARE(s.activeLayer()->image,edge); QVERIFY(s.applyLastFilter());
    }
    void channelPackKeepsColorUnderZeroAlpha() {
        EditorSession s; s.insertImage(solid(Qt::white), "Red"); const auto id = s.activeLayer()->id;
        const std::array<ChannelSource,4> sources{{{id,false},{{},true},{{},false},{{},false}}};
        const auto packed = ChannelPack::render(*s.document(),sources,{16,16}); QCOMPARE(packed.format(),QImage::Format_RGBA8888);
        QCOMPARE(packed.constScanLine(0)[0],uchar(255)); QCOMPARE(packed.constScanLine(0)[3],uchar(0));
        const auto bytes = ChannelPack::png(packed,300); QVERIFY(!bytes.isEmpty()); const auto decoded = QImage::fromData(bytes,"PNG").convertToFormat(QImage::Format_RGBA8888);
        QCOMPARE(decoded.constScanLine(0)[0],uchar(255)); QCOMPARE(decoded.constScanLine(0)[1],uchar(255)); QCOMPARE(decoded.constScanLine(0)[3],uchar(0));
    }
    void splitChannelsIsUndoable() {
        EditorSession s; s.insertImage(solid(QColor(200,80,20,255)), "RGBA"); const auto before = *s.document(); QVERIFY(s.splitChannels());
        QCOMPARE(s.document()->layers.size(),5); QCOMPARE(s.document()->layers.last().name,QString("Red")); QCOMPARE(s.activeLayer()->image.pixelColor(4,4),QColor(200,200,200));
        s.undo(); QCOMPARE(s.document()->layers,before.layers);
    }
    void lineStylesPersistAndCrispDots() {
        QTemporaryDir dir; EditorSession s; s.createDocument(40,20);
        QVERIFY(s.addShape(ShapeKind::Line,{2,5,25,1},Qt::red,Qt::red,1,0,QPointF(2,5),QPointF(27,5),false,"Dotted","Square"));
        QCOMPARE(s.document()->formatVersion,15); const auto image=s.activeLayer()->image; bool opaque=false, clear=false;
        for(int x=0;x<image.width();++x) { opaque |= image.pixelColor(x,0).alpha()==255; clear |= image.pixelColor(x,0).alpha()==0; } QVERIFY(opaque); QVERIFY(clear);
        const QString path=dir.filePath("line.comp"); ProjectWriter::save(*s.document(),path); const auto doc=ProjectReader::load(path);
        QCOMPARE(doc.layers.last().shapeStyle->lineStyle.value(),QString("Dotted")); QCOMPARE(doc.layers.last().shapeStyle->lineCap.value(),QString("Square"));
    }
    void pressureChangesCoverageWithoutRepaintingEarlierDabs() {
        EditorSession s; s.createDocument(100,50,true); s.setPenInput(.2,true,true); QVERIFY(s.beginBrushStroke({15,25},Qt::white,20,1,1));
        const int low=s.activeLayer()->image.pixelColor(2,2).alpha(); Q_UNUSED(low);
        s.setPenInput(1,true,true); s.continueBrushStroke({70,25}); QVERIFY(s.endBrushStroke());
        const auto rendered=LayerRenderer::flattened(*s.document());
        QVERIFY(rendered.pixelColor(15,25).alpha()<100); QVERIFY(rendered.pixelColor(70,25).alpha()>240);
        QCOMPARE(rendered.pixelColor(15,18).alpha(),0); QVERIFY(rendered.pixelColor(70,18).alpha()>200);
        s.undo(); QVERIFY(s.activeLayer()->image.isNull());
    }
    void pdfEveryPageAtPrintResolutionAndCanvasFit() {
        QTemporaryDir dir; const QString path=dir.filePath("two.pdf");
        { QPdfWriter pdf(path); pdf.setResolution(72); pdf.setPageSize(QPageSize(QSizeF(72,36),QPageSize::Point)); pdf.setPageMargins({0,0,0,0}); QPainter p(&pdf); p.fillRect(0,0,72,36,Qt::red); pdf.newPage(); p.fillRect(0,0,72,36,Qt::blue); }
        QString error; const auto pages=PdfImporter::read(path,{},&error); QVERIFY2(pages.size()==2,qPrintable(error)); QCOMPARE(pages[0].size(),QSize(300,150));
        EditorSession s; QVERIFY(s.insertPdf(path)); QCOMPARE(s.document()->resolution,300.0); QCOMPARE(s.document()->layers.size(),2);
        const auto fit=PdfImporter::read(path,{100,100},&error); QCOMPARE(fit[0].size(),QSize(100,50));
        QVERIFY(PdfImporter::read(path,{},&error,10).isEmpty());
    }
    void artboardsKeepTheirOwnLayersAndPlacement() {
        EditorSession s; s.createDocument(400,200); Layer board; board.id=QUuid::createUuid();board.name="Board";board.group=true;board.transform.size={400,200};
        Layer child;child.id=QUuid::createUuid();child.name="Child";child.parentId=board.id;child.transform.origin={110,30};child.transform.size={32,32};child.image=solid(Qt::red);
        PSDImportResult result;result.document=*s.document();result.document.layers={board,child};result.artboardBounds.insert(board.id,{100,20,80,60});
        const auto boards=result.artboards();QCOMPARE(boards.size(),1);QCOMPARE(boards[0].second.canvasSize,QSize(80,60));QCOMPARE(boards[0].second.layers[0].transform.origin,QPointF(10,10));QVERIFY(!boards[0].second.layers[0].parentId);
    }
    void layerSearchPreservesHierarchyAndFindsHiddenChildren() {
        EditorSession s;s.createDocument(50,50);s.insertImage(solid(Qt::red),"Needle");s.groupSelectedLayers();
        LayerListModel model;model.setDocument(s.document());const auto folder=s.activeLayer()->id;model.toggleExpanded(folder);
        model.setFilter("Needle","All");QCOMPARE(model.rowCount(),2);
        model.setFilter("no match","All");QCOMPARE(model.rowCount(),0);
        model.setFilter("","Groups");QCOMPARE(model.rowCount(),2);
    }
    void selectedFilterFollowsTheCurrentSelection() {
        MainWindow w; w.session().insertImage(solid(Qt::red), "First"); const auto first=w.session().activeLayer()->id;
        w.session().insertImage(solid(Qt::blue), "Second"); const auto second=w.session().activeLayer()->id; w.syncDocumentViews();
        w.findChild<QComboBox *>("layerTypeFilter")->setCurrentText("Selected");
        QCOMPARE(w.layerModel()->rowCount(),1); QCOMPARE(*w.layerModel()->layerId(w.layerModel()->index(0,0)), second);
        w.session().selectLayer(first); w.syncDocumentViews(false); QCOMPARE(w.layerModel()->rowCount(),1);
        QCOMPARE(*w.layerModel()->layerId(w.layerModel()->index(0,0)), first); w.session().markSaved();
    }
    void nonLatinToolKeysUsePhysicalLatinPosition() {
        QKeyEvent key(QEvent::KeyPress,0x0418,Qt::NoModifier,56,0,0,QString::fromUtf8("и"));
        QCOMPARE(ShortcutManager::sequenceFromKeyEvent(&key),QKeySequence(Qt::Key_B));
        QKeyEvent latin(QEvent::KeyPress,Qt::Key_Z,Qt::NoModifier,56,0,0,"z"); QCOMPARE(ShortcutManager::sequenceFromKeyEvent(&latin),QKeySequence(Qt::Key_Z));
    }
    void adaptiveToolRailFitsShortAndTallWindows() {
        MainWindow w; w.resize(1100, 550); w.show(); QTest::qWait(50);
        auto *host = w.findChild<QWidget *>("toolRailTools"); QVERIFY(host);
        QCOMPARE(host->property("columns").toInt(), 2);
        w.resize(1100, 1000); QTest::qWait(50);
        QCOMPARE(host->property("columns").toInt(), 1);
    }
    void tabletEventsReachTheBrushAndRevealPressureControls() {
        MainWindow w; w.resize(1100,800); w.show(); w.session().createDocument(100,100,true); w.syncDocumentViews();
        w.canvas()->setTool(CanvasWidget::Tool::Brush); QTest::qWait(50);
        auto *size = w.findChild<QCheckBox *>("penSize"); QVERIFY(size); QVERIFY(!size->isVisible());
        QPointingDevice pen("Test pen", 1, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        const QPointF pos = w.canvas()->canvasRect().center();
        QTabletEvent press(QEvent::TabletPress, &pen, pos, w.canvas()->mapToGlobal(pos), .25, 0, 0, 0, 0, 0,
            Qt::NoModifier, Qt::LeftButton, Qt::LeftButton);
        QCoreApplication::sendEvent(w.canvas(), &press);
        QTabletEvent release(QEvent::TabletRelease, &pen, pos, w.canvas()->mapToGlobal(pos), 0, 0, 0, 0, 0, 0,
            Qt::NoModifier, Qt::LeftButton, Qt::NoButton);
        QCoreApplication::sendEvent(w.canvas(), &release);
        QVERIFY(press.isAccepted()); QVERIFY(size->isVisible()); QVERIFY(!w.session().activeLayer()->image.isNull());
        w.session().markSaved();
    }
    void filterLayerPreviewCancelAndCommit() {
        MainWindow w; w.session().insertImage(solid(Qt::white), "Base"); QVERIFY(w.session().addAdjustment("RGB Split")); w.syncDocumentViews();
        const auto original = w.session().activeLayer()->adjustment; const int count = w.session().history().undoCount();
        QTimer::singleShot(0, &w, [&] { auto *d=w.findChild<QDialog *>("filterLayerDialog"); QVERIFY(d); d->findChild<QDoubleSpinBox *>("rgbSplitDistance")->setValue(35); d->reject(); });
        w.findChild<QAction *>("commandEditAdjustment")->trigger(); QCOMPARE(w.session().activeLayer()->adjustment, original); QCOMPARE(w.session().history().undoCount(), count);
        QTimer::singleShot(0, &w, [&] { auto *d=w.findChild<QDialog *>("filterLayerDialog"); QVERIFY(d); d->findChild<QDoubleSpinBox *>("rgbSplitDistance")->setValue(35); d->accept(); });
        w.findChild<QAction *>("commandEditAdjustment")->trigger(); QCOMPARE(w.session().activeLayer()->adjustment.value("filterLayerSettings").toObject().value("rgbSplitDistance").toDouble(), 35.0);
        w.session().undo(); QCOMPARE(w.session().activeLayer()->adjustment, original); w.session().markSaved();
    }
    void fontPopupHasFixedSearchAndFiltersFaces() {
        MainWindow w; w.show(); w.session().createDocument(100,100,true); w.syncDocumentViews(); w.canvas()->setTool(CanvasWidget::Tool::Text);
        auto *combo=w.findChild<QFontComboBox *>("textFont"); QVERIFY(combo); combo->showPopup(); QCoreApplication::processEvents();
        auto *search=w.findChild<QLineEdit *>("fontSearch"); auto *list=w.findChild<QListWidget *>("fontSearchResults"); QVERIFY(search && list); QVERIFY(list->count()>0);
        search->setText("font that does not exist 055"); for (int i=0;i<list->count();++i) QVERIFY(list->item(i)->isHidden());
        search->clear(); QVERIFY(!list->item(0)->isHidden());
        const QString face = list->item(0)->data(Qt::UserRole).toString(); emit list->itemClicked(list->item(0));
        emit w.canvas()->textBoxRequested(QRectF(10,10,20,20), false); QVERIFY(w.inlineTextEditor());
        QCOMPARE(w.inlineTextEditor()->fallbackFace, face); w.inlineTextEditor()->finish(false); w.session().markSaved();
    }
    void timelineAndSearchControlsWorkInWindow() {
        QTemporaryDir dir; MainWindow w;w.resize(1100,700);w.show();QVERIFY(w.openProject(fixture(dir)));QCoreApplication::processEvents();
        auto *frames=w.findChild<QListWidget *>("gifFrames");QVERIFY(frames);QCOMPARE(frames->count(),3);frames->setCurrentRow(2);QCOMPARE(w.session().activeLayer()->gif->selected,2);
        auto *duplicate=w.findChild<QPushButton *>("gifDuplicate");QVERIFY(duplicate);duplicate->click();QCOMPARE(w.session().activeLayer()->gif->frames.size(),4);
        auto *search=w.findChild<QLineEdit *>("layerSearch");QVERIFY(search);search->setText("not here");QCOMPARE(w.layerModel()->rowCount(),0);search->clear();QVERIFY(w.layerModel()->rowCount()>0);
        w.session().markSaved();
    }
};
QTEST_MAIN(TestUpstream155)
#include "TestUpstream155.moc"
