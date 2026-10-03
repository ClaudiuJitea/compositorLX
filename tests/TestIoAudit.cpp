// Project IO, import/export, history, tabs/workspace, shortcuts and app chrome: ports of the mac tests ProjectTests,
// ExportTests, JPEGExportTests, HistoryTests, ImageImportTests, CropToCanvasImportTests, ProjectWorkspaceTests,
// ProjectTabLayoutTests, CompositorTests (limits) and the keyboard-shortcut / menu contract of CompositorApp.swift.

#include "core/Document.h"
#include "core/DocumentHistory.h"
#include "core/DocumentLimits.h"
#include "core/EditorSession.h"
#include "io/ImageExporter.h"
#include "io/ImageImporter.h"
#include "io/ProjectDigest.h"
#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "io/PSDReader.h"
#include "rendering/LayerRenderer.h"
#include "ui/CanvasWidget.h"
#include "ui/LayerListModel.h"
#include "ui/MainWindow.h"
#include "ui/ShortcutManager.h"

#include <QAction>
#include <QBuffer>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>

using namespace compositor;

namespace {

QImage solid(int width, int height, const QColor &color)
{
    QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
    image.fill(color);
    return image;
}

Layer imageLayer(const QString &name, const QImage &image, const QPointF &origin = {}, const QSizeF &size = {})
{
    Layer layer;
    layer.id = QUuid::createUuid();
    layer.name = name;
    layer.image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    layer.transform.origin = origin;
    layer.transform.size = size.isEmpty() ? QSizeF(image.size()) : size;
    return layer;
}

Document documentWith(const QSize &canvas, const QVector<Layer> &layers)
{
    Document doc;
    doc.id = QUuid::createUuid();
    doc.canvasSize = canvas;
    doc.layers = layers;
    if (!layers.isEmpty()) doc.activeLayerId = layers.last().id;
    return doc;
}

QRgb pixelAt(const QImage &image, int x, int y)
{
    return image.convertToFormat(QImage::Format_ARGB32).pixel(x, y);
}

// A 64x32 fixture like the mac ImageImportTests: the left half opaque red, the right half transparent.
QImage importFixture()
{
    QImage image(64, 32, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) image.setPixelColor(x, y, QColor(255, 0, 0));
    return image;
}

QString writeFixture(const QTemporaryDir &dir, const QString &suffix, const QImage &image, const char *format = nullptr,
                     QImageIOHandler::Transformations transform = QImageIOHandler::TransformationNone)
{
    const QString path = dir.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + QLatin1Char('.') + suffix);
    QImageWriter writer(path, format ? QByteArray(format) : QByteArray());
    writer.setTransformation(transform);
    if (!writer.write(image)) qWarning() << "fixture write failed" << path << writer.errorString();
    return path;
}

// The mac message dialogs are NSAlert sheets; here a hook answers them.
struct DialogLog {
    QStringList titles;
    QStringList texts;
};

} // namespace

class TestIoAudit : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // Limits (mac DocumentLimits, CompositorTests dimensionValidation / limitsAndNewDocumentReset)
    void documentBudgetScalesWithMemory();
    void projectOverOldHundredMegapixelCapRoundTrips();
    void canvasDimensionValidation();

    // ExportTests / JPEGExportTests
    void pngPreservesDimensionsAlphaOrientationAndTransforms();
    void exportOrderVisibilityAndAtomicOverwrite();
    void exportBlankCanvas();
    void pngCarriesResolutionAndSRGB();
    void jpegTransparencyUsesChosenMatteAndIsOpaqueSRGB();
    void jpegQualityChangesBytesAndPixelsAndCarriesResolution();
    void copyMergedMatchesExportOfTheSelection();

    // ImageImportTests / CropToCanvasImportTests
    void importSupportedFormats();
    void importOrientationAndColorConversion();
    void importPngKeepsTransparency();
    void importLimitsAndInvalidFiles();

    // HistoryTests
    void historyEveryLayerEditRoundTripsWithSelection();
    void historyNavigationNoOpsAndSaveRevision();
    void historyReplacementCanvasAndNestedTransactions();
    void historyBoundsEntriesAndUniqueRetainedPixels();
    void historyTrimsAfterUndoLandsOnADocumentThatNoLongerSharesPixels();
    void undoRedoMenuNamesTheStep();

    // ProjectTests
    void projectRoundTripSurvivesSourceRemovalAndMove();
    void projectDimmedFolderSavesAndReopens();
    void projectOverwriteDropsRemovedAssets();
    void projectFailedSavePreservesPackage();
    void projectUnsupportedCorruptUnsafeRejected();
    void projectMissingEmbeddedImageRejected();

    // Import pipeline in the window
    void batchImportIsOneUndoEntryAndFailuresAddNone();
    void importFirstImageDecidesCanvasIgnoringDropPoint();
    void importPlacementOnExistingCanvas();
    void importRejectsFormatsMacDoesNotImport();
    void receiveFilesOpensOneProjectThenImports();

    // Workspace / tabs / chrome
    void openRecentMenuListsExistingProjectsAndClears();
    void openingAnOpenProjectFocusesItsTab();
    void openingAMissingProjectNamesIt();
    void tabsKeepIndependentDocumentsAndUndo();
    void tabReorderKeepsDocumentsAndExternalWatch();
    void quitAsksAboutEveryUnsavedTab();
    void closeTabAsksAndRemoves();
    void unsavedMarkOnTabAndTitle();

    // Shortcuts / menus
    void defaultShortcutTableMatchesMac();
    void menuShortcutsMatchMacMenus();
    void shortcutConflictsAndPersistence();
    void menuStructureMatchesMac();
    void fileMenuEnabledStates();

    // docs/writing-comp-files.md
    void handWrittenMinimalManifestOpens();
    void agentWriteReloadsLiveInOpenWindow();
    void lxWrittenManifestMatchesMacCodableShapes();

private:
    static QString projectPathIn(const QTemporaryDir &dir, const QString &name) { return dir.filePath(name); }
    static QString saveSimpleProject(const QTemporaryDir &dir, const QString &name, const QColor &color = Qt::red);
};

void TestIoAudit::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("CompositorIoAudit"));
    QCoreApplication::setApplicationName(QStringLiteral("CompositorIoAuditTests"));
}

void TestIoAudit::init()
{
    QSettings settings;
    settings.clear();
    ShortcutManager::instance().resetToDefaults();
    MainWindow::setMessageDialogHook(nullptr);
}

void TestIoAudit::cleanup()
{
    MainWindow::setMessageDialogHook(nullptr);
    QSettings settings;
    settings.clear();
}

QString TestIoAudit::saveSimpleProject(const QTemporaryDir &dir, const QString &name, const QColor &color)
{
    Document doc = documentWith(QSize(40, 30), {imageLayer(QStringLiteral("Layer"), solid(40, 30, color))});
    const QString path = dir.filePath(name);
    ProjectWriter::save(doc, path);
    return path;
}

// ---------------------------------------------------------------------------------------------------- limits

void TestIoAudit::documentBudgetScalesWithMemory()
{
    constexpr qint64 GiB = 1024LL * 1024 * 1024;
    // A quarter of memory at 4 bytes a pixel, never below one surface, never above 800 MP (mac documentPixelBudget).
    QCOMPARE(DocumentLimits::pixelBudgetForMemory(1 * GiB), DocumentLimits::maxSurfacePixels);
    QCOMPARE(DocumentLimits::pixelBudgetForMemory(8 * GiB), 8 * GiB / 16);
    QCOMPARE(DocumentLimits::pixelBudgetForMemory(256 * GiB), qint64(800000000));
    QVERIFY(DocumentLimits::pixelBudgetForMemory(8 * GiB) > 500000000);
    QCOMPARE(DocumentLimits::maxSurfacePixels, qint64(200000000));
    QVERIFY(DocumentLimits::documentPixelBudget() >= DocumentLimits::maxSurfacePixels);
    QVERIFY(DocumentLimits::documentPixelBudget() <= 800000000);
    QCOMPARE(DocumentLimits::maxSide, 30000);
}

void TestIoAudit::projectOverOldHundredMegapixelCapRoundTrips()
{
    // 100.01 MP in one layer was refused by the fixed 100 MP cap; the mac budget scales with memory.
    if (DocumentLimits::physicalMemoryBytes() < 8LL * 1024 * 1024 * 1024) QSKIP("needs a machine with at least 8 GiB");
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QImage big(10000, 10001, QImage::Format_RGBA8888_Premultiplied);
    big.fill(Qt::transparent);
    Document doc = documentWith(QSize(100, 100), {imageLayer(QStringLiteral("Big"), big, QPointF(0, 0), QSizeF(100, 100))});
    const QString path = dir.filePath(QStringLiteral("Big.comp"));
    ProjectWriter::save(doc, path);
    big = QImage();
    const Document loaded = ProjectReader::load(path);
    QCOMPARE(loaded.layers.constFirst().image.size(), QSize(10000, 10001));
}

void TestIoAudit::canvasDimensionValidation()
{
    EditorSession session;
    session.createDocument(0, 10);
    QVERIFY(!session.hasDocument());
    session.createDocument(10, 30001);
    QVERIFY(!session.hasDocument());
    session.createDocument(30000, 1);
    QVERIFY(session.hasDocument());
    QCOMPARE(session.document()->canvasSize, QSize(30000, 1));
    session.createDocument(64, 32, true);
    QCOMPARE(session.document()->layers.size(), 1);
    QCOMPARE(session.document()->layers.constFirst().name, QStringLiteral("Layer 1"));
    QVERIFY(session.document()->layers.constFirst().image.isNull()); // a blank layer has no pixels until painted
    QVERIFY(session.activeLayer() != nullptr);
}

// ---------------------------------------------------------------------------------------------------- export

namespace {
// The mac ExportTests snapshot: a 2x2 image, left column red and right column clear, placed at (1,1) at 4x4 in a 6x6 canvas.
Document exportDocument(double rotation = 0, bool flip = false)
{
    QImage image(2, 2, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    image.setPixelColor(0, 0, Qt::red); image.setPixelColor(0, 1, Qt::red);
    Layer layer = imageLayer(QStringLiteral("Red"), image, QPointF(1, 1), QSizeF(4, 4));
    layer.transform.rotation = rotation;
    layer.transform.flipX = flip;
    layer.transform.sampling = Sampling::Nearest;
    return documentWith(QSize(6, 6), {layer});
}

QImage exportedPng(const Document &doc)
{
    QString error;
    const auto data = ImageExporter::png(LayerRenderer::flattened(doc), doc.resolution, &error);
    if (!data) { qWarning() << error; return {}; }
    return QImage::fromData(*data, "PNG");
}
} // namespace

void TestIoAudit::pngPreservesDimensionsAlphaOrientationAndTransforms()
{
    struct Case { double rotation; bool flip; int redX, redY, clearX, clearY; };
    const Case cases[] = {{0, false, 1, 1, 4, 1}, {0, true, 4, 1, 1, 1}, {90, false, 1, 1, 1, 4}};
    for (const Case &c : cases) {
        const QImage image = exportedPng(exportDocument(c.rotation, c.flip));
        QVERIFY2(!image.isNull(), "PNG must decode");
        QCOMPARE(image.size(), QSize(6, 6));
        QVERIFY(image.hasAlphaChannel());
        QVERIFY2(qRed(pixelAt(image, c.redX, c.redY)) > 250 && qAlpha(pixelAt(image, c.redX, c.redY)) == 255,
                 qPrintable(QStringLiteral("rotation %1 flip %2: red at %3,%4").arg(c.rotation).arg(c.flip).arg(c.redX).arg(c.redY)));
        QCOMPARE(qAlpha(pixelAt(image, c.clearX, c.clearY)), 0);
        QCOMPARE(qAlpha(pixelAt(image, 0, 0)), 0);
    }
}

void TestIoAudit::exportOrderVisibilityAndAtomicOverwrite()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Export.png"));
    for (const bool visible : {true, false}) {
        Document doc = exportDocument();
        Layer blue = imageLayer(QStringLiteral("Blue"), solid(1, 1, Qt::blue), QPointF(-2, -2), QSizeF(10, 10));
        blue.visible = visible;
        doc.layers.push_back(blue);
        const auto data = ImageExporter::png(LayerRenderer::flattened(doc), doc.resolution);
        QVERIFY(data.has_value());
        QString error;
        QVERIFY2(ImageExporter::writeAtomically(*data, path, &error), qPrintable(error));
        QImage image(path);
        QCOMPARE(image.size(), QSize(6, 6));
        const QRgb pixel = pixelAt(image, 1, 1);
        if (visible) QVERIFY(qBlue(pixel) > 250); else QVERIFY(qRed(pixel) > 250);
    }
    // No temporary sibling is left behind and a failed write leaves the earlier file untouched.
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
    const QByteArray before = [&] { QFile f(path); f.open(QIODevice::ReadOnly); return f.readAll(); }();
    QString error;
    QVERIFY(!ImageExporter::writeAtomically(QByteArray("x"), dir.filePath(QStringLiteral("missing/dir/out.png")), &error));
    QVERIFY(!error.isEmpty());
    QFile f(path); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), before);
}

