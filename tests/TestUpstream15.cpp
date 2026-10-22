// Regression cases from Compositor 1.4.6–1.5.3, through 365b574.
#include "core/CanvasUnits.h"
#include "core/EditorSession.h"
#include "io/CMYK.h"
#include "io/LensCorrection.h"
#include "io/HdrImage.h"
#include "io/ImageExporter.h"
#include "io/ImageImporter.h"
#include "io/PrintExporter.h"
#include "io/PSDReader.h"
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTabBar>
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "rendering/DeepImage.h"
#include "rendering/LayerEffectsRenderer.h"
#include "rendering/LayerRenderer.h"
#include "rendering/NavigatorGeometry.h"
#include "rendering/ShapePath.h"
#include "ui/CameraRawDialog.h"
#include "ui/CanvasWidget.h"
#include "ui/MainWindow.h"
#include "ui/InlineTextEditor.h"
#include "ui/ShortcutManager.h"
#include <QtTest>
#include <QAction>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QDir>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QSettings>
#include <QLayout>
#include <set>

using namespace compositor;
class TestUpstream15 : public QObject
{
    Q_OBJECT
    static QImage solid(int w, int h, QColor color)
    {
        QImage i(w, h, QImage::Format_RGBA8888_Premultiplied);
        i.fill(color);
        return i;
    }
    static QImage gradient16(int w = 512, int h = 8)
    {
        QImage image(w, h, QImage::Format_RGBA64_Premultiplied);
        for (int y = 0; y < h; ++y) {
            auto *p = reinterpret_cast<QRgba64 *>(image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const quint16 v = quint16(30000 + x);
                p[x] = QRgba64::fromRgba64(v, v, v, 65535);
            }
        }
        return image;
    }
    static void version(const QString &path, int value)
    {
        QFile f(path + "/manifest.json");
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto obj = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        obj["version"] = value;
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(obj).toJson());
    }
  private slots:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName("compositor-tests");
        QCoreApplication::setApplicationName("upstream15");
    }
    void floatingSelectionAndClipboardKeepNativePixels()
    {
        EditorSession s; s.createDocument(600, 40);
        const QImage deep = gradient16(512, 8);
        QVERIFY(s.insertPixelLayer(deep, QPointF(20, 10), "Deep", "Paste"));
        QVERIFY(DeepImage::isDeep(s.activeLayer()->image));
        s.setRectangularSelection(QRect(84, 10, 128, 8));
        const auto copied = s.copiedPixels(false);
        QVERIFY(copied); QCOMPARE(copied->second, QPoint(84, 10));
        const QImage pixels = DeepImage::canonical(copied->first);
        const auto *copy = reinterpret_cast<const QRgba64 *>(pixels.constScanLine(0));
        QCOMPARE(copy[0].red(), quint16(30064)); QCOMPARE(copy[127].red(), quint16(30191));
        const QUuid source = s.activeLayer()->id;
        QVERIFY(s.beginSelectionTransform());
        QVERIFY(DeepImage::isDeep(s.activeLayer()->image));
        s.activeLayer()->transform.origin += QPointF(0, 12);
        QVERIFY(s.commitSelectionTransform());
        QCOMPARE(s.activeLayer()->id, source);
        const QImage moved = DeepImage::canonical(s.activeLayer()->image);
        const auto *row = reinterpret_cast<const QRgba64 *>(moved.constScanLine(12));
        QCOMPARE(row[64].red(), quint16(30064)); QCOMPARE(row[191].red(), quint16(30191));
        QCOMPARE(s.selectionBounds(), std::optional<QRect>(QRect(84,22,128,8)));
        s.undo(); QCOMPARE(s.activeLayer()->image, deep);
    }
    void blankDeepGradientAndPartialGradientKeepPrecision()
    {
        EditorSession s; s.createDocument(1024,16); s.addBlankLayer();
        QVERIFY(s.convertDepth(true)); QVERIFY(DeepImage::isDeep(s.activeLayer()->image));
        QVERIFY(s.applyGradient(QPointF(0,8),QPointF(1024,8),Qt::black,Qt::white,false,false,false,1));
        const QImage gradient = s.activeLayer()->image.convertToFormat(QImage::Format_RGBA64_Premultiplied);
        std::set<quint16> values;
        const auto *row = reinterpret_cast<const QRgba64 *>(gradient.constScanLine(8));
        for(int x=0;x<1024;++x) values.insert(row[x].red());
        QVERIFY2(values.size()>900,"A 16-bit gradient was quantized before compositing");
        s.setRectangularSelection(QRect(0,0,32,16));
        QVERIFY(s.applyGradient(QPointF(0,8),QPointF(32,8),Qt::red,Qt::blue,false,false,false,.5));
        const QImage edited = s.activeLayer()->image.convertToFormat(gradient.format());
        QCOMPARE(reinterpret_cast<const QRgba64 *>(edited.constScanLine(8))[900],row[900]);
        s.undo(); QCOMPARE(s.activeLayer()->image,gradient);
        s.undo(); s.undo(); s.undo(); QVERIFY(s.activeLayer()->image.isNull());
    }
    void blurStrokePreviewsAndCommitsTheSameNativePixels()
    {
        for(bool deep : {false,true}) {
            EditorSession s; s.createDocument(128,32);
            QImage stripes=solid(128,32,Qt::black);
            for(int x=0;x<128;x+=4) for(int y=0;y<32;++y) stripes.setPixelColor(x,y,Qt::white);
            if(deep) stripes=stripes.convertToFormat(QImage::Format_RGBA64_Premultiplied);
            s.insertImage(stripes,"Stripes");
            QVERIFY(s.beginBlurStroke(QPointF(40,16),20,1,1,3));
            s.continueBrushStroke(QPointF(70,16));
            const QImage preview=s.activeLayer()->image;
            QVERIFY(preview!=stripes); QCOMPARE(DeepImage::isDeep(preview),deep);
            QVERIFY(s.endBrushStroke()); QCOMPARE(s.activeLayer()->image,preview);
            s.undo(); QCOMPARE(s.activeLayer()->image,stripes);
            QVERIFY(s.beginBlurStroke(QPointF(40,16),20,1,1,3));
            QVERIFY(s.cancelBrushStroke()); QCOMPARE(s.activeLayer()->image,stripes);
        }
    }
    void lensProfilesCorrectGeometryAtNativeDepth()
    {
        LensMetadata metadata{"Canon","Canon EOS 550D","Canon","Canon EF-S 18-55mm f/3.5-5.6 IS II",18,3.5,1000};
        QVERIFY2(!LensCorrection::profileName(metadata).isEmpty(),"Install the Lensfun version_1 profile database");
        QImage image=gradient16(128,80);
        const QImage corrected=LensCorrection::apply(image,metadata);
        QCOMPARE(corrected.size(),image.size()); QCOMPARE(corrected.format(),image.format());
        QVERIFY(corrected!=image);
        std::atomic<bool> cancelled{true}; QVERIFY(LensCorrection::apply(image,metadata,&cancelled).isNull());
        metadata.lensModel="Unknown lens without a profile";
        QVERIFY(LensCorrection::profileName(metadata).isEmpty()); QCOMPARE(LensCorrection::apply(image,metadata),image);
    }
    void gradingHasThreeWayAndIndividualPages()
    {
        EditorSession s; s.createDocument(8,8,true);
        CameraRawDialog dialog(nullptr,s,s.activeLayer()->id,[]{});
        auto *page=dialog.findChild<QComboBox *>("cameraRawGradingPage"); QVERIFY(page); QCOMPARE(page->count(),5);
        QCOMPARE(page->currentIndex(),0);
        auto *global=dialog.findChild<QWidget *>("gradeGlobalColumn");
        auto *shadows=dialog.findChild<QWidget *>("gradeShadowsColumn");
        QVERIFY(global->isHidden()); QVERIFY(!shadows->isHidden());
        page->setCurrentIndex(4); QVERIFY(!global->isHidden()); QVERIFY(shadows->isHidden());
        auto *hue=dialog.findChild<QDoubleSpinBox *>("gradeGlobalHue"); QVERIFY(hue); hue->setValue(120);
        dialog.reject();
    }
    void exposureAndDitherRememberOnlyCommittedSettings()
    {
        MainWindow window; window.session().createDocument(32,16); window.session().insertImage(solid(32,16,QColor(120,100,80)),"Photo"); window.syncDocumentViews();
        auto *exposure=window.findChild<QAction *>("cmdExposure"); QVERIFY(exposure);
        const auto edit=[&](double expected,double value,bool commit){
            QTimer::singleShot(0,&window,[&,expected,value,commit]{
                for(auto *dialog:window.findChildren<QDialog *>()) if(dialog->isVisible()&&dialog->windowTitle()=="Exposure") {
                    auto *field=dialog->findChild<QDoubleSpinBox *>("exposureStops"); QVERIFY(field);
                    QCOMPARE(field->value(),expected); field->setValue(value);
                    if(commit)dialog->accept();else dialog->reject();
                }
            });
            exposure->trigger();
        };
        edit(0,1,true); edit(1,2,false); edit(1,1,false);
        QCOMPARE(window.session().filterMemory.value("exposure.stops").toDouble(),1.0);
        auto *dither=window.findChild<QAction *>("cmdDither"); QVERIFY(dither);
        QTimer::singleShot(0,&window,[&]{
            for(auto *dialog:window.findChildren<QDialog *>()) if(dialog->isVisible()&&dialog->windowTitle()=="Dither") {
                auto *style=dialog->findChild<QComboBox *>("ditherStyle"); QVERIFY(style);
                QCOMPARE(style->findData(int(DitherStyle::Scanlines)),-1);
                style->setCurrentIndex(style->findData(int(DitherStyle::Bayer4)));
                dialog->findChild<QDoubleSpinBox *>("ditherPixelSize")->setValue(5);dialog->accept();
            }
        });
        dither->trigger(); QCOMPARE(window.session().lastDither.style,DitherStyle::Bayer4);
        QCOMPARE(window.session().lastDither.pixelSize,5.0);
    }
    void rotatedAndFlippedTextEditsInPlace()
    {
        MainWindow window; window.resize(1100,800); window.show();
        auto &s=window.session(); s.createDocument(500,300);
        TextStyle style; style.content="Rotated"; style.fontName="DejaVuSans"; style.fontSize=30;
        QVERIFY(s.addText(style,QRectF(100,80,200,120),false,false,false,true));
        s.activeLayer()->transform.rotation=27; s.activeLayer()->transform.flipX=true;
        const QUuid id=s.activeLayer()->id;
        window.syncDocumentViews(); window.canvas()->setTool(CanvasWidget::Tool::Text);
        emit window.canvas()->textLayerEditRequested(id);
        auto *editor=window.inlineTextEditor(); QVERIFY(editor);
        auto *view=window.canvas()->findChild<QGraphicsView *>("transformedTextView"); QVERIFY(view); QVERIFY(view->isVisible());
        auto *proxy=editor->graphicsProxyWidget(); QVERIFY(proxy);
        QVERIFY(std::abs(proxy->transform().m12())>.1); QVERIFY(proxy->transform().determinant()<0);
        QTest::keyClicks(editor," text");
        QDir().mkpath("/tmp/compositor-port-ui");
        window.grab().save("/tmp/compositor-port-ui/rotated-text.png");
        editor->finish(true);
        QCOMPARE(s.activeLayer()->text->content,QString("Rotated text"));
        QCOMPARE(s.activeLayer()->transform.rotation,27.0); QVERIFY(s.activeLayer()->transform.flipX);
        QVERIFY(!window.inlineTextEditor());
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
        QVERIFY(!window.canvas()->findChild<QGraphicsView *>("transformedTextView"));
    }
    void hdrMotionBlurSpreadsHighlightEnergyAndTiffKeepsSixteenBits()
    {
        QImage hdr(65,9,QImage::Format_RGBA32FPx4_Premultiplied); hdr.fill(Qt::transparent);
        auto *row=reinterpret_cast<float *>(hdr.scanLine(4)); row[32*4]=8; row[32*4+3]=1;
        const QImage blurred=RasterOperations::motionBlur(hdr,0,8).convertToFormat(hdr.format());
        double sum=0;
        const auto *out=reinterpret_cast<const float *>(blurred.constScanLine(4));
        for(int x=0;x<65;++x)sum+=out[x*4];
        QVERIFY(std::abs(sum-8)<.002); QVERIFY(out[29*4]>.8); QVERIFY(out[32*4]<1);
        const QImage deep=gradient16(); const auto tiff=PrintExporter::tiff(deep,300); QVERIFY(tiff);
        const QImage read=QImage::fromData(*tiff,"TIFF").convertToFormat(deep.format()); QVERIFY(!read.isNull());
        QCOMPARE(reinterpret_cast<const QRgba64 *>(read.constScanLine(0))[201].red(),quint16(30201));
    }
    void progressiveRadialAndInvertedMatchUpstreamBehavior()
    {
        QImage stripes=solid(200,200,Qt::white);
        for(int y=0;y<200;++y)for(int x=0;x<200;x+=4){stripes.setPixelColor(x,y,Qt::black);stripes.setPixelColor(x+1,y,Qt::black);}
        const auto contrast=[](const QImage &image,int x,int y){int lo=255,hi=0;for(int i=x;i<x+8;++i){int v=image.pixelColor(i,y).red();lo=std::min(lo,v);hi=std::max(hi,v);}return hi-lo;};
        ProgressiveBlurSettings settings;settings.to=6;settings.radial=true;settings.start={100,100};settings.end={160,100};
        const QImage radial=settings.apply(stripes); QVERIFY(contrast(radial,96,100)>230); QVERIFY(contrast(radial,4,4)<40);
        settings.inverted=true; const QImage inverted=settings.apply(stripes);
        QVERIFY(contrast(inverted,96,100)<40); QVERIFY(contrast(inverted,4,4)>230);
        settings.inverted=false;settings.radial=false;settings.from=1.5;settings.start={100,20};settings.end={100,180};
        const QImage both=settings.apply(stripes);QVERIFY(contrast(both,96,10)<230);QVERIFY(contrast(both,96,10)>contrast(both,96,190));
    }
    void smallStrokeOnLargeBlankCanvasAllocatesOnlyItsExtent()
    {
        EditorSession s; s.createDocument(10000,10000,true);
        QVERIFY(s.beginBrushStroke(QPointF(5000,5000),Qt::red,16,1,1));
        QVERIFY(s.activeLayer()->image.width()<64); QVERIFY(s.activeLayer()->image.sizeInBytes()<64*64*4);
        s.continueBrushStroke(QPointF(5080,5020));
        QVERIFY(s.activeLayer()->image.width()<128); QVERIFY(s.activeLayer()->image.height()<64);
        QVERIFY(s.endBrushStroke()); QVERIFY(s.activeLayer()->image.width()<128);
        s.undo(); QVERIFY(s.activeLayer()->image.isNull());
        QVERIFY(s.beginBrushStroke(QPointF(5000,5000),Qt::red,16,1,1));
        s.continueBrushStroke(QPointF(4960,4980)); QVERIFY(s.cancelBrushStroke());
        QVERIFY(s.activeLayer()->image.isNull());
    }
    void realHeifImportsAtItsNativeBitDepth()
    {
        QString error;
        const QImage eight=ImageImporter::read(QStringLiteral(FIXTURES_DIR)+"/generated-rgb8.heic",&error);
        QVERIFY2(!eight.isNull(),qPrintable(error)); QCOMPARE(eight.size(),QSize(64,32));
        const QImage ten=ImageImporter::read(QStringLiteral(FIXTURES_DIR)+"/generated-rgb10.hif",&error);
        QVERIFY2(!ten.isNull(),qPrintable(error)); QCOMPARE(ten.size(),QSize(512,32));
        QVERIFY(DeepImage::isDeep(ten));
        const QImage wide=ten.convertToFormat(QImage::Format_RGBA64);
        std::set<quint16> values;const auto *row=reinterpret_cast<const QRgba64 *>(wide.constScanLine(16));
        for(int x=0;x<512;++x)values.insert(row[x].red());
        QVERIFY(values.size()>400);
    }
    void closingWithAFilterCommitsBeforeTheSavePrompt()
    {
        MainWindow window; window.show(); auto &s=window.session();
        s.createDocument(32,16); s.insertImage(solid(32,16,QColor(100,80,60)),"Photo");
        window.syncDocumentViews(); s.markSaved();
        int prompts=0;
        window.setMessageDialogHook([&](const QString &,const QString &)->std::optional<QMessageBox::StandardButton>{
            ++prompts; return QMessageBox::Discard;
        });
        QTimer::singleShot(0,&window,[&]{
            for(auto *dialog:window.findChildren<QDialog *>()) if(dialog->isVisible()&&dialog->windowTitle()=="Exposure") {
                dialog->findChild<QDoubleSpinBox *>("exposureStops")->setValue(1);window.close();
            }
        });
        window.findChild<QAction *>("cmdExposure")->trigger();
        QTRY_COMPARE(prompts,1); QVERIFY(!window.isVisible());
        QCOMPARE(s.history().undoName(),QString("Exposure"));
    }
    void mergedDeepPixelsAndEmbeddedPreviewSurviveSaving()
    {
        EditorSession s; s.createDocument(512,8); s.insertImage(gradient16(),"Deep");
        s.addBlankLayer(); QVERIFY(s.beginBrushStroke(QPointF(10,4),Qt::red,4,1,1)); QVERIFY(s.endBrushStroke());
        QVERIFY(s.mergeLayers()); QVERIFY(DeepImage::isDeep(s.activeLayer()->image));
        QTemporaryDir dir; const QString path=dir.path()+"/merged.comp";
        ProjectWriter::save(*s.document(),path); const Document read=ProjectReader::load(path);
        const QImage image=read.layers.last().image.convertToFormat(QImage::Format_RGBA64_Premultiplied);
        QCOMPARE(reinterpret_cast<const QRgba64 *>(image.constScanLine(0))[400].red(),quint16(30400));
        const QImage preview(path+"/QuickLook/Preview.jpg"); QVERIFY(!preview.isNull()); QCOMPARE(preview.size(),QSize(512,8));
    }
    void layerDropIntoAnotherCanvasUsesThePointerAndKeepsDepth()
    {
        EditorSession source,target;source.createDocument(128,80);source.insertImage(gradient16(64,8),"Source");
        source.activeLayer()->transform.rotation=25;
        const QUuid id=source.activeLayer()->id;
        target.createDocument(300,200);target.insertImage(solid(300,200,Qt::white),"Target");
        QTemporaryDir dir;ProjectWriter::save(*source.document(),dir.path()+"/source.comp");ProjectWriter::save(*target.document(),dir.path()+"/target.comp");
        MainWindow window; window.resize(1100,800);window.show();
        QVERIFY(window.openProject(dir.path()+"/source.comp"));QVERIFY(window.openProject(dir.path()+"/target.comp"));
        window.canvas()->setZoom(1);QCoreApplication::processEvents();
        QMimeData mime;mime.setData("application/x-compositor-layers",id.toString(QUuid::WithoutBraces).toUtf8());
        const QPoint canvasPoint=window.canvas()->widgetRectForDocumentRect(QRectF(100,70,1,1)).topLeft().toPoint();
        const QPoint point=window.canvas()->mapTo(&window,canvasPoint);
        QDragEnterEvent enter(point,Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&window,&enter);QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(point),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&window,&drop);QVERIFY(drop.isAccepted());
        QCOMPARE(window.document()->layers.size(),2);const Layer *copied=window.session().activeLayer();
        QVERIFY(DeepImage::isDeep(copied->image));QCOMPARE(copied->transform.rotation,25.0);
        QVERIFY(QLineF(copied->transform.center(),QPointF(100,70)).length()<1);
        window.session().undo();window.syncDocumentViews();QCOMPARE(window.document()->layers.size(),1);
    }
    void canvasRotationPixelsAndUndo()
    {
        EditorSession s;
        s.createDocument(3, 2);
        QImage image = solid(3, 2, Qt::black);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 3; ++x)
                image.setPixelColor(x, y, QColor((y * 3 + x + 1) * 10, 0, 0));
        QVERIFY(s.insertImage(image, "Test"));
        s.activeLayer()->transform.sampling = Sampling::Nearest;
        QVERIFY(s.rotateCanvas(true));
        QCOMPARE(s.document()->canvasSize, QSize(2, 3));
        const QImage result = LayerRenderer::flattened(*s.document());
        QList<int> reds;
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 2; ++x)
                reds << qRed(result.pixel(x, y));
        QCOMPARE(reds, QList<int>({40, 10, 50, 20, 60, 30}));
        s.undo();
        QCOMPARE(s.document()->canvasSize, QSize(3, 2));
        QCOMPARE(LayerRenderer::flattened(*s.document()), image);
        s.redo();
        QVERIFY(s.rotateCanvas(false));
        QCOMPARE(LayerRenderer::flattened(*s.document()), image);
    }
    void canvasRotationGuidesSelectionAndPlacedMask()
    {
        EditorSession s;
        s.createDocument(100, 60, true);
        s.document()->guides = {{QUuid::createUuid(), CanvasGuide::Axis::Vertical, 30},
                                {QUuid::createUuid(), CanvasGuide::Axis::Horizontal, 10}};
        s.setRectangularSelection(QRect(0, 0, 20, 10));
        s.activeLayer()->mask = solid(2, 2, Qt::white).convertToFormat(QImage::Format_Grayscale8);
        s.activeLayer()->maskPlacement = s.activeLayer()->transform;
        const auto before = s.activeLayer()->maskPlacement;
        QVERIFY(s.rotateCanvas(true));
        QCOMPARE(s.document()->guides[0].axis, CanvasGuide::Axis::Horizontal);
        QCOMPARE(s.document()->guides[0].position, 30.0);
        QCOMPARE(s.document()->guides[1].position, 50.0);
        QCOMPARE(s.selectionBounds(), std::optional<QRect>(QRect(50, 0, 10, 20)));
        QVERIFY(s.activeLayer()->maskPlacement != before);
        s.undo();
        QCOMPARE(s.activeLayer()->maskPlacement, before);
    }
    void sixteenBitProjectRoundTripAndVersionGate()
    {
        EditorSession s;
        s.createDocument(512, 8);
        const QImage deep = gradient16();
        QVERIFY(s.insertImage(deep, "Deep"));
        QCOMPARE(s.document()->formatVersion, 12);
        QVERIFY(DeepImage::isDeep(s.activeLayer()->image));
        QTemporaryDir dir;
        const QString path = dir.path() + "/wide.comp";
        ProjectWriter::save(*s.document(), path);
        const auto read = ProjectReader::load(path);
        QCOMPARE(read.layers.last().image, deep);
        QCOMPARE(read.formatVersion, 12);
        Document invalid = *s.document();
        invalid.formatVersion = 11;
        QVERIFY_EXCEPTION_THROWN(ProjectWriter::save(invalid, dir.path() + "/old.comp"), ProjectWriteError);
        version(path, 11);
        QVERIFY_EXCEPTION_THROWN(ProjectReader::load(path), ProjectError);
        s.undo();
        QCOMPARE(s.document()->formatVersion, 9);
        s.redo();
        QCOMPARE(s.document()->formatVersion, 12);
    }
    void sixteenBitCanvasAndAdjustmentsKeepFineSteps()
    {
        EditorSession s;
        s.createDocument(512, 8);
        QVERIFY(s.insertImage(gradient16(), "Deep"));
        QImage canvas =
            LayerRenderer::flattened(*s.document()).convertToFormat(QImage::Format_RGBA64_Premultiplied);
        const auto *row = reinterpret_cast<const QRgba64 *>(canvas.constScanLine(0));
        QCOMPARE(row[0].red(), quint16(30000));
        QCOMPARE(row[1].red(), quint16(30001));
        LevelsSettings levels;
        levels.ranges[0].black = 80;
        levels.ranges[0].white = 180;
        QVERIFY(s.applyLevels(levels));
        QVERIFY(DeepImage::isDeep(s.activeLayer()->image));
        std::set<quint16> colors;
        const auto *p = reinterpret_cast<const QRgba64 *>(s.activeLayer()->image.constScanLine(0));
        for (int x = 0; x < 512; ++x)
            colors.insert(p[x].red());
        QVERIFY2(colors.size() > 250, "Levels rounded a 16-bit gradient to 8 bits");
        s.undo();
        QCOMPARE(s.activeLayer()->image, gradient16());
    }
    void paintKeepsUntouchedSixteenBitPixels()
    {
        EditorSession s;
        s.createDocument(512, 8);
        const QImage before = gradient16();
        s.insertImage(before, "Deep");
        QVERIFY(s.beginBrushStroke(QPointF(10, 4), Qt::red, 4, 1, 1));
        QVERIFY(s.endBrushStroke());
        QVERIFY(DeepImage::isDeep(s.activeLayer()->image));
        const auto *p = reinterpret_cast<const QRgba64 *>(s.activeLayer()->image.constScanLine(0));
        QCOMPARE(p[400].red(), quint16(30400));
        s.undo();
        QCOMPARE(s.activeLayer()->image, before);
    }
    void hdrProjectAndExportKeepHighlights()
    {
        QImage image(4, 3, QImage::Format_RGBA16FPx4_Premultiplied);
        image.fill(Qt::black);
        for (int y = 0; y < 3; ++y) {
            auto *p = reinterpret_cast<qfloat16 *>(image.scanLine(y));
            for (int x = 0; x < 4; ++x) {
                p[x * 4] = qfloat16(2.0f);
                p[x * 4 + 1] = qfloat16(.5f);
                p[x * 4 + 2] = qfloat16(.25f);
                p[x * 4 + 3] = qfloat16(1.0f);
            }
        }
        EditorSession s;
        s.createDocument(4, 3);
        s.insertImage(image, "HDR");
        QCOMPARE(s.document()->formatVersion, 13);
        QTemporaryDir dir;
        const QString path = dir.path() + "/hdr.comp";
        ProjectWriter::save(*s.document(), path);
        const auto read = ProjectReader::load(path);
        QVERIFY(DeepImage::isHDR(read.layers.last().image));
        const auto *p = reinterpret_cast<const qfloat16 *>(read.layers.last().image.constBits());
        QVERIFY(std::abs(float(p[0]) - 2) < .02);
        const QImage exposed = RasterOperations::exposure(image, -1, 0, 1);
        QVERIFY(DeepImage::isHDR(exposed));
        const auto *e = reinterpret_cast<const qfloat16 *>(exposed.constBits());
        QVERIFY(float(e[0]) > 1.0f);
        version(path, 12);
        QVERIFY_EXCEPTION_THROWN(ProjectReader::load(path), ProjectError);
    }
    void smoothCornersAndFormat14()
    {
        const QRectF rect(0, 0, 100, 70);
        QVERIFY(rectanglePath(rect, 15, true) != rectanglePath(rect, 15, false));
        QCOMPARE(rectanglePath(rect, 0, true), rectanglePath(rect, 0, false));
        QCOMPARE(rectanglePath(rect, 35, true), rectanglePath(rect, 35, false));
        EditorSession s;
        s.createDocument(200, 120);
        QVERIFY(s.addShape(ShapeKind::Rectangle, QRectF(20, 20, 100, 70), Qt::red, Qt::transparent, 0, 15,
                           std::nullopt, std::nullopt, true));
        QCOMPARE(s.document()->formatVersion, 14);
        QVERIFY(s.activeLayer()->shapeStyle->smoothing.value_or(false));
        QTemporaryDir dir;
        const QString path = dir.path() + "/smooth.comp";
        ProjectWriter::save(*s.document(), path);
        QVERIFY(ProjectReader::load(path).layers.last().shapeStyle->smoothing.value_or(false));
        Document invalid = *s.document();
        invalid.formatVersion = 13;
        QVERIFY_EXCEPTION_THROWN(ProjectWriter::save(invalid, dir.path() + "/old.comp"), ProjectWriteError);
        version(path, 13);
        QVERIFY_EXCEPTION_THROWN(ProjectReader::load(path), ProjectError);
        s.undo();
        QCOMPARE(s.document()->formatVersion, 9);
    }
    void translucentOverlayPreservesCoverageAndShadow()
    {
        LayerEffects effects;
        ColorOverlayEffect overlay;
        overlay.red = overlay.green = overlay.blue = 1;
        effects.colorOverlay = overlay;
        QImage source = solid(16, 16, QColor(0, 0, 0, 128));
        auto [result, inset] = LayerEffectsRenderer::render(source, {}, effects);
        Q_UNUSED(inset)
        QRgb p = result.pixel(result.width() / 2, result.height() / 2);
        QCOMPARE(qAlpha(p), 128);
        QCOMPARE(qRed(p), 128);
        QCOMPARE(qGreen(p), 128);
        ShadowEffect shadow;
        shadow.distance = shadow.blur = 0;
        shadow.red = 1;
        shadow.opacity = 1;
        effects.shadow = shadow;
        result = LayerEffectsRenderer::render(source, {}, effects).first;
        p = result.pixel(result.width() / 2, result.height() / 2);
        QVERIFY(std::abs(qAlpha(p) - 192) <= 2);
        QVERIFY(std::abs(qRed(p) - 192) <= 2);
        QVERIFY(std::abs(qGreen(p) - 128) <= 2);
    }
    void imageSizeScalesEffectsAndResolutionKeepsText()
    {
        EditorSession s;
        s.createDocument(100, 100);
        s.insertImage(solid(100, 100, Qt::red), "Effects");
        LayerEffects effects;
        effects.stroke = StrokeEffect{};
        effects.shadow = ShadowEffect{};
        s.activeLayer()->effects = effects;
        QVERIFY(s.resizeImage(QSize(50, 50), 72, Sampling::Nearest));
        QCOMPARE(s.activeLayer()->effects->stroke->size, 2.0);
        QCOMPARE(s.activeLayer()->effects->shadow->distance, 10.0);
        s.undo();
        QCOMPARE(s.activeLayer()->effects, std::optional<LayerEffects>(effects));
    }
    void scanlinesGoldensAndBlackLevel()
    {
        ScanlinesSettings settings;
        settings.lineSpacing = 8;
        QImage white = settings.apply(solid(16, 32, Qt::white));
        QCOMPARE(qRed(white.pixel(5, 3)), 255);
        QCOMPARE(qRed(white.pixel(5, 4)), 255);
        QVERIFY(qRed(white.pixel(5, 0)) < 40);
        settings.blackLevel = 40;
        const QImage black = settings.apply(solid(16, 32, Qt::black));
        QVERIFY(qRed(black.pixel(8, 4)) > 60);
        QVERIFY(qRed(black.pixel(8, 0)) < 10);
        settings.displace = 300;
        settings.thickness = 0;
        QCOMPARE(settings.normalized().displace, 100.0);
        QCOMPARE(settings.normalized().thickness, 5.0);
    }
    void progressiveBlurUndoAndLastFilterCancel()
    {
        QImage image = solid(64, 64, Qt::white);
        {
            QPainter p(&image);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)
                    if ((x + y) % 2)
                        p.fillRect(x, y, 1, 1, Qt::black);
        }
        ProgressiveBlurSettings settings;
        settings.from = 0;
        settings.to = 6;
        settings.start = {32, 16};
        settings.end = {32, 48};
        const QImage out = settings.apply(image);
        QCOMPARE(out.pixel(20, 0), image.pixel(20, 0));
        QVERIFY(std::abs(qRed(out.pixel(20, 60)) - 128) < 20);
        EditorSession s;
        s.createDocument(64, 64);
        s.insertImage(image, "Blur");
        QVERIFY(s.applyProgressiveBlur(settings));
        QVERIFY(s.hasLastFilter());
        const QImage expected = LayerRenderer::flattened(*s.document());
        s.undo();
        QCOMPARE(s.activeLayer()->image, image);
        QVERIFY(s.applyLastFilter());
        QCOMPARE(LayerRenderer::flattened(*s.document()), expected);
        s.undo();
        const Layer original = *s.activeLayer();
        s.beginEdit("Preview canceled");
        s.applyDither({});
        s.rollbackLayer(original.id, original);
        s.endEdit();
        QVERIFY(s.applyLastFilter());
        QCOMPARE(LayerRenderer::flattened(*s.document()), expected);
    }
    void progressiveBlurSpreadsBeyondTheLayer()
    {
        EditorSession session;
        session.createDocument(64, 64);
        session.insertImage(solid(16, 16, Qt::red), "Patch");
        ProgressiveBlurSettings settings;
        settings.to = 4;
        settings.start = QPointF(24, 24);
        settings.end = QPointF(40, 40);
        QVERIFY(session.applyProgressiveBlur(settings));
        QVERIFY(session.activeLayer()->image.width() > 16);
        const QImage image = LayerRenderer::flattened(*session.document());
        QVERIFY(qAlpha(image.pixel(41, 36)) > 0);
        session.undo();
        QCOMPARE(session.activeLayer()->image.size(), QSize(16, 16));
    }
    void gaussianPreservesAlphaAndDepthConversionUndo()
    {
        EditorSession s;
        s.createDocument(20, 20);
        QImage image = solid(20, 20, Qt::transparent);
        {
            QPainter p(&image);
            p.fillRect(6, 6, 8, 8, Qt::red);
        }
        s.insertImage(image, "Blur");
        QVERIFY(s.applyGaussianBlur(3, true));
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x)
                QCOMPARE(qAlpha(s.activeLayer()->image.pixel(x, y)), qAlpha(image.pixel(x, y)));
        QVERIFY(s.convertDepth(true));
        QCOMPARE(s.document()->formatVersion, 12);
        s.undo();
        QVERIFY(!DeepImage::isDeep(s.activeLayer()->image));
        s.redo();
        QVERIFY(s.convertDepth(false));
        QVERIFY(!DeepImage::isDeep(s.activeLayer()->image));
    }
    void cameraRawResetOnlyTouchesOneGroup()
    {
        CameraRawSettings settings;
        settings.exposure = 1;
        settings.tint = 25;
        settings.grading.shadows.saturation = 70;
        settings.geometry.scale = 30;
        const auto reset = settings.resetting(0);
        QCOMPARE(reset.exposure, 0.0);
        QCOMPARE(reset.tint, 25.0);
        QCOMPARE(reset.grading, settings.grading);
        QCOMPARE(reset.geometry, settings.geometry);
        EditorSession s;
        s.createDocument(16, 16);
        s.insertImage(solid(16, 16, Qt::gray), "Raw");
        CameraRawDialog dialog(nullptr, s, s.activeLayer()->id);
        dialog.setSettings(settings);
        auto *button = dialog.findChild<QPushButton *>("cameraRawSectionReset0");
        QVERIFY(button);
        button->click();
        QCOMPARE(dialog.settings(), reset);
        dialog.reject();
    }
    void physicalUnitsAndCanvasBackgroundUndo()
    {
        QCOMPARE(canvasPixels(8.5, CanvasUnit::Inches, 300), 2550);
        QCOMPARE(canvasPixels(21, CanvasUnit::Centimeters, 300), 2480);
        QCOMPARE(canvasPixels(297, CanvasUnit::Millimeters, 300), 3508);
        EditorSession s;
        s.createDocument(20, 10, true, 300, QColor(Qt::white));
        QCOMPARE(s.document()->resolution, 300.0);
        QCOMPARE(s.activeLayer()->image.pixelColor(0, 0), QColor(Qt::white));
        s.undo();
        QVERIFY(!s.document());
        s.redo();
        QCOMPARE(s.document()->resolution, 300.0);
    }
    void navigatorGeometryAndMiddleZoom()
    {
        NavigatorGeometry g{QSizeF(400, 200), QSizeF(160, 120)};
        QCOMPARE(g.imageRect(), QRectF(0, 20, 160, 80));
        QCOMPARE(g.documentPoint(QPointF(80, 60)), QPointF(200, 100));
        EditorSession s;
        s.createDocument(400, 200, true);
        CanvasWidget canvas;
        canvas.resize(800, 600);
        canvas.setDocument(s.document());
        canvas.show();
        QTest::qWait(10);
        canvas.actualPixels();
        QTest::mousePress(&canvas, Qt::MiddleButton, Qt::ControlModifier, QPoint(300, 300));
        QTest::mouseMove(&canvas, QPoint(400, 300));
        QTest::mouseRelease(&canvas, Qt::MiddleButton, Qt::ControlModifier, QPoint(400, 300));
        QVERIFY(canvas.zoom() > 1.5);
    }
    void cmykAndPrintExports()
    {
        const auto profiles = CMYK::installed();
        if (profiles.isEmpty())
            QSKIP("No system CMYK ICC profiles installed");
        const auto profile = profiles.first();
        const QImage image = solid(8, 6, QColor(200, 90, 30, 180));
        QVERIFY(CMYKProfile::fromData(profile.data));
        QVERIFY(!CMYKProfile::fromData("bad"));
        const auto inks = CMYK::inks(image, profile);
        QCOMPARE(inks.size(), 8 * 6 * 4);
        QCOMPARE(CMYK::fromInks(inks, image.size(), profile).size(), image.size());
        QCOMPARE(qAlpha(CMYK::proof(image, profile).pixel(0, 0)), 180);
        const auto pdf = PrintExporter::pdf(image, 72, profile);
        QVERIFY(pdf);
        QVERIFY(pdf->startsWith("%PDF-1.4"));
        QVERIFY(pdf->contains("/MediaBox [0 0 8.000000 6.000000]"));
        QVERIFY(pdf->contains("/N 4"));
        const auto jpeg = PrintExporter::jpeg(image, 300, 90, profile);
        QVERIFY(jpeg);
        QVERIFY(jpeg->contains("ICC_PROFILE"));
        QCOMPARE(QImage::fromData(*jpeg).size(), image.size());
        const auto tiff = PrintExporter::tiff(image, 300, profile);
        QVERIFY(tiff);
        QVERIFY(tiff->size() > 100);
    }
    void cmykPhotoshopMergedAlpha()
    {
        if (CMYK::installed().isEmpty())
            QSKIP("No CMYK ICC profile installed");
        for (bool rle : {false, true}) {
            QByteArray bytes;
            QDataStream out(&bytes, QIODevice::WriteOnly);
            out.setByteOrder(QDataStream::BigEndian);
            out.writeRawData("8BPS", 4);
            out << quint16(1);
            out.writeRawData("\0\0\0\0\0\0", 6);
            out << quint16(5) << quint32(1) << quint32(1) << quint16(8) << quint16(4);
            out << quint32(0) << quint32(0) << quint32(0) << quint16(rle ? 1 : 0);
            if (rle)
                for (int c = 0; c < 5; ++c)
                    out << quint16(2);
            for (int c = 0; c < 5; ++c) {
                if (rle)
                    out << quint8(0);
                out << quint8(c == 4 ? 128 : 255);
            }
            PSDImportResult result;
            QString error;
            QVERIFY2(PSDReader::read(bytes, result, &error), qPrintable(error));
            QVERIFY(!result.cmykProfile.isEmpty());
            QCOMPARE(qAlpha(result.document.layers.last().image.pixel(0, 0)), 128);
        }
    }
    void radianceImportAndSixteenBitResampling()
    {
        QTemporaryDir dir;
        QFile file(dir.path() + "/test.hdr");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n");
        const char samples[] = {char(128), char(64), char(32), char(130),
                                char(128), char(64), char(32), char(130)};
        file.write(samples, 8);
        file.close();
        const QImage hdr = ImageImporter::read(file.fileName());
        QVERIFY(!hdr.isNull());
        QVERIFY(DeepImage::isHDR(hdr));
        QVERIFY(float(reinterpret_cast<const qfloat16 *>(hdr.constBits())[0]) > 1);
        EditorSession session;
        session.createDocument(512, 8);
        session.insertImage(gradient16(), "Deep");
        QVERIFY(session.resizeImage(QSize(256, 4), 72, Sampling::Nearest));
        QVERIFY(DeepImage::isDeep(session.activeLayer()->image));
        const auto *p = reinterpret_cast<const QRgba64 *>(session.activeLayer()->image.constBits());
        QVERIFY(p[1].red() - p[0].red() > 0 && p[1].red() - p[0].red() < 10);
        session.undo();
        QCOMPARE(session.activeLayer()->image, gradient16());
    }
    void blurAtCanvasEdgeStaysOpaque()
    {
        EditorSession s;
        s.createDocument(32, 32);
        s.insertImage(solid(32, 32, Qt::red), "Solid");
        QVERIFY(s.applyGaussianBlur(5));
        const QImage shown = LayerRenderer::flattened(*s.document());
        QCOMPARE(shown.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(shown.pixelColor(31, 31), QColor(Qt::red));
    }
    void commandPaletteExportAndNewCanvasControls()
    {
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        auto &session = window.session();
        session.createDocument(100, 80);
        session.insertImage(solid(100, 80, Qt::blue), "Blue");
        window.syncDocumentViews();
        QDir().mkpath("/tmp/compositor-port-ui");
        auto *search = window.findChild<QAction *>("cmdSearchCommands");
        QVERIFY(search);
        bool used = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *field = dialog->findChild<QLineEdit *>("commandSearch");
            QVERIFY(field);
            field->setText("rotate canvas clockwise");
            dialog->grab().save("/tmp/compositor-port-ui/commands.png");
            used = true;
            QTest::keyClick(field, Qt::Key_Return);
        });
        search->trigger();
        QVERIFY(used);
        QCOMPARE(session.document()->canvasSize, QSize(80, 100));
        auto *exportAction = window.findChild<QAction *>("commandExportAs");
        QVERIFY(exportAction);
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto *color = dialog->findChild<QComboBox *>("exportColor");
            QVERIFY(color);
            QCOMPARE(color->currentIndex(), 0);
            dialog->grab().save("/tmp/compositor-port-ui/export.png");
            dialog->reject();
        });
        exportAction->trigger();
        session.setDocument({});
        window.syncDocumentViews();
        auto *unit = window.findChild<QComboBox *>("newCanvasUnit");
        auto *dpi = window.findChild<QComboBox *>("newCanvasDpi");
        QVERIFY(unit && dpi);
        dpi->setCurrentIndex(1);
        unit->setCurrentIndex(1);
        window.findChild<QDoubleSpinBox *>("newCanvasWidth")->setValue(1);
        window.findChild<QDoubleSpinBox *>("newCanvasHeight")->setValue(.5);
        window.grab().save("/tmp/compositor-port-ui/new-canvas.png");
        window.findChild<QPushButton *>("createCanvas")->click();
        QCOMPARE(session.document()->canvasSize, QSize(300, 150));
        QCOMPARE(session.document()->resolution, 300.0);
    }
    void layersFooterAdjustmentBeforeEffects()
    {
        // 067fca4, f25effa: New adjustment layer, then Add layer effect.
        MainWindow window;
        auto *footer = window.findChild<QWidget *>("layerFooter");
        QVERIFY(footer && footer->layout());
        auto *adjustment = window.findChild<QToolButton *>("addAdjustmentButton");
        auto *effects = window.findChild<QToolButton *>("addEffectButton");
        QVERIFY(adjustment && effects);
        QVERIFY(footer->layout()->indexOf(adjustment) < footer->layout()->indexOf(effects));
        QCOMPARE(adjustment->toolTip(), QStringLiteral("New adjustment layer"));
        QCOMPARE(effects->toolTip(), QStringLiteral("Add layer effect"));
        QVERIFY(adjustment->menu());
    }
    void toggleFullscreenShowsTheCanvasAlone()
    {
        // ad4bddf, 720c121: F hides every panel and bar; Esc leaves once there's nothing to cancel.
        MainWindow window;
        window.resize(1000, 700);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto *fullscreen = window.findChild<QAction *>("cmdFullscreen");
        QVERIFY(fullscreen && fullscreen->isCheckable());
        window.syncDocumentViews();
        QVERIFY(!fullscreen->isEnabled());
        window.session().createDocument(100, 80);
        window.syncDocumentViews();
        QVERIFY(fullscreen->isEnabled());
        fullscreen->trigger();
        QVERIFY(window.canvas()->canvasOnly());
        QVERIFY(fullscreen->isChecked());
        for (const char *name : {"inspector", "toolRail", "transformBar", "bottomStatus", "tabBar"})
            QVERIFY2(!window.findChild<QWidget *>(name)->isVisible(), name);
        QVERIFY(window.canvas()->escapeHasNothingToCancel());
        QTest::keyClick(window.canvas(), Qt::Key_Escape);
        QVERIFY(!window.canvas()->canvasOnly());
        QVERIFY(!fullscreen->isChecked());
        QVERIFY(window.findChild<QWidget *>("inspector")->isVisible());
    }
    void commandPaletteRanksAndRunsToolModes()
    {
        // CommandPaletteSearch: abbreviations, whole words first; tools' modes by name; layer mask commands.
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        window.session().createDocument(100, 80);
        window.session().insertImage(solid(100, 80, Qt::blue), "Blue");
        window.syncDocumentViews();
        auto *search = window.findChild<QAction *>("cmdSearchCommands");
        QVERIFY(search);
        const auto run = [&](const QString &query, const std::function<void(QListWidget *)> &check, bool accept = true) {
            bool used = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                auto *field = dialog->findChild<QLineEdit *>("commandSearch");
                auto *list = dialog->findChild<QListWidget *>("commandResults");
                QVERIFY(field && list);
                field->setText(query);
                check(list);
                used = true;
                if (accept) QTest::keyClick(field, Qt::Key_Return);
                else dialog->reject();
            });
            search->trigger();
            QVERIFY(used);
        };
        // "lasso" is the Lasso, whole at a word's start and shortest, not letters spread through another title.
        run("lasso", [](QListWidget *list) {
            QCOMPARE(list->item(0)->text(), QStringLiteral("Tool › Lasso    L"));
            QVERIFY(list->item(1)->text().startsWith("Tool › Polygonal Lasso"));
        }, false);
        run("polygonal lasso", [](QListWidget *list) { QVERIFY(list->item(0)->text().startsWith("Tool › Polygonal Lasso")); });
        QCOMPARE(window.canvas()->tool(), CanvasWidget::Tool::Lasso);
        QVERIFY(window.canvas()->polygonalLasso());
        run("add layer mask", [](QListWidget *list) { QVERIFY(list->item(0)->text().startsWith("Layer › Add Layer Mask")); });
        QVERIFY(!window.session().activeLayer()->mask.isNull());
    }
    void exportAsOffersDepthAndExrOnlyWhenTheyKeepSomething()
    {
        // b9186f7, 905efc5: EXR only with an HDR layer; a 16-bit PNG only with a 16-bit layer, off by default.
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        auto &session = window.session();
        session.createDocument(64, 8);
        session.insertImage(solid(64, 8, Qt::red), "Flat");
        window.syncDocumentViews();
        auto *exportAs = window.findChild<QAction *>("commandExportAs");
        QVERIFY(exportAs);
        const auto inspect = [&](const std::function<void(QDialog *)> &check) {
            bool used = false;
            QTimer::singleShot(0, &window, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                check(dialog);
                used = true;
                dialog->reject();
            });
            exportAs->trigger();
            QVERIFY(used);
        };
        inspect([](QDialog *dialog) {
            auto *format = dialog->findChild<QComboBox *>("exportFormat");
            QCOMPARE(format->findText("OpenEXR"), -1);
            QCOMPARE(format->itemText(0), QStringLiteral("JPEG"));
            QVERIFY(format->findText("GIF") > format->findText("PDF"));
            format->setCurrentIndex(format->findText("PNG"));
            QApplication::processEvents();
            QVERIFY(!dialog->findChild<QCheckBox *>("exportSixteenBit")->isVisibleTo(dialog));
        });
        session.insertImage(gradient16(64, 8), "Deep");
        window.syncDocumentViews();
        inspect([](QDialog *dialog) {
            auto *format = dialog->findChild<QComboBox *>("exportFormat");
            format->setCurrentIndex(format->findText("PNG"));
            QTest::qWait(150);
            auto *sixteen = dialog->findChild<QCheckBox *>("exportSixteenBit");
            QVERIFY(sixteen->isVisibleTo(dialog));
            QVERIFY(!sixteen->isChecked());
            QCOMPARE(format->findText("OpenEXR"), -1);
        });
        QImage hdr(64, 8, QImage::Format_RGBA16FPx4_Premultiplied);
        hdr.fill(QColor(255, 128, 0));
        session.insertImage(hdr, "HDR");
        window.syncDocumentViews();
        inspect([](QDialog *dialog) { QVERIFY(dialog->findChild<QComboBox *>("exportFormat")->findText("OpenEXR") >= 0); });
    }
    void navigatorStartsOffAndIsRemembered()
    {
        QSettings().remove("tool/navigator.visible");
        {
            MainWindow window;
            auto *navigator = window.findChild<QAction *>("cmdNavigator");
            QVERIFY(navigator);
            QCOMPARE(navigator->text(), QStringLiteral("Navigator (300% and above)"));
            QVERIFY(!navigator->isChecked());
            navigator->setChecked(true);
        }
        MainWindow again;
        QVERIFY(again.findChild<QAction *>("cmdNavigator")->isChecked());
        QSettings().remove("tool/navigator.visible");
    }
    void webpImportAndClearShortcut()
    {
        QTemporaryDir dir;
        const QImage image = solid(8, 8, Qt::red);
        const QString path = dir.path() + "/test.webp";
        if (image.save(path, "WEBP"))
            QCOMPARE(ImageImporter::read(path).size(), image.size());
        ShortcutManager shortcuts;
        auto overrides = shortcuts.overrides();
        const QString id = "Menus:Copy";
        overrides[id] = QKeySequence();
        QVERIFY(shortcuts.validate(overrides).isEmpty());
        shortcuts.setOverrides(overrides);
        QVERIFY(shortcuts.shortcut(id).isEmpty());
    }
};
QTEST_MAIN(TestUpstream15)
#include "TestUpstream15.moc"
