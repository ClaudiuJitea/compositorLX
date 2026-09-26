#include "io/SvgImporter.h"
#include "core/EditorSession.h"
#include "ui/MainWindow.h"

#include <QColorSpace>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTest>
#include <QTimer>

namespace compositor {

class TestSvgImport : public QObject {
    Q_OBJECT

private:
    QString fixturePath(const QString &relative) const {
        return QDir(QStringLiteral(FIXTURES_DIR)).filePath(QStringLiteral("svg/") + relative);
    }

private slots:
    void testSvgMatchesAndHeaderSniffing();
    void testSvgProbeAndViewBoxParsing();
    void testSvgComputeTargetSizeAspectRatio();
    void testSvgReadRasterizationAndSrgbColors();
    void testSvgLimitsAndFailureBehavior();
    void testEditorSessionInsertSvgLifecycle();
    void testMainWindowSvgIntegration();
};

void TestSvgImport::testSvgMatchesAndHeaderSniffing()
{
    const QString validRect = fixturePath(QStringLiteral("valid/basic_rect.svg"));
    const QString validZ = fixturePath(QStringLiteral("valid/compressed.svgz"));
    const QString empty = fixturePath(QStringLiteral("malformed/empty.svg"));
    const QString notSvg = fixturePath(QStringLiteral("malformed/not_an_svg.svg"));
    const QString missing = fixturePath(QStringLiteral("does_not_exist.svg"));

    QVERIFY(SvgImporter::matches(validRect));
    QVERIFY(SvgImporter::matches(validZ));
    QVERIFY(!SvgImporter::matches(empty));
    QVERIFY(!SvgImporter::matches(notSvg));
    QVERIFY(!SvgImporter::matches(missing));

    // In-memory data matching
    const QByteArray xmlSvg = "<svg width=\"100\" height=\"100\"><circle/></svg>";
    const QByteArray withBOM = "\xEF\xBB\xBF<?xml version=\"1.0\"?><svg><rect/></svg>";
    const QByteArray notXml = "Some random string content";

    QVERIFY(SvgImporter::matchesData(xmlSvg));
    QVERIFY(SvgImporter::matchesData(withBOM));
    QVERIFY(!SvgImporter::matchesData(notXml));
    QVERIFY(!SvgImporter::matchesData(QByteArray{}));
}

void TestSvgImport::testSvgProbeAndViewBoxParsing()
{
    // 1. Probing basic_rect.svg
    const QString validRect = fixturePath(QStringLiteral("valid/basic_rect.svg"));
    SvgInfo info;
    QString error;
    QVERIFY(SvgImporter::probe(validRect, &info, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(info.intrinsicSize, QSize(200, 100));
    QVERIFY(info.hasViewBox);
    QCOMPARE(info.viewBox, QRectF(0, 0, 200, 100));
    QVERIFY(info.hasWidth);
    QVERIFY(info.hasHeight);

    // 2. Probing viewbox_only.svg
    const QString vbOnly = fixturePath(QStringLiteral("valid/viewbox_only.svg"));
    SvgInfo vbInfo;
    QVERIFY(SvgImporter::probe(vbOnly, &vbInfo, &error));
    QCOMPARE(vbInfo.intrinsicSize, QSize(400, 200));
    QVERIFY(vbInfo.hasViewBox);
    QCOMPARE(vbInfo.viewBox, QRectF(0, 0, 400, 200));
    QVERIFY(!vbInfo.hasWidth);
    QVERIFY(!vbInfo.hasHeight);

    // 3. parseViewBox unit tests
    bool ok = false;
    QRectF r = SvgImporter::parseViewBox(QStringLiteral("  0  0  300  150 "), &ok);
    QVERIFY(ok);
    QCOMPARE(r, QRectF(0, 0, 300, 150));

    r = SvgImporter::parseViewBox(QStringLiteral("10, 20, 300, 400"), &ok);
    QVERIFY(ok);
    QCOMPARE(r, QRectF(10, 20, 300, 400));

    r = SvgImporter::parseViewBox(QStringLiteral("-50 -100 200 400"), &ok);
    QVERIFY(ok);
    QCOMPARE(r, QRectF(-50, -100, 200, 400));

    // Invalid viewBox values
    r = SvgImporter::parseViewBox(QStringLiteral("0 0 0 100"), &ok);
    QVERIFY(!ok);

    r = SvgImporter::parseViewBox(QStringLiteral("0 0 100 -50"), &ok);
    QVERIFY(!ok);

    r = SvgImporter::parseViewBox(QStringLiteral("invalid coordinates"), &ok);
    QVERIFY(!ok);

    r = SvgImporter::parseViewBox(QStringLiteral(""), &ok);
    QVERIFY(!ok);

    // 4. parseLength unit tests
    QCOMPARE(SvgImporter::parseLength(QStringLiteral("100"), 0, &ok), 100.0);
    QVERIFY(ok);

    QCOMPARE(SvgImporter::parseLength(QStringLiteral("200px"), 0, &ok), 200.0);
    QVERIFY(ok);

    const double ptVal = SvgImporter::parseLength(QStringLiteral("72pt"), 0, &ok);
    QVERIFY(ok);
    QCOMPARE(std::round(ptVal), 96.0);

    const double inVal = SvgImporter::parseLength(QStringLiteral("2in"), 0, &ok);
    QVERIFY(ok);
    QCOMPARE(std::round(inVal), 192.0);

    const double pctVal = SvgImporter::parseLength(QStringLiteral("50%"), 800.0, &ok);
    QVERIFY(ok);
    QCOMPARE(pctVal, 400.0);

    // Invalid length
    QCOMPARE(SvgImporter::parseLength(QStringLiteral("-50"), 0, &ok), 0.0);
    QVERIFY(!ok);

    QCOMPARE(SvgImporter::parseLength(QStringLiteral("not-a-number"), 0, &ok), 0.0);
    QVERIFY(!ok);
}

void TestSvgImport::testSvgComputeTargetSizeAspectRatio()
{
    const QSize svgSize(1600, 900);

    // Case 1: Square canvas (800x800) -> width bound limits scale to 800/1600 = 0.5 -> 800x450
    const QSize targetSquare = SvgImporter::computeTargetSize(svgSize, QSize(800, 800));
    QCOMPARE(targetSquare, QSize(800, 450));
    QCOMPARE(double(targetSquare.width()) / targetSquare.height(), 1600.0 / 900.0);

    // Case 2: Tall portrait canvas (400x800) -> width bound limits scale to 400/1600 = 0.25 -> 400x225
    const QSize targetPortrait = SvgImporter::computeTargetSize(svgSize, QSize(400, 800));
    QCOMPARE(targetPortrait, QSize(400, 225));

    // Case 3: Wide landscape canvas (3200x1800) -> scale is 2.0 -> 3200x1800
    const QSize targetLarge = SvgImporter::computeTargetSize(svgSize, QSize(3200, 1800));
    QCOMPARE(targetLarge, QSize(3200, 1800));

    // Case 4: No canvas fitting (std::nullopt) -> preserves natural declared size
    const QSize targetNatural = SvgImporter::computeTargetSize(svgSize, std::nullopt);
    QCOMPARE(targetNatural, svgSize);

    // Case 5: 0 or negative fitting -> returns intrinsic size
    const QSize targetZero = SvgImporter::computeTargetSize(svgSize, QSize(0, 0));
    QCOMPARE(targetZero, svgSize);
}

void TestSvgImport::testSvgReadRasterizationAndSrgbColors()
{
    // 1. Natural size decode of basic_rect.svg
    const QString validRect = fixturePath(QStringLiteral("valid/basic_rect.svg"));
    QString error;
    QImage img = SvgImporter::read(validRect, std::nullopt, 100000000LL, &error);
    QVERIFY(!img.isNull());
    QVERIFY(error.isEmpty());
    QCOMPARE(img.size(), QSize(200, 100));
    QCOMPARE(img.format(), QImage::Format_RGBA8888_Premultiplied);
    QCOMPARE(img.colorSpace().primaries(), QColorSpace::Primaries::SRgb);

    // 2. Scaled decode with canvas fitting (100x100) -> should fit to 100x50
    QImage scaled = SvgImporter::read(validRect, QSize(100, 100), 100000000LL, &error);
    QVERIFY(!scaled.isNull());
    QCOMPARE(scaled.size(), QSize(100, 50));

    // 3. Compressed SVGZ decode
    const QString validZ = fixturePath(QStringLiteral("valid/compressed.svgz"));
    QImage imgZ = SvgImporter::read(validZ, std::nullopt, 100000000LL, &error);
    QVERIFY(!imgZ.isNull());
    QCOMPARE(imgZ.size(), QSize(200, 100));

    // 4. Color verification on srgb_colors.svg (100x100)
    const QString colorSvg = fixturePath(QStringLiteral("valid/srgb_colors.svg"));
    QImage colorImg = SvgImporter::read(colorSvg, std::nullopt, 100000000LL, &error);
    QVERIFY(!colorImg.isNull());
    QCOMPARE(colorImg.size(), QSize(100, 100));

    // Top-left: #FF0000 (red)
    const QColor cTopLeft = colorImg.pixelColor(25, 25);
    QVERIFY(cTopLeft.red() > 240);
    QVERIFY(cTopLeft.green() < 15);
    QVERIFY(cTopLeft.blue() < 15);
    QCOMPARE(cTopLeft.alpha(), 255);

    // Top-right: #00FF00 (green)
    const QColor cTopRight = colorImg.pixelColor(75, 25);
    QVERIFY(cTopRight.green() > 240);
    QVERIFY(cTopRight.red() < 15);
    QVERIFY(cTopRight.blue() < 15);
    QCOMPARE(cTopRight.alpha(), 255);

    // Bottom-left: #0000FF (blue)
    const QColor cBottomLeft = colorImg.pixelColor(25, 75);
    QVERIFY(cBottomLeft.blue() > 240);
    QVERIFY(cBottomLeft.red() < 15);
    QVERIFY(cBottomLeft.green() < 15);
    QCOMPARE(cBottomLeft.alpha(), 255);

    // Bottom-right: #FFFFFF (white)
    const QColor cBottomRight = colorImg.pixelColor(75, 75);
    QVERIFY(cBottomRight.red() > 240);
    QVERIFY(cBottomRight.green() > 240);
    QVERIFY(cBottomRight.blue() > 240);
    QCOMPARE(cBottomRight.alpha(), 255);
}

void TestSvgImport::testSvgLimitsAndFailureBehavior()
{
    QString error;

    // 1. Empty file
    const QString empty = fixturePath(QStringLiteral("malformed/empty.svg"));
    QImage imgEmpty = SvgImporter::read(empty, std::nullopt, 100000000LL, &error);
    QVERIFY(imgEmpty.isNull());
    QVERIFY(!error.isEmpty());

    // 2. Truncated XML
    const QString truncated = fixturePath(QStringLiteral("malformed/truncated.svg"));
    error.clear();
    QImage imgTruncated = SvgImporter::read(truncated, std::nullopt, 100000000LL, &error);
    QVERIFY(imgTruncated.isNull());
    QVERIFY(!error.isEmpty());

    // 3. Not an SVG
    const QString notSvg = fixturePath(QStringLiteral("malformed/not_an_svg.svg"));
    error.clear();
    QImage imgNotSvg = SvgImporter::read(notSvg, std::nullopt, 100000000LL, &error);
    QVERIFY(imgNotSvg.isNull());
    QVERIFY(!error.isEmpty());

    // 4. Zero dimensions
    const QString zeroDim = fixturePath(QStringLiteral("malformed/zero_dimensions.svg"));
    error.clear();
    QImage imgZero = SvgImporter::read(zeroDim, std::nullopt, 100000000LL, &error);
    QVERIFY(imgZero.isNull());
    QVERIFY(!error.isEmpty());

    // 5. Negative dimensions
    const QString negDim = fixturePath(QStringLiteral("malformed/negative_dimensions.svg"));
    error.clear();
    QImage imgNeg = SvgImporter::read(negDim, std::nullopt, 100000000LL, &error);
    QVERIFY(imgNeg.isNull());
    QVERIFY(!error.isEmpty());

    // 6. Huge dimensions (50,000 x 50,000 > MaxDimension 30,000)
    const QString huge = fixturePath(QStringLiteral("malformed/huge_dimensions.svg"));
    error.clear();
    QImage imgHuge = SvgImporter::read(huge, std::nullopt, 100000000LL, &error);
    QVERIFY(imgHuge.isNull());
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains(QStringLiteral("maximum allowed side")));

    // 7. Surface pixel budget limit (1600x900 = 1,440,000 px, limit = 500,000 px)
    const QString aspect169 = fixturePath(QStringLiteral("valid/aspect_16_9.svg"));
    error.clear();
    QImage imgBudget = SvgImporter::read(aspect169, std::nullopt, 500000LL, &error);
    QVERIFY(imgBudget.isNull());
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains(QStringLiteral("pixel budget")));
}

void TestSvgImport::testEditorSessionInsertSvgLifecycle()
{
    // 1. Insert into empty EditorSession (creates new document)
    EditorSession emptySession;
    QVERIFY(!emptySession.hasDocument());
    const quint64 initialRev = emptySession.sessionRevision();

    const QString validRect = fixturePath(QStringLiteral("valid/basic_rect.svg"));
    QString error;
    QVERIFY(emptySession.insertSvg(validRect, std::nullopt, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(emptySession.hasDocument());
    QCOMPARE(emptySession.document()->canvasSize, QSize(200, 100));
    QCOMPARE(emptySession.document()->layers.size(), 1);
    QCOMPARE(emptySession.document()->layers[0].image.size(), QSize(200, 100));
    QVERIFY(emptySession.sessionRevision() > initialRev);
    QVERIFY(emptySession.canUndo());

    // Test Undo / Redo
    const quint64 beforeUndoRev = emptySession.sessionRevision();
    emptySession.undo();
    QVERIFY(emptySession.sessionRevision() > beforeUndoRev);
    // After undoing the document creation edit
    emptySession.redo();
    QVERIFY(emptySession.hasDocument());
    QCOMPARE(emptySession.document()->layers.size(), 1);

    // 2. Insert into existing session with canvas fitting
    EditorSession canvasSession;
    canvasSession.createDocument(1000, 1000, false);
    QCOMPARE(canvasSession.document()->canvasSize, QSize(1000, 1000));
    QCOMPARE(canvasSession.document()->layers.size(), 0);

    const QString aspect169 = fixturePath(QStringLiteral("valid/aspect_16_9.svg")); // 1600x900
    QVERIFY(canvasSession.insertSvg(aspect169, std::nullopt, &error));
    QCOMPARE(canvasSession.document()->layers.size(), 1);

    // Should be scaled to fit 1000x1000 canvas -> 1000x563
    const Layer &insertedLayer = canvasSession.document()->layers[0];
    QCOMPARE(insertedLayer.image.width(), 1000);
    QVERIFY(insertedLayer.image.height() == 563 || insertedLayer.image.height() == 562);
    // Canvas dimensions unchanged
    QCOMPARE(canvasSession.document()->canvasSize, QSize(1000, 1000));
    // Layer origin centered
    QCOMPARE(insertedLayer.transform.origin.x(), 0.0);
    QVERIFY(insertedLayer.transform.origin.y() > 200.0 && insertedLayer.transform.origin.y() < 230.0);

    // 3. Failure on malformed SVG leaves session unmutated
    const quint64 revBeforeFail = canvasSession.sessionRevision();
    const int layersBeforeFail = canvasSession.document()->layers.size();
    const QString malformed = fixturePath(QStringLiteral("malformed/truncated.svg"));
    QVERIFY(!canvasSession.insertSvg(malformed, std::nullopt, &error));
    QCOMPARE(canvasSession.document()->layers.size(), layersBeforeFail);
    QCOMPARE(canvasSession.sessionRevision(), revBeforeFail);
}

void TestSvgImport::testMainWindowSvgIntegration()
{
    MainWindow window;

    // 1. openProject with valid SVG opens in tab
    const QString validRect = fixturePath(QStringLiteral("valid/basic_rect.svg"));
    QVERIFY(window.openProject(validRect));
    QVERIFY(window.session().hasDocument());
    QCOMPARE(window.session().document()->canvasSize, QSize(200, 100));
    QCOMPARE(window.session().document()->layers.size(), 1);

    // 2. importImageFiles with another SVG adds layer
    const QString aspect169 = fixturePath(QStringLiteral("valid/aspect_16_9.svg"));
    QVERIFY(window.importImageFiles({aspect169}));
    QCOMPARE(window.session().document()->layers.size(), 2);

    // 3. openProject with malformed SVG fails safely without crashing
    QTimer::singleShot(50, []() {
        QWidget *modal = QApplication::activeModalWidget();
        if (auto *dialog = qobject_cast<QDialog *>(modal)) {
            dialog->accept();
        }
    });
    const QString malformed = fixturePath(QStringLiteral("malformed/not_an_svg.svg"));
    QVERIFY(!window.openProject(malformed));
}

} // namespace compositor

QTEST_MAIN(compositor::TestSvgImport)
#include "TestSvgImport.moc"