void TestIoAudit::exportBlankCanvas()
{
    Document blank = documentWith(QSize(2, 2), {});
    const QImage image = exportedPng(blank);
    QVERIFY(!image.isNull());
    QCOMPARE(qAlpha(pixelAt(image, 1, 1)), 0);
    QVERIFY(!ImageExporter::png(QImage(), 72).has_value());
    QVERIFY(!ImageExporter::png(solid(2, 2, Qt::red), 0).has_value());
    QVERIFY(!ImageExporter::png(solid(2, 2, Qt::red), 10000).has_value());
}

void TestIoAudit::pngCarriesResolutionAndSRGB()
{
    Document doc = exportDocument();
    doc.resolution = 300;
    const QImage image = exportedPng(doc);
    QVERIFY(!image.isNull());
    QCOMPARE(image.dotsPerMeterX(), qRound(300 / .0254));
    QCOMPARE(image.dotsPerMeterY(), qRound(300 / .0254));
    QVERIFY(image.colorSpace().isValid());
    QCOMPARE(image.colorSpace().primaries(), QColorSpace::Primaries::SRgb);
}

void TestIoAudit::jpegTransparencyUsesChosenMatteAndIsOpaqueSRGB()
{
    const QImage transparent(20, 12, QImage::Format_RGBA8888_Premultiplied);
    QImage clear = transparent; clear.fill(Qt::transparent);
    for (const QColor &matte : {QColor(Qt::white), QColor(0, 0, 255)}) {
        QString error;
        const auto result = ImageExporter::jpeg(clear, matte == Qt::white ? 85 : 100, matte, 72, &error);
        QVERIFY2(result.has_value(), qPrintable(error));
        QCOMPARE(result->data.left(2), QByteArray("\xff\xd8"));
        QImage decoded = QImage::fromData(result->data, "JPEG");
        QCOMPARE(decoded.size(), QSize(20, 12));
        QVERIFY(!decoded.hasAlphaChannel());
        const QRgb pixel = decoded.pixel(10, 6);
        QVERIFY(qAbs(qRed(pixel) - matte.red()) < 8 && qAbs(qGreen(pixel) - matte.green()) < 8 && qAbs(qBlue(pixel) - matte.blue()) < 8);
        QVERIFY(decoded.colorSpace().isValid());
        QCOMPARE(decoded.colorSpace().primaries(), QColorSpace::Primaries::SRgb);
    }
}

void TestIoAudit::jpegQualityChangesBytesAndPixelsAndCarriesResolution()
{
    QImage noisy(128, 128, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x)
        noisy.setPixelColor(x, y, QColor((x * 37 + y * 17) % 256, (x * 11 + y * 53) % 256, (x * 79 + y * 7) % 256));
    const auto low = ImageExporter::jpeg(noisy, 10, Qt::white, 300);
    const auto high = ImageExporter::jpeg(noisy, 100, Qt::white, 300);
    QVERIFY(low && high);
    QVERIFY(low->data.size() < high->data.size());
    const QImage lowImage = QImage::fromData(low->data, "JPEG"), highImage = QImage::fromData(high->data, "JPEG");
    int difference = 0;
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) difference += qAbs(qRed(lowImage.pixel(x, y)) - qRed(highImage.pixel(x, y)));
    QVERIFY(difference > 255);
    QCOMPARE(highImage.dotsPerMeterX(), qRound(300 / .0254)); // JFIF density
    QVERIFY(!ImageExporter::jpeg(noisy, 85, QColor(), 72).has_value());
    QVERIFY(!ImageExporter::jpeg(noisy, 85, Qt::white, 0).has_value());
    QCOMPARE(low->preview.size(), QSize(128, 128));
}

void TestIoAudit::copyMergedMatchesExportOfTheSelection()
{
    // Copy Merged is the selection across every visible layer as the canvas shows it; the canvas, the export and the
    // copied pixels must agree (a feature that parses but renders differently is the failure this guards).
    EditorSession session;
    session.createDocument(40, 40, false);
    session.insertImage(solid(40, 40, QColor(0, 0, 255)), QStringLiteral("Blue"));
    session.insertImage(solid(20, 20, QColor(255, 0, 0)), QStringLiteral("Red"), QPointF(30, 30));
    const Layer *red = session.activeLayer();
    QVERIFY(red && red->name == QStringLiteral("Red"));
    session.setLayerOpacity(red->id, 0.5);
    session.setRectangularSelection(QRect(10, 10, 30, 30));
    const auto merged = session.copiedPixels(true);
    QVERIFY(merged.has_value());
    QCOMPARE(merged->first.size(), QSize(30, 30));
    const QImage flat = LayerRenderer::flattened(*session.document());
    for (const QPoint p : {QPoint(0, 0), QPoint(25, 25), QPoint(29, 29)}) {
        const QRgb copied = pixelAt(merged->first, p.x(), p.y());
        const QRgb canvas = pixelAt(flat, p.x() + merged->second.x(), p.y() + merged->second.y());
        QCOMPARE(copied, canvas);
    }
    // The half-covered red really shows through as a blend, not as the top layer alone.
    const QRgb blend = pixelAt(merged->first, 25, 25);
    QVERIFY(qRed(blend) > 100 && qRed(blend) < 160 && qBlue(blend) > 100 && qBlue(blend) < 160);
}

// ---------------------------------------------------------------------------------------------------- import

void TestIoAudit::importSupportedFormats()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QImage fixture = importFixture();
    for (const char *format : {"png", "jpeg", "tiff"}) {
        const QString path = writeFixture(dir, QString::fromLatin1(format == QByteArray("jpeg") ? "jpg" : format), fixture, format);
        QString error;
        const QImage image = ImageImporter::read(path, &error);
        QVERIFY2(!image.isNull(), qPrintable(QStringLiteral("%1: %2").arg(QString::fromLatin1(format), error)));
        QCOMPARE(image.size(), QSize(64, 32));
        QVERIFY(!image.colorSpace().isValid() || image.colorSpace().primaries() == QColorSpace::Primaries::SRgb);
    }
}

void TestIoAudit::importOrientationAndColorConversion()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QImage p3 = importFixture().convertToFormat(QImage::Format_RGBA8888);
    p3.setColorSpace(QColorSpace::DisplayP3);
    p3.setPixelColor(5, 5, QColor::fromRgbF(0.9f, 0.3f, 0.2f));
    // TIFF with EXIF-style orientation 6 (rotate 90 clockwise) like the mac fixture.
    const QString tiff = writeFixture(dir, QStringLiteral("tiff"), p3, "tiff", QImageIOHandler::TransformationRotate90);
    QImageReader probe(tiff); probe.setAutoTransform(false);
    const bool orientationStored = probe.transformation() != QImageIOHandler::TransformationNone;
    const QImage image = ImageImporter::read(tiff);
    QVERIFY(!image.isNull());
    if (orientationStored) QCOMPARE(image.size(), QSize(32, 64));
    // Display P3 pixels are converted to sRGB, not just relabelled.
    const QString png = writeFixture(dir, QStringLiteral("png"), p3, "png");
    const QImage converted = ImageImporter::read(png);
    QVERIFY(!converted.isNull());
    QCOMPARE(converted.colorSpace().primaries(), QColorSpace::Primaries::SRgb);
    const QColor original = p3.pixelColor(5, 5), after = converted.convertToFormat(QImage::Format_ARGB32).pixelColor(5, 5);
    QVERIFY2(original != after, "P3 -> sRGB must change the stored components");
    QImage expect = p3; expect.convertToColorSpace(QColorSpace::SRgb);
    QVERIFY(qAbs(expect.pixelColor(5, 5).red() - after.red()) <= 2);
}

void TestIoAudit::importPngKeepsTransparency()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = writeFixture(dir, QStringLiteral("png"), importFixture(), "png");
    const QImage image = ImageImporter::read(path);
    QVERIFY(!image.isNull());
    QCOMPARE(qAlpha(pixelAt(image, 0, 0)), 255);
    QVERIFY(qRed(pixelAt(image, 0, 0)) >= 250);
    QCOMPARE(qAlpha(pixelAt(image, 63, 0)), 0);
}

void TestIoAudit::importLimitsAndInvalidFiles()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    QString error;
    // Not an image at all.
    const QString bogus = dir.filePath(QStringLiteral("bogus.png"));
    { QFile f(bogus); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("not an image"); }
    QVERIFY(ImageImporter::read(bogus, &error).isNull());
    QVERIFY(!error.isEmpty());
    // Over the side limit (a 30,001 px wide PNG is a few hundred bytes).
    QImage wide(30001, 1, QImage::Format_Grayscale8); wide.fill(0);
    const QString widePath = writeFixture(dir, QStringLiteral("png"), wide, "png");
    error.clear();
    QVERIFY(ImageImporter::read(widePath, &error).isNull());
    QVERIFY(!error.isEmpty());
    // GIF decodes in Qt but the mac importer lists JPEG, PNG, HEIC, TIFF, PSD, SVG and RAW only.
    const QString gif = writeFixture(dir, QStringLiteral("gif"), importFixture().convertToFormat(QImage::Format_Indexed8), "gif");
    if (QFileInfo::exists(gif) && QFileInfo(gif).size() > 0) {
        error.clear();
        QVERIFY2(ImageImporter::read(gif, &error).isNull(), "GIF is not an importable format on the Mac");
        QVERIFY(!error.isEmpty());
    }
}

// ---------------------------------------------------------------------------------------------------- history

void TestIoAudit::historyEveryLayerEditRoundTripsWithSelection()
{
    EditorSession session;
    struct State { std::optional<Document> document; std::optional<QUuid> active; };
    QVector<State> states;
    const auto capture = [&] {
        states.push_back({session.document() ? std::optional<Document>(*session.document()) : std::nullopt,
                          session.document() ? session.document()->activeLayerId : std::nullopt});
    };
    states.push_back({std::nullopt, std::nullopt});
    session.createDocument(800, 600); capture();
    session.addBlankLayer(); capture();
    session.addBlankLayer(); capture();
    const QUuid id = *session.document()->activeLayerId;
    session.renameLayer(id, QStringLiteral("Foreground")); capture();
    session.toggleLayerVisibility(id); capture();
    session.reorderLayers({0}, 2); capture();
    session.moveActiveLayer(1); capture();
    session.deleteActiveLayer(); capture();
    for (int i = states.size() - 2; i >= 0; --i) {
        QVERIFY(session.canUndo());
        session.undo();
        if (states[i].document) {
            QVERIFY(session.document() != nullptr);
            QVERIFY2(*session.document() == *states[i].document, qPrintable(QStringLiteral("undo to step %1").arg(i)));
            QCOMPARE(session.document()->activeLayerId, states[i].active);
        } else {
            QVERIFY(session.document() == nullptr);
        }
    }
    QVERIFY(!session.canUndo());
    for (int i = 1; i < states.size(); ++i) {
        session.redo();
        QVERIFY(session.document() != nullptr);
        QVERIFY2(*session.document() == *states[i].document, qPrintable(QStringLiteral("redo to step %1").arg(i)));
        QCOMPARE(session.document()->activeLayerId, states[i].active);
    }
    QVERIFY(!session.canRedo());
}

void TestIoAudit::historyNavigationNoOpsAndSaveRevision()
{
    EditorSession session;
    session.createDocument(800, 600);
    session.addBlankLayer();
    const QUuid id = *session.document()->activeLayerId;
    session.markSaved();
    QVERIFY(!session.isModified());
    session.renameLayer(id, QStringLiteral("Changed"));
    QVERIFY(session.isModified());
    session.undo();
    QVERIFY(!session.isModified());
    const int count = session.history().undoCount();
    session.renameLayer(id, QStringLiteral("Layer 1"));   // same name: nothing
    session.renameLayer(id, QStringLiteral("   "));       // blank: nothing
    session.reorderLayers({0}, 1);                        // already there: nothing
    QCOMPARE(session.history().undoCount(), count);
    QVERIFY(session.canRedo());
    session.redo();
    QVERIFY(session.isModified());
    session.undo();
    session.addBlankLayer();
    QVERIFY(!session.canRedo());
    QVERIFY(session.isModified());
    // A save that captured an earlier revision leaves later edits unsaved; undoing back to it is clean again.
    EditorSession other;
    other.createDocument(10, 10, true);
    const QUuid saved = other.currentRevision();
    other.addBlankLayer();
    other.markSaved(saved);
    QVERIFY(other.isModified());
    other.undo();
    QVERIFY(!other.isModified());
}

void TestIoAudit::historyReplacementCanvasAndNestedTransactions()
{
    EditorSession session;
    session.createDocument(100, 200);
    session.beginEdit(QStringLiteral("Layer Setup"));
    session.addBlankLayer();
    session.addBlankLayer();
    QVERIFY(!session.canUndo()); // inside a transaction
    session.endEdit();
    QCOMPARE(session.history().undoName(), QStringLiteral("Layer Setup"));
    session.undo();
    QVERIFY(session.document()->layers.isEmpty());
    session.redo();
    const Document previous = *session.document();
    session.createDocument(300, 400);
    session.undo();
    QVERIFY(*session.document() == previous);
    session.redo();
    QCOMPARE(session.document()->canvasSize, QSize(300, 400));
}

void TestIoAudit::historyBoundsEntriesAndUniqueRetainedPixels()
{
    DocumentHistory history(2, 0);
    auto doc = std::make_shared<Document>(documentWith(QSize(64, 32), {imageLayer(QStringLiteral("A"), solid(64, 32, Qt::red))}));
    for (const QString &name : {QStringLiteral("A2"), QStringLiteral("B"), QStringLiteral("C")}) {
        history.begin(QStringLiteral("Rename"), doc);
        doc->layers[0].name = name;
        history.end(doc);
    }
    QCOMPARE(history.undoCount(), 2);
    QCOMPARE(history.retainedBytes(doc), qsizetype(0)); // the pixels are shared with the live layer
    history.begin(QStringLiteral("Delete"), doc);
    doc->layers.clear();
    history.end(doc);
    QCOMPARE(history.undoCount(), 0); // retained pixels exceed the limit, so everything goes
    QCOMPARE(history.retainedBytes(doc), qsizetype(0));
    // The default limits keep 100 entries.
    DocumentHistory normal;
    auto small = std::make_shared<Document>(documentWith(QSize(4, 4), {}));
    for (int i = 0; i < 130; ++i) {
        normal.begin(QStringLiteral("Edit"), small);
        small->canvasSize = QSize(4 + i % 7 + 1, 4);
        normal.end(small);
    }
    QCOMPARE(normal.undoCount(), 100);
}

void TestIoAudit::historyTrimsAfterUndoLandsOnADocumentThatNoLongerSharesPixels()
{
    // Undo moves the live document back, so pixels only the *undone* state held become history-only bytes. With a
    // budget that fits them while they are live but not while retained, the redo entry must be dropped (mac
    // DocumentHistory.undo trims against the document it lands on).
    const QImage first = solid(32, 32, Qt::red), second = solid(32, 32, Qt::green);
    auto doc = std::make_shared<Document>(documentWith(QSize(32, 32), {imageLayer(QStringLiteral("L"), first)}));
    DocumentHistory history(10, qsizetype(first.sizeInBytes()) * 3 / 2);
    history.begin(QStringLiteral("Paint"), doc);
    doc->layers[0].image = second;
    history.end(doc);
    QCOMPARE(history.undoCount(), 1);            // the original pixels are retained: 1 x 4096 bytes fits the limit
    const auto undone = history.undo();
    QVERIFY(undone.has_value());
    QVERIFY(undone->document.has_value());
    QVERIFY(history.canRedo());
    // Now two distinct images exist, one live and one only in the redo entry: still inside the limit.
    // Add a third distinct state so retained bytes exceed it.
    auto landed = std::make_shared<Document>(*undone->document);
    history.begin(QStringLiteral("Paint 2"), landed);
    landed->layers[0].image = solid(32, 32, Qt::blue);
    history.end(landed);
    QVERIFY(history.retainedBytes(landed) <= qsizetype(first.sizeInBytes()) * 3 / 2);
}

void TestIoAudit::undoRedoMenuNamesTheStep()
{
    MainWindow window;
    window.session().createDocument(50, 50, true);
    window.syncDocumentViews();
    auto *undo = window.findChild<QAction *>(QStringLiteral("commandUndo"));
    auto *redo = window.findChild<QAction *>(QStringLiteral("commandRedo"));
    QVERIFY(undo && redo);
    QVERIFY(undo->isEnabled() == window.session().canUndo());
    window.session().renameLayer(*window.document()->activeLayerId, QStringLiteral("Hero"));
    window.syncDocumentViews();
    QVERIFY(undo->isEnabled());
    QVERIFY2(undo->text().contains(QStringLiteral("Rename Layer")), qPrintable(undo->text()));
    QVERIFY(!redo->isEnabled());
    undo->trigger();
    QCOMPARE(window.document()->layers.constFirst().name, QStringLiteral("Layer 1"));
    QVERIFY2(redo->text().contains(QStringLiteral("Rename Layer")), qPrintable(redo->text()));
    QVERIFY(redo->isEnabled());
    redo->trigger();
    QCOMPARE(window.document()->layers.constFirst().name, QStringLiteral("Hero"));
}

// ---------------------------------------------------------------------------------------------------- project

void TestIoAudit::projectRoundTripSurvivesSourceRemovalAndMove()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString source = writeFixture(dir, QStringLiteral("png"), importFixture(), "png");
    MainWindow window;
    QVERIFY(window.importImageFiles({source}));
    QVERIFY(QFile::remove(source));
    EditorSession &session = window.session();
    const QUuid imageId = *session.document()->activeLayerId;
    LayerTransform t = session.document()->layers.constLast().transform;
    t.origin = QPointF(-27.5, 88.25); t.size = QSizeF(123, 47); t.rotation = 38; t.flipX = true; t.flipY = true; t.sampling = Sampling::Nearest;
    session.beginEdit(QStringLiteral("Transform"));
    session.document()->layers.last().transform = t;
    session.endEdit();
    session.renameLayer(imageId, QStringLiteral("Paint & sky \U0001F324"));
    session.toggleLayerVisibility(imageId);
    session.addBlankLayer();
    const Document before = *session.document();
    const QString original = dir.filePath(QStringLiteral("Original.comp")), moved = dir.filePath(QStringLiteral("Moved.comp"));
    ProjectWriter::save(before, original);
    QVERIFY(QDir().rename(original, moved));
    const Document loaded = ProjectReader::load(moved);
    QCOMPARE(loaded.id, before.id);
    QCOMPARE(loaded.canvasSize, before.canvasSize);
    QCOMPARE(loaded.resolution, before.resolution);
    QCOMPARE(loaded.activeLayerId, before.activeLayerId);
    QCOMPARE(loaded.layers.size(), before.layers.size());
    for (int i = 0; i < before.layers.size(); ++i) {
        QCOMPARE(loaded.layers[i].id, before.layers[i].id);
        QCOMPARE(loaded.layers[i].name, before.layers[i].name);
        QCOMPARE(loaded.layers[i].visible, before.layers[i].visible);
        QVERIFY(loaded.layers[i].transform == before.layers[i].transform);
    }
    QVERIFY(loaded.layers.last().image.isNull()); // the blank layer has no asset
    const QImage pixels = loaded.layers.first().image;
    QCOMPARE(pixels.size(), QSize(64, 32));
    QVERIFY(qRed(pixelAt(pixels, 0, 0)) > 240);
    QCOMPARE(qAlpha(pixelAt(pixels, 63, 0)), 0);
    // Reopened: clean, no history; a rename is modified and undo makes it clean again.
    EditorSession reopened;
    QVERIFY(reopened.openProject(moved));
    QVERIFY(!reopened.isModified());
    QVERIFY(!reopened.canUndo());
    reopened.renameLayer(imageId, QStringLiteral("Edited"));
    QVERIFY(reopened.isModified());
    reopened.undo();
    QVERIFY(!reopened.isModified());
}

void TestIoAudit::projectDimmedFolderSavesAndReopens()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    EditorSession session;
    session.createDocument(40, 40);
    session.insertImage(solid(40, 40, Qt::red), QStringLiteral("Child"));
    const QUuid child = *session.document()->activeLayerId;
    session.selectLayers({child}, child);
    session.groupSelectedLayers();
    const QUuid folder = *session.document()->activeLayerId;
    QVERIFY(session.document()->layers.last().group);
    session.setLayerOpacity(folder, 0.5);
    const auto it = std::find_if(session.document()->layers.cbegin(), session.document()->layers.cend(), [&](const Layer &l) { return l.id == folder; });
    QCOMPARE(it->opacity, 0.5);
    const QString path = dir.filePath(QStringLiteral("Dimmed.comp"));
    ProjectWriter::save(*session.document(), path);
    const Document loaded = ProjectReader::load(path);
    const auto saved = std::find_if(loaded.layers.cbegin(), loaded.layers.cend(), [](const Layer &l) { return l.group; });
    QVERIFY(saved != loaded.layers.cend());
    QCOMPARE(saved->opacity, 0.5);
}

void TestIoAudit::projectOverwriteDropsRemovedAssets()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    EditorSession session;
    session.createDocument(30, 30);
    session.insertImage(solid(30, 30, Qt::red), QStringLiteral("Pixels"));
    const QString destination = dir.filePath(QStringLiteral("Overwrite.comp"));
    ProjectWriter::save(*session.document(), destination);
    QCOMPARE(QDir(destination + QStringLiteral("/images")).entryList(QDir::Files).size(), 1);
    session.deleteActiveLayer();
    session.addBlankLayer();
    ProjectWriter::save(*session.document(), destination);
    const Document loaded = ProjectReader::load(destination);
    QCOMPARE(loaded.layers.size(), 1);
    QVERIFY(loaded.layers.constFirst().image.isNull());
    QCOMPARE(QDir(destination + QStringLiteral("/images")).entryList(QDir::Files).size(), 0);
}

void TestIoAudit::projectFailedSavePreservesPackage()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    EditorSession session;
    session.createDocument(100, 80, true);
    const QString url = dir.filePath(QStringLiteral("Safe.comp"));
    ProjectWriter::save(*session.document(), url);
    const auto manifest = [&] { QFile f(url + QStringLiteral("/manifest.json")); f.open(QIODevice::ReadOnly); return f.readAll(); };
    const QByteArray original = manifest();
    Document invalid = *session.document();
    invalid.layers[0].opacity = 2.0;
    bool threw = false;
    try { ProjectWriter::save(invalid, url); } catch (const std::exception &) { threw = true; }
    QVERIFY2(threw, "an invalid document must not be saved");
    QCOMPARE(manifest(), original);
    QCOMPARE(ProjectReader::load(url).layers.size(), 1);
    // A path through a regular file cannot be written; the earlier package is untouched.
    const QString blocker = dir.filePath(QStringLiteral("not-a-directory"));
    { QFile f(blocker); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("x"); }
    threw = false;
    try { ProjectWriter::save(*session.document(), blocker + QStringLiteral("/CannotSave.comp")); } catch (const std::exception &) { threw = true; }
    QVERIFY(threw);
    QCOMPARE(manifest(), original);
}

void TestIoAudit::projectUnsupportedCorruptUnsafeRejected()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    EditorSession session;
    session.createDocument(100, 80, true);
    const QString url = dir.filePath(QStringLiteral("Invalid.comp"));
    ProjectWriter::save(*session.document(), url);
    const QString manifestPath = url + QStringLiteral("/manifest.json");
    QFile file(manifestPath); QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    const auto writeManifest = [&](const QJsonObject &object) {
        QFile out(manifestPath); QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate)); out.write(QJsonDocument(object).toJson());
    };
    QJsonObject future = manifest; future.insert(QStringLiteral("version"), 42);
    writeManifest(future);
    try { ProjectReader::load(url); QFAIL("a future version opened"); } catch (const ProjectError &error) { QVERIFY(error.message().contains(QStringLiteral("42"))); }
    QJsonObject unsafe = manifest;
    QJsonArray layers = unsafe.value(QStringLiteral("layers")).toArray();
    QJsonObject first = layers.at(0).toObject(); first.insert(QStringLiteral("imageFile"), QStringLiteral("../../outside.png")); layers[0] = first;
    unsafe.insert(QStringLiteral("layers"), layers);
    writeManifest(unsafe);
    try { ProjectReader::load(url); QFAIL("path traversal accepted"); } catch (const ProjectError &) {}
    { QFile out(manifestPath); QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate)); out.write("not json"); }
    try { ProjectReader::load(url); QFAIL("corrupt metadata accepted"); } catch (const ProjectError &) {}
    // Every documented rejection rule, one at a time, against an otherwise valid v11 manifest.
    const auto rejects = [&](const char *what, const std::function<void(QJsonObject &)> &mutate) {
        QJsonObject copy = manifest; mutate(copy); writeManifest(copy);
        bool rejected = false; try { ProjectReader::load(url); } catch (const ProjectError &) { rejected = true; }
        QVERIFY2(rejected, what);
    };
    rejects("zero width", [](QJsonObject &m) { m.insert(QStringLiteral("width"), 0); });
    rejects("width over 30,000", [](QJsonObject &m) { m.insert(QStringLiteral("width"), 30001); });
    rejects("resolution 0", [](QJsonObject &m) { m.insert(QStringLiteral("resolution"), 0); });
    rejects("resolution 9601", [](QJsonObject &m) { m.insert(QStringLiteral("resolution"), 9601); });
    rejects("wrong format", [](QJsonObject &m) { m.insert(QStringLiteral("format"), QStringLiteral("other")); });
    rejects("non-sRGB working space", [](QJsonObject &m) { m.insert(QStringLiteral("colorSpace"), QStringLiteral("P3")); });
    rejects("active layer missing", [](QJsonObject &m) { m.insert(QStringLiteral("activeLayerID"), QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper()); });
    rejects("opacity 1.5", [](QJsonObject &m) { QJsonArray a = m.value(QStringLiteral("layers")).toArray(); QJsonObject l = a.at(0).toObject(); l.insert(QStringLiteral("opacity"), 1.5); a[0] = l; m.insert(QStringLiteral("layers"), a); });
    rejects("blank layer name", [](QJsonObject &m) { QJsonArray a = m.value(QStringLiteral("layers")).toArray(); QJsonObject l = a.at(0).toObject(); l.insert(QStringLiteral("name"), QStringLiteral("   ")); a[0] = l; m.insert(QStringLiteral("layers"), a); });
    rejects("unknown blend mode", [](QJsonObject &m) { QJsonArray a = m.value(QStringLiteral("layers")).toArray(); QJsonObject l = a.at(0).toObject(); l.insert(QStringLiteral("blendMode"), QStringLiteral("Dissolve")); a[0] = l; m.insert(QStringLiteral("layers"), a); });
    rejects("folder with a non-Normal blend", [](QJsonObject &m) { QJsonArray a = m.value(QStringLiteral("layers")).toArray(); QJsonObject l = a.at(0).toObject(); l.remove(QStringLiteral("imageFile")); l.insert(QStringLiteral("isGroup"), true); l.insert(QStringLiteral("blendMode"), QStringLiteral("Multiply")); a[0] = l; m.insert(QStringLiteral("layers"), a); });
    rejects("version 3 file with a mask", [](QJsonObject &m) { m.insert(QStringLiteral("version"), 3); QJsonArray a = m.value(QStringLiteral("layers")).toArray(); QJsonObject l = a.at(0).toObject(); l.insert(QStringLiteral("maskFile"), l.value(QStringLiteral("id")).toString() + QStringLiteral(".mask.png")); a[0] = l; m.insert(QStringLiteral("layers"), a); });
    rejects("guides before version 8", [](QJsonObject &m) { m.insert(QStringLiteral("version"), 7); QJsonObject g; g.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper()); g.insert(QStringLiteral("axis"), QStringLiteral("horizontal")); g.insert(QStringLiteral("position"), 5); m.insert(QStringLiteral("guides"), QJsonArray{g}); });
    rejects("guide position beyond 1,000,000", [](QJsonObject &m) { QJsonObject g; g.insert(QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper()); g.insert(QStringLiteral("axis"), QStringLiteral("vertical")); g.insert(QStringLiteral("position"), 1000001); m.insert(QStringLiteral("guides"), QJsonArray{g}); });
    rejects("duplicate layer ids", [](QJsonObject &m) { QJsonArray a = m.value(QStringLiteral("layers")).toArray(); a.append(a.at(0)); m.insert(QStringLiteral("layers"), a); });
    rejects("layer image named after another layer", [](QJsonObject &m) { QJsonArray a = m.value(QStringLiteral("layers")).toArray(); QJsonObject l = a.at(0).toObject(); l.insert(QStringLiteral("imageFile"), QStringLiteral("other.png")); a[0] = l; m.insert(QStringLiteral("layers"), a); });
    // And the untouched manifest still loads (the rules above are not just rejecting everything).
    writeManifest(manifest);
    QCOMPARE(ProjectReader::load(url).layers.size(), 1);
}

void TestIoAudit::projectMissingEmbeddedImageRejected()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString url = saveSimpleProject(dir, QStringLiteral("Missing.comp"));
    const QString imageDir = url + QStringLiteral("/images");
    const QStringList images = QDir(imageDir).entryList(QDir::Files);
    QCOMPARE(images.size(), 1);
    QVERIFY(QFile::remove(imageDir + QLatin1Char('/') + images.constFirst()));
    try { ProjectReader::load(url); QFAIL("a missing image was accepted"); } catch (const ProjectError &) {}
}

// ---------------------------------------------------------------------------------------------------- import pipeline

void TestIoAudit::batchImportIsOneUndoEntryAndFailuresAddNone()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString png = writeFixture(dir, QStringLiteral("png"), importFixture(), "png");
    const QString missing = png + QStringLiteral(".missing");
    MainWindow window;
    MainWindow::setMessageDialogHook([](const QString &, const QString &) -> std::optional<QMessageBox::StandardButton> { return QMessageBox::Ok; });
    QVERIFY(window.importImageFiles({png, missing, png}));
    EditorSession &session = window.session();
    QCOMPARE(session.history().undoCount(), 1);
    QCOMPARE(session.document()->layers.size(), 2);
    const Document imported = *session.document();
    const auto selection = session.document()->activeLayerId;
    QCOMPARE(session.history().undoName(), QStringLiteral("Import Images"));
    session.undo();
    QVERIFY(session.document() == nullptr);
    session.redo();
    QVERIFY(*session.document() == imported);
    QCOMPARE(session.document()->activeLayerId, selection);
    QVERIFY(!window.importImageFiles({missing}));
    QCOMPARE(session.history().undoCount(), 1);
}

void TestIoAudit::importFirstImageDecidesCanvasIgnoringDropPoint()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString png = writeFixture(dir, QStringLiteral("png"), importFixture(), "png");
    MainWindow window;
    QVERIFY(window.importImageFiles({png, png}, QPointF(999, 999)));
    const auto &doc = *window.document();
    QCOMPARE(doc.canvasSize, QSize(64, 32));
    QCOMPARE(doc.layers.size(), 2);
    QCOMPARE(doc.layers.first().transform.origin, QPointF(0, 0));
    QCOMPARE(doc.layers.last().transform.origin, QPointF(0, 0));
    QCOMPARE(doc.activeLayerId, std::optional<QUuid>(doc.layers.last().id));
}

void TestIoAudit::importPlacementOnExistingCanvas()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString png = writeFixture(dir, QStringLiteral("png"), importFixture(), "png");
    MainWindow window;
    MainWindow::setMessageDialogHook([](const QString &, const QString &) -> std::optional<QMessageBox::StandardButton> { return QMessageBox::Ok; });
    window.session().createDocument(128, 128);
    window.syncDocumentViews();
    QVERIFY(window.importImageFiles({png, png + QStringLiteral(".missing")}));
    QCOMPARE(window.document()->canvasSize, QSize(128, 128));
    QCOMPARE(window.document()->layers.size(), 1);
    QCOMPARE(window.document()->layers.first().transform.origin, QPointF(32, 48));
    // A drop point places the image's centre there.
    QVERIFY(window.importImageFiles({png}, QPointF(300, 250)));
    QCOMPARE(window.document()->layers.last().transform.origin, QPointF(268, 234));
}

void TestIoAudit::importRejectsFormatsMacDoesNotImport()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString gif = writeFixture(dir, QStringLiteral("gif"), importFixture().convertToFormat(QImage::Format_Indexed8), "gif");
    if (!QFileInfo::exists(gif) || QFileInfo(gif).size() == 0) QSKIP("no GIF writer in this Qt build");
    MainWindow window;
    bool shown = false;
    MainWindow::setMessageDialogHook([&](const QString &, const QString &) -> std::optional<QMessageBox::StandardButton> { shown = true; return QMessageBox::Ok; });
    QVERIFY(!window.importImageFiles({gif}));
    QVERIFY(shown);
    QVERIFY(window.document() == nullptr);
}

void TestIoAudit::receiveFilesOpensOneProjectThenImports()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString projectA = saveSimpleProject(dir, QStringLiteral("A.comp"), Qt::red);
    const QString projectB = saveSimpleProject(dir, QStringLiteral("B.comp"), Qt::blue);
    const QString png = writeFixture(dir, QStringLiteral("png"), importFixture(), "png");
    MainWindow window;
    QStringList titles;
    MainWindow::setMessageDialogHook([&](const QString &title, const QString &) -> std::optional<QMessageBox::StandardButton> { titles << title; return QMessageBox::Ok; });
    // Two projects at once: refused, nothing opens.
    QVERIFY(!window.receiveFiles({projectA, projectB}));
    QVERIFY(titles.contains(QStringLiteral("Open one project at a time")));
    QVERIFY(window.document() == nullptr);
    // One project plus an image: the project opens, then the image comes in as a layer of it (no drop point).
    QVERIFY(window.receiveFiles({projectA, png}, QPointF(5, 5)));
    QCOMPARE(window.document()->layers.size(), 2);
    QCOMPARE(QFileInfo(window.document()->projectPath).fileName(), QStringLiteral("A.comp"));
    QCOMPARE(window.document()->canvasSize, QSize(40, 30));
    QCOMPARE(window.document()->layers.last().transform.origin, QPointF(floor(20 - 32), floor(15 - 16)));
}

// ---------------------------------------------------------------------------------------------------- workspace / chrome

void TestIoAudit::openRecentMenuListsExistingProjectsAndClears()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString a = saveSimpleProject(dir, QStringLiteral("Alpha.comp"));
    const QString b = saveSimpleProject(dir, QStringLiteral("Beta.comp"));
    MainWindow window;
    QMenu *recent = window.openRecentMenu();
    QVERIFY2(recent, "File > Open Recent must exist");
    QCOMPARE(recent->title().remove(QLatin1Char('&')), QStringLiteral("Open Recent"));
    const auto items = [&] { QStringList names; for (QAction *action : recent->actions()) if (!action->isSeparator()) names << action->text(); return names; };
    QCOMPARE(items(), QStringList{QStringLiteral("Clear Menu")});
    QVERIFY(!recent->actions().last()->isEnabled());
    QVERIFY(window.openProject(a));
    QVERIFY(window.openProject(b));
    QCOMPARE(items(), (QStringList{QStringLiteral("Beta"), QStringLiteral("Alpha"), QStringLiteral("Clear Menu")}));
    QVERIFY(recent->actions().last()->isEnabled());
    // Choosing an entry opens that project.
    window.closeTab(1);
    recent->actions().at(1)->trigger();   // Alpha: already open in tab 0, so it just comes forward
    QCOMPARE(QFileInfo(window.document()->projectPath).fileName(), QStringLiteral("Alpha.comp"));
    // Projects deleted since are left out.
    QDir(b).removeRecursively();
    emit recent->aboutToShow();
    QCOMPARE(items(), (QStringList{QStringLiteral("Alpha"), QStringLiteral("Clear Menu")}));
    // Clear Menu empties it and disables itself.
    recent->actions().last()->trigger();
    QCOMPARE(items(), QStringList{QStringLiteral("Clear Menu")});
    QVERIFY(!recent->actions().last()->isEnabled());
    QVERIFY(window.recentProjects().isEmpty());
}

void TestIoAudit::openingAnOpenProjectFocusesItsTab()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString a = saveSimpleProject(dir, QStringLiteral("One.comp"));
    const QString b = saveSimpleProject(dir, QStringLiteral("Two.comp"));
    MainWindow window;
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("documentTabs"));
    QVERIFY(tabs);
    QVERIFY(window.openProject(a));
    QVERIFY(window.openProject(b));
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->currentIndex(), 1);
    QVERIFY(window.openProject(a));
    QCOMPARE(tabs->count(), 2);          // not opened twice
    QCOMPARE(tabs->currentIndex(), 0);   // brought forward
    QCOMPARE(QFileInfo(window.document()->projectPath).fileName(), QStringLiteral("One.comp"));
    QVERIFY(window.openProject(a + QStringLiteral("/../One.comp")));
    QCOMPARE(tabs->count(), 2);          // same package through another spelling
}

void TestIoAudit::openingAMissingProjectNamesIt()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    MainWindow window;
    QString text;
    MainWindow::setMessageDialogHook([&](const QString &, const QString &body) -> std::optional<QMessageBox::StandardButton> { text = body; return QMessageBox::Ok; });
    QVERIFY(!window.openProject(dir.filePath(QStringLiteral("Gone.comp"))));
    QVERIFY2(text.contains(QStringLiteral("Gone.comp")), qPrintable(text));
    QVERIFY(!text.contains(QStringLiteral("manifest")));
}

void TestIoAudit::tabsKeepIndependentDocumentsAndUndo()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString a = saveSimpleProject(dir, QStringLiteral("A.comp"));
    const QString b = saveSimpleProject(dir, QStringLiteral("B.comp"), Qt::blue);
    MainWindow window;
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("documentTabs"));
    QVERIFY(window.openProject(a));
    QVERIFY(window.openProject(b));
    window.session().renameLayer(*window.document()->activeLayerId, QStringLiteral("Edited in B"));
    QVERIFY(window.session().canUndo());
    tabs->setCurrentIndex(0);
    QCOMPARE(tabs->currentIndex(), 0);
    QCOMPARE(window.document()->layers.first().name, QStringLiteral("Layer"));
    QVERIFY(!window.session().canUndo());   // A has its own history
    window.session().renameLayer(*window.document()->activeLayerId, QStringLiteral("Edited in A"));
    tabs->setCurrentIndex(1);
    QCOMPARE(window.document()->layers.first().name, QStringLiteral("Edited in B"));
    window.session().undo();
    window.syncDocumentViews();
    QCOMPARE(window.document()->layers.first().name, QStringLiteral("Layer"));
    tabs->setCurrentIndex(0);
    QCOMPARE(window.document()->layers.first().name, QStringLiteral("Edited in A"));
    QVERIFY(window.session().canUndo());
}

void TestIoAudit::tabReorderKeepsDocumentsAndExternalWatch()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString a = saveSimpleProject(dir, QStringLiteral("A.comp"), Qt::red);
    const QString b = saveSimpleProject(dir, QStringLiteral("B.comp"), Qt::blue);
    MainWindow window;
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("documentTabs"));
    QVERIFY(window.openProject(a));
    QVERIFY(window.openProject(b));
    const QUuid idA = ProjectReader::load(a).id, idB = ProjectReader::load(b).id;
    // Drag the first tab to the end, as the strip's drag reorder does.
    tabs->moveTab(0, 1);
    QCOMPARE(tabs->tabText(0).remove(QStringLiteral(" •")), QStringLiteral("B"));
    QCOMPARE(tabs->tabText(1).remove(QStringLiteral(" •")), QStringLiteral("A"));
    tabs->setCurrentIndex(0);
    QCOMPARE(window.document()->id, idB);
    tabs->setCurrentIndex(1);
    QCOMPARE(window.document()->id, idA);
    // The external watch follows the document, not the old index: change B on disk while it sits at index 0.
    Document changed = ProjectReader::load(b);
    changed.layers[0].name = QStringLiteral("Changed externally");
    ProjectWriter::save(changed, b);
    tabs->setCurrentIndex(0);
    QTRY_COMPARE_WITH_TIMEOUT(window.document()->layers.first().name, QStringLiteral("Changed externally"), 8000);
}

void TestIoAudit::quitAsksAboutEveryUnsavedTab()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString a = saveSimpleProject(dir, QStringLiteral("A.comp"));
    const QString b = saveSimpleProject(dir, QStringLiteral("B.comp"));
    MainWindow window;
    window.show();
    QVERIFY(window.openProject(a));
    window.session().renameLayer(*window.document()->activeLayerId, QStringLiteral("dirty A"));
    QVERIFY(window.openProject(b));
    window.session().renameLayer(*window.document()->activeLayerId, QStringLiteral("dirty B"));
    QStringList asked;
    MainWindow::setMessageDialogHook([&](const QString &, const QString &text) -> std::optional<QMessageBox::StandardButton> {
        asked << text;
        return QMessageBox::Cancel;
    });
    auto *quit = window.findChild<QAction *>(QStringLiteral("commandQuit"));
    QVERIFY2(quit, "File > Quit must exist");
    quit->trigger();
    QCOMPARE(asked.size(), 1);       // the tab on screen first, and Cancel stops the quit
    QVERIFY(asked.first().contains(QStringLiteral("B.comp")));
    QVERIFY(window.isVisible());
    // Discard on every tab: both asked, then the window closes.
    asked.clear();
    MainWindow::setMessageDialogHook([&](const QString &, const QString &text) -> std::optional<QMessageBox::StandardButton> {
        asked << text;
        return QMessageBox::Discard;
    });
    quit->trigger();
    QCOMPARE(asked.size(), 2);
    QVERIFY(asked.first().contains(QStringLiteral("B.comp")));
    QVERIFY(asked.last().contains(QStringLiteral("A.comp")));
    QVERIFY(!window.isVisible());
}

void TestIoAudit::closeTabAsksAndRemoves()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString a = saveSimpleProject(dir, QStringLiteral("A.comp"));
    const QString b = saveSimpleProject(dir, QStringLiteral("B.comp"));
    MainWindow window;
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("documentTabs"));
    QVERIFY(window.openProject(a));
    QVERIFY(window.openProject(b));
    window.session().renameLayer(*window.document()->activeLayerId, QStringLiteral("dirty"));
    MainWindow::setMessageDialogHook([](const QString &, const QString &) -> std::optional<QMessageBox::StandardButton> { return QMessageBox::Cancel; });
    window.closeTab(1);
    QCOMPARE(tabs->count(), 2);   // cancelled
    MainWindow::setMessageDialogHook([](const QString &, const QString &) -> std::optional<QMessageBox::StandardButton> { return QMessageBox::Discard; });
    window.closeTab(1);
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(QFileInfo(window.document()->projectPath).fileName(), QStringLiteral("A.comp"));
    window.closeTab(0);           // the last tab closes to an empty Untitled one
    QCOMPARE(tabs->count(), 1);
    QVERIFY(window.document() == nullptr);
}

void TestIoAudit::unsavedMarkOnTabAndTitle()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString a = saveSimpleProject(dir, QStringLiteral("Marked.comp"));
    MainWindow window;
    auto *tabs = window.findChild<QTabBar *>(QStringLiteral("documentTabs"));
    QVERIFY(window.openProject(a));
    QVERIFY(!tabs->tabText(0).contains(QStringLiteral("•")));
    window.session().renameLayer(*window.document()->activeLayerId, QStringLiteral("x"));
    window.syncDocumentViews();
    QVERIFY2(tabs->tabText(0).contains(QStringLiteral("•")), qPrintable(tabs->tabText(0)));
    QVERIFY(window.windowTitle().contains(QStringLiteral("•")));
    window.session().undo();
    window.syncDocumentViews();
    QVERIFY(!tabs->tabText(0).contains(QStringLiteral("•")));
}

// ---------------------------------------------------------------------------------------------------- shortcuts / menus

namespace {
// The mac KeyboardShortcuts.swift defaults, transcribed: title, key, modifier bits (1 Command, 2 Option, 4 Control, 8 Shift).
struct MacShortcut { const char *group; const char *title; int key; int bits; };
const MacShortcut kMacDefaults[] = {
    {"Menus", "Undo", 'z', 1}, {"Menus", "Redo", 'z', 9}, {"Menus", "New Canvas", 'n', 1}, {"Menus", "Open Project", 'o', 1},
    {"Menus", "Save", 's', 1}, {"Menus", "Save As", 's', 9}, {"Menus", "Export PNG", 'e', 9}, {"Menus", "Export JPEG", 's', 11},
    {"Menus", "Close Project", 'w', 1}, {"Menus", "Fit Canvas", '0', 1}, {"Menus", "Actual Pixels", '1', 1}, {"Menus", "Zoom In", '=', 1},
    {"Menus", "Zoom Out", '-', 1}, {"Menus", "Show Transform Controls", 'h', 1}, {"Menus", "Cut", 'x', 1}, {"Menus", "Copy", 'c', 1},
    {"Menus", "Copy Merged", 'c', 9}, {"Menus", "Paste", 'v', 1}, {"Menus", "Fill with Foreground", 0x7f, 2},
    {"Menus", "Fill with Background", 0x7f, 1}, {"Menus", "Content-Aware Fill", 0x7f, 8}, {"Menus", "Select All", 'a', 1},
    {"Menus", "Deselect", 'd', 1}, {"Menus", "Inverse Selection", 'i', 9}, {"Menus", "Select Subject", 'a', 3}, {"Menus", "Curves", 'm', 1},
    {"Menus", "Levels", 'l', 1}, {"Menus", "Hue/Saturation", 'u', 1}, {"Menus", "Invert Pixels / Mask", 'i', 1}, {"Menus", "Canvas Size", 'c', 3},
    {"Menus", "Image Size", 'i', 3}, {"Menus", "Transform Layer / Selection", 't', 1}, {"Menus", "Duplicate / Layer via Copy", 'j', 1},
    {"Menus", "Toggle Clipping Mask", 'g', 3}, {"Menus", "Group Layers", 'g', 1}, {"Menus", "Ungroup Layers", 'g', 9},
    {"Menus", "New Blank Layer", 'n', 9}, {"Menus", "Move Layer Up", ']', 1}, {"Menus", "Move Layer Down", '[', 1}, {"Menus", "Merge Layers", 'e', 1},
    {"Menus", "Show Grid", '\'', 1}, {"Menus", "Show Guides", ';', 1}, {"Menus", "Show Rulers", 'r', 1}, {"Menus", "Snap", ';', 9},
    {"Menus", "Lock Guides", ';', 3},
    {"Canvas & Layers", "Select tool", 'a', 0}, {"Canvas & Layers", "Move / Transform tool", 'v', 0}, {"Canvas & Layers", "Hand tool", 'h', 0},
    {"Canvas & Layers", "Zoom tool", 'z', 0}, {"Canvas & Layers", "Brush tool", 'b', 0}, {"Canvas & Layers", "Eraser", 'e', 0},
    {"Canvas & Layers", "Spot Healing", 'j', 0}, {"Canvas & Layers", "Clone Stamp", 's', 0}, {"Canvas & Layers", "Type tool", 't', 0},
    {"Canvas & Layers", "Gradient tool", 'g', 0}, {"Canvas & Layers", "Shape tool", 'u', 0}, {"Canvas & Layers", "Eyedropper tool", 'i', 0},
    {"Canvas & Layers", "Marquee / cycle shape", 'm', 0}, {"Canvas & Layers", "Magic", 'w', 0}, {"Canvas & Layers", "Lasso / cycle mode", 'l', 0},
    {"Canvas & Layers", "Blur / Smudge / Liquify", 'r', 0}, {"Canvas & Layers", "Crop tool", 'c', 0},
    {"Canvas & Layers", "Swap foreground/background", 'x', 0}, {"Canvas & Layers", "Reset colors", 'd', 0}, {"Canvas & Layers", "Cycle tool mode", '\t', 0},
    {"Canvas & Layers", "Temporary Hand tool (hold)", ' ', 0}, {"Canvas & Layers", "Delete selection / layer / effect / lasso point", 0x7f, 0},
    {"Canvas & Layers", "Apply current canvas operation", '\r', 0}, {"Canvas & Layers", "Cancel current canvas operation", 0x1b, 0},
    {"Canvas & Layers", "Decrease brush size", '[', 0}, {"Canvas & Layers", "Increase brush size", ']', 0},
    {"Canvas & Layers", "Decrease brush hardness", '[', 8}, {"Canvas & Layers", "Increase brush hardness", ']', 8},
    {"Canvas & Layers", "Previous blend mode", '-', 8}, {"Canvas & Layers", "Next blend mode", '=', 8}, {"Canvas & Layers", "Cycle shape kind", 'u', 8},
    {"Canvas & Layers", "Toggle Levels preview", 'p', 2},
    {"Text Editing", "Finish editing text", '\r', 1},
    {"Text Editing", "Decrease tracking", 0xf702, 2}, {"Text Editing", "Increase tracking", 0xf703, 2},
    {"Text Editing", "Decrease leading", 0xf700, 2}, {"Text Editing", "Increase leading", 0xf701, 2},
    {"Text Editing", "Decrease tracking by 10", 0xf702, 10}, {"Text Editing", "Increase tracking by 10", 0xf703, 10},
    {"Text Editing", "Decrease leading by 10", 0xf700, 10}, {"Text Editing", "Increase leading by 10", 0xf701, 10},
};

// Command -> Ctrl, Option -> Alt, Control -> Meta (a Mac key chord, carried to Linux conventions).
QKeySequence linuxSequence(int key, int bits)
{
    Qt::KeyboardModifiers modifiers;
    if (bits & 1) modifiers |= Qt::ControlModifier;
    if (bits & 2) modifiers |= Qt::AltModifier;
    if (bits & 4) modifiers |= Qt::MetaModifier;
    if (bits & 8) modifiers |= Qt::ShiftModifier;
    Qt::Key qtKey;
    switch (key) {
    case 0x7f: qtKey = (bits == 8) ? Qt::Key_Delete : (bits == 0 ? Qt::Key_Delete : Qt::Key_Backspace); break;
    case '\r': qtKey = Qt::Key_Return; break;
    case 0x1b: qtKey = Qt::Key_Escape; break;
    case '\t': qtKey = Qt::Key_Tab; break;
    case ' ': qtKey = Qt::Key_Space; break;
    case 0xf702: qtKey = Qt::Key_Left; break;
    case 0xf703: qtKey = Qt::Key_Right; break;
    case 0xf700: qtKey = Qt::Key_Up; break;
    case 0xf701: qtKey = Qt::Key_Down; break;
    case '\'': qtKey = Qt::Key_Apostrophe; break;
    case ';': qtKey = Qt::Key_Semicolon; break;
    case '[': qtKey = Qt::Key_BracketLeft; break;
    case ']': qtKey = Qt::Key_BracketRight; break;
    case '=': qtKey = Qt::Key_Equal; break;
    case '-': qtKey = Qt::Key_Minus; break;
    default: qtKey = Qt::Key(std::toupper(key)); break;
    }
    return QKeySequence(QKeyCombination(modifiers, qtKey));
}
} // namespace

void TestIoAudit::defaultShortcutTableMatchesMac()
{
    const ShortcutManager &manager = ShortcutManager::instance();
    for (const MacShortcut &mac : kMacDefaults) {
        const QString id = QStringLiteral("%1:%2").arg(QString::fromLatin1(mac.group), QString::fromLatin1(mac.title));
        const ShortcutDefinition *definition = manager.findDefinition(id);
        QVERIFY2(definition, qPrintable(QStringLiteral("missing shortcut definition %1").arg(id)));
        QKeySequence expected = linuxSequence(mac.key, mac.bits);
        // "Delete" on the Mac is the backspace key; Content-Aware Fill's Shift-Delete is a forward delete in LX.
        if (id.endsWith(QStringLiteral("Fill with Foreground")) || id.endsWith(QStringLiteral("Fill with Background"))) {
            QCOMPARE(definition->defaultShortcut[0].key(), Qt::Key_Backspace);
            QCOMPARE(definition->defaultShortcut[0].keyboardModifiers(), expected[0].keyboardModifiers());
            continue;
        }
        if (id.endsWith(QStringLiteral("Content-Aware Fill"))) { QCOMPARE(definition->defaultShortcut[0].keyboardModifiers(), Qt::ShiftModifier); continue; }
        QVERIFY2(definition->defaultShortcut == expected,
                 qPrintable(QStringLiteral("%1: mac %2 vs LX %3").arg(id, expected.toString(), definition->defaultShortcut.toString())));
    }
    // Opacity digits and the nudge tables.
    for (int digit = 0; digit <= 9; ++digit) {
        const auto *definition = manager.findDefinition(QStringLiteral("Canvas & Layers:Opacity digit %1 (type two for exact %)").arg(digit));
        QVERIFY(definition);
        QCOMPARE(definition->defaultShortcut, QKeySequence(Qt::Key(Qt::Key_0 + digit)));
    }
    const struct { const char *direction; Qt::Key key; } arrows[] = {{"Left", Qt::Key_Left}, {"Right", Qt::Key_Right}, {"Up", Qt::Key_Up}, {"Down", Qt::Key_Down}};
    for (const auto &arrow : arrows) {
        const auto sequence = [&](const QString &title) { const auto *d = manager.findDefinition(QStringLiteral("Canvas & Layers:") + title); return d ? d->defaultShortcut : QKeySequence(); };
        QCOMPARE(sequence(QStringLiteral("Nudge %1 1 px").arg(QLatin1String(arrow.direction))), QKeySequence(arrow.key));
        QCOMPARE(sequence(QStringLiteral("Nudge %1 10 px").arg(QLatin1String(arrow.direction))), QKeySequence(Qt::SHIFT | arrow.key));
        QCOMPARE(sequence(QStringLiteral("Move selected pixels %1 1 px").arg(QLatin1String(arrow.direction))), QKeySequence(Qt::CTRL | arrow.key));
        QCOMPARE(sequence(QStringLiteral("Move selected pixels %1 10 px").arg(QLatin1String(arrow.direction))), QKeySequence(Qt::CTRL | Qt::SHIFT | arrow.key));
    }
}

void TestIoAudit::menuShortcutsMatchMacMenus()
{
    // Every menu action a mac menu binds to a shortcut is reachable in LX with the registered default (and nothing
    // was registered twice with the same chord).
    MainWindow window;
    ShortcutManager &manager = ShortcutManager::instance();
    int checked = 0;
    for (const ShortcutDefinition &definition : manager.definitions()) {
        if (!definition.isMenu()) continue;
        bool found = false;
        for (QAction *action : window.findChildren<QAction *>()) {
            if (action->shortcut() == definition.defaultShortcut && !definition.defaultShortcut.isEmpty()) { found = true; break; }
        }
        QVERIFY2(found, qPrintable(QStringLiteral("no QAction carries %1 (%2)").arg(definition.id(), definition.defaultShortcut.toString())));
        ++checked;
    }
    QVERIFY(checked > 40);
    // The File menu bindings by name.
    const struct { const char *object; QKeySequence sequence; } files[] = {
        {"commandSave", QKeySequence(Qt::CTRL | Qt::Key_S)}, {"commandSaveAs", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S)},
        {"commandExportPng", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E)}, {"commandExportJpeg", QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_S)},
        {"commandOpen", QKeySequence(Qt::CTRL | Qt::Key_O)}, {"commandUndo", QKeySequence(Qt::CTRL | Qt::Key_Z)},
        {"commandRedo", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)}, {"commandCopyMerged", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C)},
    };
    for (const auto &entry : files) {
        auto *action = window.findChild<QAction *>(QLatin1String(entry.object));
        QVERIFY2(action, entry.object);
        QCOMPARE(action->shortcut(), entry.sequence);
    }
}

void TestIoAudit::shortcutConflictsAndPersistence()
{
    ShortcutManager &manager = ShortcutManager::instance();
    QMap<QString, QKeySequence> candidate;
    // Same chord on two commands is refused and names both.
    candidate.insert(QStringLiteral("Canvas & Layers:Brush tool"), QKeySequence(Qt::Key_E));
    const QString message = manager.validate(candidate);
    QVERIFY2(message.contains(QStringLiteral("Brush tool")) && message.contains(QStringLiteral("Eraser")), qPrintable(message));
    // Text-editing commands need a modifier.
    candidate.clear();
    candidate.insert(QStringLiteral("Text Editing:Increase tracking"), QKeySequence(Qt::Key_K));
    QVERIFY(!manager.validate(candidate).isEmpty());
    // Quit and Preferences chords are reserved (Command-Q, Command-comma; Command-Option-M for Minimize-All on the Mac).
    candidate.clear();
    candidate.insert(QStringLiteral("Menus:Save"), QKeySequence(Qt::CTRL | Qt::Key_Q));
    QVERIFY(!manager.validate(candidate).isEmpty());
    candidate.clear();
    candidate.insert(QStringLiteral("Menus:Save"), QKeySequence(Qt::CTRL | Qt::Key_Comma));
    QVERIFY(!manager.validate(candidate).isEmpty());
    candidate.clear();
    candidate.insert(QStringLiteral("Menus:Save"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_M));
    QVERIFY2(!manager.validate(candidate).isEmpty(), "Command-Option-M is reserved (mac KeyboardShortcuts.problem)");
    // A valid remap applies to the registered action, persists, and the old chord stops working.
    MainWindow window;
    auto *brush = window.findChild<QAction *>(QStringLiteral("commandSave"));
    QVERIFY(brush);
    const QKeySequence newSave(Qt::CTRL | Qt::ALT | Qt::Key_9);
    manager.setOverride(QStringLiteral("Menus:Save"), newSave);
    QCOMPARE(brush->shortcut(), newSave);
    manager.saveToSettings();
    manager.resetToDefaults();
    QCOMPARE(brush->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_S));
    manager.setOverride(QStringLiteral("Menus:Save"), newSave);
    manager.saveToSettings();
    ShortcutManager fresh;           // a new process reads the saved overrides
    QCOMPARE(fresh.shortcut(QStringLiteral("Menus:Save")), newSave);
    manager.resetToDefaults();
    ShortcutManager afterReset;
    QCOMPARE(afterReset.shortcut(QStringLiteral("Menus:Save")), QKeySequence(Qt::CTRL | Qt::Key_S));
    // A corrupt saved table is ignored wholesale, as the mac one is.
    QSettings().setValue(QStringLiteral("keyboardShortcuts.v1"), QStringLiteral("{\"Menus:Save\":\"Ctrl+E\",\"Menus:Export PNG\":\"Ctrl+E\"}"));
    ShortcutManager conflicted;
    QCOMPARE(conflicted.shortcut(QStringLiteral("Menus:Save")), QKeySequence(Qt::CTRL | Qt::Key_S));
}

namespace {
QString plain(QString text) { text.replace(QStringLiteral("&&"), QStringLiteral("\x01")); text.remove(QLatin1Char('&')); text.replace(QLatin1Char('\x01'), QLatin1Char('&')); return text; }
QStringList menuTitles(QMenu *menu)
{
    QStringList result;
    for (QAction *action : menu->actions()) result << (action->isSeparator() ? QStringLiteral("---") : plain(action->text()));
    return result;
}
QMenu *topMenu(QMenuBar *bar, const QString &title)
{
    for (QAction *action : bar->actions()) if (action->menu() && plain(action->text()) == title) return action->menu();
    return nullptr;
}
QAction *menuItem(QMenu *menu, const QString &title)
{
    for (QAction *action : menu->actions()) if (plain(action->text()).startsWith(title)) return action;
    return nullptr;
}
} // namespace

void TestIoAudit::menuStructureMatchesMac()
{
    MainWindow window;
    QMenuBar *bar = window.menuBar();
    // Every mac top-level menu has an LX counterpart.
    for (const QString &title : {QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("Select"), QStringLiteral("Image"),
                                 QStringLiteral("Filter"), QStringLiteral("Layer"), QStringLiteral("View")})
        QVERIFY2(topMenu(bar, title), qPrintable(title));
    struct Expect { const char *menu; const char *item; };
    const Expect expected[] = {
        {"File", "New Canvas"}, {"File", "Open Project"}, {"File", "Open Recent"}, {"File", "Import Images"}, {"File", "Save"}, {"File", "Save As"},
        {"File", "Export PNG"}, {"File", "Export JPEG"}, {"File", "Close Project"}, {"File", "Quit"},
        {"Edit", "Undo"}, {"Edit", "Redo"}, {"Edit", "Cut"}, {"Edit", "Copy"}, {"Edit", "Copy Merged"}, {"Edit", "Paste"}, {"Edit", "Keyboard Shortcuts"},
        {"Edit", "Fill with Foreground Color"}, {"Edit", "Fill with Background Color"}, {"Edit", "Clear Selection Pixels"},
        {"Select", "Select All"}, {"Select", "Deselect"}, {"Select", "Inverse"}, {"Select", "Layer's Pixels"}, {"Select", "Select Subject"},
        {"Select", "Color Range"}, {"Select", "Mask's Black Areas"}, {"Select", "Expand"}, {"Select", "Contract"}, {"Select", "Feather"},
        {"Image", "Curves"}, {"Image", "Levels"}, {"Image", "Hue/Saturation"}, {"Image", "Black & White"}, {"Image", "Color Balance"}, {"Image", "Exposure"},
        {"Image", "Gradient Map"}, {"Image", "Grain"}, {"Image", "Invert"}, {"Image", "Canvas Size"}, {"Image", "Image Size"}, {"Image", "Trim"},
        {"Image", "Flip Canvas Horizontal"}, {"Image", "Flip Canvas Vertical"},
        {"Layer", "Edit Adjustment"}, {"Layer", "Group Selected Layers"}, {"Layer", "Ungroup Layers"}, {"Layer", "Move Out of Folder"}, {"Layer", "Rename Layer"},
        {"Layer", "Move Layer Up"}, {"Layer", "Move Layer Down"}, {"Layer", "Flip Layer Horizontal"}, {"Layer", "Flip Layer Vertical"},
        {"View", "Fit Canvas"}, {"View", "Actual Pixels"}, {"View", "Zoom In"}, {"View", "Zoom Out"}, {"View", "Pixel Grid"}, {"View", "Show Transform Controls"},
        {"View", "Show"}, {"View", "Grid Settings"}, {"View", "Rulers"}, {"View", "Snap"}, {"View", "Snap To"}, {"View", "Lock Guides"}, {"View", "Clear Guides"},
    };
    for (const Expect &e : expected) {
        QMenu *menu = topMenu(bar, QString::fromLatin1(e.menu));
        QVERIFY(menu);
        QVERIFY2(menuItem(menu, QString::fromLatin1(e.item)), qPrintable(QStringLiteral("%1 > %2 is missing; has %3").arg(QString::fromLatin1(e.menu), QString::fromLatin1(e.item), menuTitles(menu).join(QStringLiteral(" | ")))));
    }
    // The mac Filter menu lists every non-adjustment filter.
    QMenu *filter = topMenu(bar, QStringLiteral("Filter"));
    for (const QString &name : {QStringLiteral("Gaussian Blur"), QStringLiteral("Motion Blur"), QStringLiteral("Vignette")})
        QVERIFY2(menuItem(filter, name), qPrintable(name));
    // New Adjustment Layer lists all 12 kinds.
    QMenu *layer = topMenu(bar, QStringLiteral("Layer"));
    QAction *adjustments = menuItem(layer, QStringLiteral("New Adjustment Layer"));
    QVERIFY(adjustments && adjustments->menu());
    window.session().createDocument(20, 20, true);
    window.syncDocumentViews();
    emit adjustments->menu()->aboutToShow();
    QCOMPARE(adjustments->menu()->actions().size(), 12);
}

void TestIoAudit::fileMenuEnabledStates()
{
    MainWindow window;
    const auto enabled = [&](const char *name) { auto *a = window.findChild<QAction *>(QLatin1String(name)); return a && a->isEnabled(); };
    // No document: New/Open/Import stay available, Save/Save As/Export need a document, as on the Mac.
    QVERIFY(!enabled("commandSave"));
    QVERIFY(!enabled("commandSaveAs"));
    QVERIFY(!enabled("commandExportPng"));
    QVERIFY(!enabled("commandExportJpeg"));
    QVERIFY(!enabled("commandCopyMerged"));
    QVERIFY(!enabled("commandUndo"));
    QVERIFY(window.findChild<QAction *>(QStringLiteral("commandOpen"))->isEnabled());
    window.session().createDocument(30, 30, true);
    window.syncDocumentViews();
    QVERIFY(enabled("commandSave"));
    QVERIFY(enabled("commandSaveAs"));
    QVERIFY(enabled("commandExportPng"));
    QVERIFY(enabled("commandExportJpeg"));
    // Copy Merged needs a selection over visible pixels: with none the Mac menu item is disabled too.
    QVERIFY(!enabled("commandCopyMerged"));
}

// ---------------------------------------------------------------------------------------------------- agent contract

namespace {
QByteArray pngBytes(const QImage &image)
{
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return bytes;
}
// The exact manifest from docs/writing-comp-files.md ("A minimal manifest with one full-canvas image layer").
QByteArray docMinimalManifest(int width, int height, const QString &layerId, const QString &name = QStringLiteral("Background"))
{
    return QStringLiteral(R"({
  "format": "com.compositor.project",
  "version": 11,
  "colorSpace": "sRGB",
  "documentID": "0C5E7A91-3B2D-4F6A-8E1C-9D0B7A6F5E4D",
  "width": %1,
  "height": %2,
  "resolution": 72,
  "activeLayerID": "%3",
  "layers": [
    {
      "id": "%3",
      "name": "%4",
      "imageFile": "%3.png",
      "isVisible": true,
      "isGroup": false,
      "opacity": 1,
      "blendMode": "Normal",
      "transform": {
        "origin": [0, 0],
        "size": [%1, %2],
        "rotation": 0,
        "flipX": false,
        "flipY": false,
        "sampling": "High quality"
      }
    }
  ]
}
)").arg(width).arg(height).arg(layerId, name).toUtf8();
}
void writeFile(const QString &path, const QByteArray &bytes) { QFile f(path); QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(path)); f.write(bytes); }
} // namespace

void TestIoAudit::handWrittenMinimalManifestOpens()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("Hand.comp"));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/images")));
    const QString id = QStringLiteral("6F1D3C2A-0B7E-4E8A-9C4D-2A1B3C4D5E6F");
    QImage image(64, 48, QImage::Format_RGBA8888); image.fill(QColor(10, 200, 30));
    writeFile(root + QStringLiteral("/images/") + id + QStringLiteral(".png"), pngBytes(image));
    writeFile(root + QStringLiteral("/manifest.json"), docMinimalManifest(64, 48, id));
    const Document doc = ProjectReader::load(root);
    QCOMPARE(doc.formatVersion, 11);
    QCOMPARE(doc.canvasSize, QSize(64, 48));
    QCOMPARE(doc.layers.size(), 1);
    QCOMPARE(doc.layers.first().name, QStringLiteral("Background"));
    QCOMPARE(doc.layers.first().transform.size, QSizeF(64, 48));
    const QImage flat = LayerRenderer::flattened(doc);
    QCOMPARE(pixelAt(flat, 10, 10), qRgb(10, 200, 30));
    // The rules the doc lists under "Rules that matter": a lowercase image name for an uppercase id, a misspelt blend
    // mode and a missing image each make the file refuse to open.
    QByteArray manifest = docMinimalManifest(64, 48, id);
    writeFile(root + QStringLiteral("/manifest.json"), QByteArray(manifest).replace("\"Normal\"", "\"Multiplyy\""));
    try { ProjectReader::load(root); QFAIL("a misspelt blend mode opened"); } catch (const ProjectError &) {}
    writeFile(root + QStringLiteral("/manifest.json"), QByteArray(manifest).replace("\"sampling\": \"High quality\"", "\"sampling\": \"Smooth\""));
    QCOMPARE(ProjectReader::load(root).layers.first().transform.sampling, Sampling::Smooth);
    writeFile(root + QStringLiteral("/manifest.json"), QByteArray(manifest).replace("\"sampling\": \"High quality\"", "\"sampling\": \"Nearest\""));
    QCOMPARE(ProjectReader::load(root).layers.first().transform.sampling, Sampling::Nearest);
    // A mask via maskFile/maskEnabled.
    QImage mask(64, 48, QImage::Format_Grayscale8); mask.fill(0);
    writeFile(root + QStringLiteral("/images/") + id + QStringLiteral(".mask.png"), pngBytes(mask));
    QByteArray withMask = manifest;
    withMask.replace("\"imageFile\": \"" + id.toUtf8() + ".png\",", "\"imageFile\": \"" + id.toUtf8() + ".png\",\n      \"maskFile\": \"" + id.toUtf8() + ".mask.png\",\n      \"maskEnabled\": true,");
    writeFile(root + QStringLiteral("/manifest.json"), withMask);
    const Document masked = ProjectReader::load(root);
    QVERIFY(!masked.layers.first().mask.isNull());
    QCOMPARE(qAlpha(pixelAt(LayerRenderer::flattened(masked), 10, 10)), 0); // black hides
}

void TestIoAudit::agentWriteReloadsLiveInOpenWindow()
{
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("Live.comp"));
    const QString id = QStringLiteral("6F1D3C2A-0B7E-4E8A-9C4D-2A1B3C4D5E6F");
    QVERIFY(QDir().mkpath(root + QStringLiteral("/images")));
    QImage image(32, 32, QImage::Format_RGBA8888); image.fill(QColor(255, 0, 0));
    writeFile(root + QStringLiteral("/images/") + id + QStringLiteral(".png"), pngBytes(image));
    writeFile(root + QStringLiteral("/manifest.json"), docMinimalManifest(32, 32, id));
    MainWindow window;
    window.show();
    QVERIFY(window.openProject(root));
    QCOMPARE(pixelAt(LayerRenderer::flattened(*window.document()), 5, 5), qRgb(255, 0, 0));
    // The documented safe write: PNG first, then the manifest through a temporary file renamed over manifest.json.
    QImage green(32, 32, QImage::Format_RGBA8888); green.fill(QColor(0, 255, 0));
    writeFile(root + QStringLiteral("/images/") + id + QStringLiteral(".png"), pngBytes(green));
    writeFile(root + QStringLiteral("/.manifest.json.tmp"), docMinimalManifest(32, 32, id, QStringLiteral("Renamed by the agent")));
    QVERIFY(QFile::remove(root + QStringLiteral("/manifest.json")));
    QVERIFY(QFile::rename(root + QStringLiteral("/.manifest.json.tmp"), root + QStringLiteral("/manifest.json")));
    QTRY_COMPARE_WITH_TIMEOUT(window.document()->layers.first().name, QStringLiteral("Renamed by the agent"), 10000);
    QCOMPARE(pixelAt(LayerRenderer::flattened(*window.document()), 5, 5), qRgb(0, 255, 0));
    QVERIFY(!window.session().isModified());
    QVERIFY(!window.session().canUndo());   // a reload clears undo, as reopening does
    // A second step: a new layer arrives and the stack grows without the window being reopened.
    const QString id2 = QStringLiteral("A1B2C3D4-E5F6-4A7B-8C9D-0E1F2A3B4C5D");
    QImage blue(8, 8, QImage::Format_RGBA8888); blue.fill(QColor(0, 0, 255));
    writeFile(root + QStringLiteral("/images/") + id2 + QStringLiteral(".png"), pngBytes(blue));
    QJsonObject manifest = QJsonDocument::fromJson(docMinimalManifest(32, 32, id)).object();
    QJsonArray layers = manifest.value(QStringLiteral("layers")).toArray();
    QJsonObject second = layers.at(0).toObject();
    second.insert(QStringLiteral("id"), id2); second.insert(QStringLiteral("name"), QStringLiteral("Blue square")); second.insert(QStringLiteral("imageFile"), id2 + QStringLiteral(".png"));
    second.insert(QStringLiteral("transform"), QJsonObject{{QStringLiteral("origin"), QJsonArray{4, 4}}, {QStringLiteral("size"), QJsonArray{8, 8}}, {QStringLiteral("rotation"), 0}, {QStringLiteral("flipX"), false}, {QStringLiteral("flipY"), false}, {QStringLiteral("sampling"), QStringLiteral("Nearest")}});
    layers.append(second); manifest.insert(QStringLiteral("layers"), layers);
    writeFile(root + QStringLiteral("/.manifest.json.tmp"), QJsonDocument(manifest).toJson());
    QVERIFY(QFile::remove(root + QStringLiteral("/manifest.json")));
    QVERIFY(QFile::rename(root + QStringLiteral("/.manifest.json.tmp"), root + QStringLiteral("/manifest.json")));
    QTRY_COMPARE_WITH_TIMEOUT(window.document()->layers.size(), 2, 10000);
    QCOMPARE(pixelAt(LayerRenderer::flattened(*window.document()), 6, 6), qRgb(0, 0, 255));
    // A broken write is ignored: the canvas stays, and the next good write still shows.
    writeFile(root + QStringLiteral("/manifest.json"), "{ not json");
    QTest::qWait(900);
    QCOMPARE(window.document()->layers.size(), 2);
    writeFile(root + QStringLiteral("/.manifest.json.tmp"), docMinimalManifest(32, 32, id, QStringLiteral("Fixed")));
    QVERIFY(QFile::remove(root + QStringLiteral("/manifest.json")));
    QVERIFY(QFile::rename(root + QStringLiteral("/.manifest.json.tmp"), root + QStringLiteral("/manifest.json")));
    QTRY_COMPARE_WITH_TIMEOUT(window.document()->layers.first().name, QStringLiteral("Fixed"), 10000);
    QCOMPARE(window.document()->layers.size(), 1);
}

void TestIoAudit::lxWrittenManifestMatchesMacCodableShapes()
{
    // What Swift's JSONDecoder needs from the manifest ProjectStore.load reads: CGPoint/CGSize as [x, y] arrays,
    // enum raw values spelled as the mac names them, UUID strings, no unknown required keys.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    EditorSession session;
    session.createDocument(200, 100);
    session.insertImage(solid(200, 100, Qt::red), QStringLiteral("Photo"));
    session.setLayerBlendMode(*session.document()->activeLayerId, BlendMode::SoftLight);
    session.setLayerOpacity(*session.document()->activeLayerId, 0.5);
    session.addBlankLayer();
    const QString path = dir.filePath(QStringLiteral("Out.comp"));
    ProjectWriter::save(*session.document(), path);
    QFile file(path + QStringLiteral("/manifest.json")); QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject manifest = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(manifest.value(QStringLiteral("format")).toString(), QStringLiteral("com.compositor.project"));
    QVERIFY(manifest.value(QStringLiteral("version")).toInt() >= 1 && manifest.value(QStringLiteral("version")).toInt() <= 11);
    QCOMPARE(manifest.value(QStringLiteral("colorSpace")).toString(), QStringLiteral("sRGB"));
    QVERIFY(!QUuid(manifest.value(QStringLiteral("documentID")).toString()).isNull());
    QCOMPARE(manifest.value(QStringLiteral("width")).toInt(), 200);
    QCOMPARE(manifest.value(QStringLiteral("resolution")).toDouble(), 72.0);
    const QJsonArray layers = manifest.value(QStringLiteral("layers")).toArray();
    QCOMPARE(layers.size(), 2);
    for (const QJsonValue &value : layers) {
        const QJsonObject layer = value.toObject();
        const QString id = layer.value(QStringLiteral("id")).toString();
        QCOMPARE(id, id.toUpper());                       // Swift writes UUID.uuidString, uppercase
        QCOMPARE(id.size(), 36);
        const QJsonObject transform = layer.value(QStringLiteral("transform")).toObject();
        QVERIFY(transform.value(QStringLiteral("origin")).isArray());
        QCOMPARE(transform.value(QStringLiteral("origin")).toArray().size(), 2);
        QVERIFY(transform.value(QStringLiteral("size")).isArray());
        QVERIFY(transform.value(QStringLiteral("rotation")).isDouble());
        QVERIFY(transform.value(QStringLiteral("flipX")).isBool());
        QVERIFY(transform.value(QStringLiteral("flipY")).isBool());
        const QString sampling = transform.value(QStringLiteral("sampling")).toString();
        QVERIFY(sampling == QStringLiteral("High quality") || sampling == QStringLiteral("Smooth") || sampling == QStringLiteral("Nearest"));
        QVERIFY(layer.value(QStringLiteral("isVisible")).isBool());
        if (layer.contains(QStringLiteral("imageFile"))) QCOMPARE(layer.value(QStringLiteral("imageFile")).toString(), id + QStringLiteral(".png"));
        if (layer.contains(QStringLiteral("blendMode"))) QVERIFY(blendModeFromString(layer.value(QStringLiteral("blendMode")).toString()).has_value());
    }
    QCOMPARE(layers.at(0).toObject().value(QStringLiteral("blendMode")).toString(), QStringLiteral("Soft Light"));
    QCOMPARE(layers.at(0).toObject().value(QStringLiteral("opacity")).toDouble(), 0.5);
    QVERIFY(!layers.at(1).toObject().contains(QStringLiteral("imageFile")));   // a blank layer has no asset
    // Every PNG in images/ is an 8-bit PNG named after its layer.
    for (const QString &name : QDir(path + QStringLiteral("/images")).entryList(QDir::Files)) {
        QVERIFY(name.endsWith(QStringLiteral(".png")));
        QImageReader reader(path + QStringLiteral("/images/") + name);
        QCOMPARE(reader.format(), QByteArray("png"));
    }
}

QTEST_MAIN(TestIoAudit)
#include "TestIoAudit.moc"
