#include "io/ProjectReader.h"
#include "io/ProjectWriter.h"
#include "io/ImageExporter.h"
#include "rendering/LayerRenderer.h"
#include "rendering/DownsampleCache.h"
#include "rendering/SubjectRemoval.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace compositor;

class ProjectReaderTests final : public QObject {
    Q_OBJECT

private slots:
    void loadsValidProject();
    void rejectsUnsupportedVersion();
    void rejectsUnsafeImagePath();
    void appliesGrayscaleMaskAsAlpha();
    void savedProjectRoundTripsAllLayerMetadata();
    void downsampleLevelsAreSelectedAndReused();
    void downsampleAveragesFineStripes();
    void subjectModelProducesAFullSizeMask();
    void jpegEncodingUsesMatteQualityAndResolution();
};

namespace {

QJsonObject baseManifest(const QUuid &documentId, const QUuid &layerId)
{
    const QString imageName = layerId.toString(QUuid::WithoutBraces).toUpper() + QStringLiteral(".png");
    return {
        {QStringLiteral("format"), QStringLiteral("com.compositor.project")},
        {QStringLiteral("version"), 7},
        {QStringLiteral("colorSpace"), QStringLiteral("sRGB")},
        {QStringLiteral("documentID"), documentId.toString(QUuid::WithoutBraces)},
        {QStringLiteral("width"), 64},
        {QStringLiteral("height"), 32},
        {QStringLiteral("activeLayerID"), layerId.toString(QUuid::WithoutBraces)},
        {QStringLiteral("layers"), QJsonArray{QJsonObject{
             {QStringLiteral("id"), layerId.toString(QUuid::WithoutBraces)},
             {QStringLiteral("name"), QStringLiteral("Pixels")},
             {QStringLiteral("isVisible"), true},
             {QStringLiteral("transform"), QJsonObject{
                  {QStringLiteral("origin"), QJsonObject{{QStringLiteral("x"), 3}, {QStringLiteral("y"), 4}}},
                  {QStringLiteral("size"), QJsonObject{{QStringLiteral("width"), 8}, {QStringLiteral("height"), 6}}},
                  {QStringLiteral("rotation"), 0},
                  {QStringLiteral("flipX"), false},
                  {QStringLiteral("flipY"), false},
                  {QStringLiteral("sampling"), QStringLiteral("High quality")}}},
             {QStringLiteral("imageFile"), imageName}}}}
    };
}

void writeManifest(const QString &root, const QJsonObject &manifest)
{
    QFile file(root + QStringLiteral("/manifest.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(QJsonDocument(manifest).toJson()), qint64(QJsonDocument(manifest).toJson().size()));
}

} // namespace

void ProjectReaderTests::loadsValidProject()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("images")));
    const QUuid documentId = QUuid::createUuid();
    const QUuid layerId = QUuid::createUuid();
    const QJsonObject manifest = baseManifest(documentId, layerId);
    writeManifest(directory.path(), manifest);
    const QString imageName = layerId.toString(QUuid::WithoutBraces).toUpper() + QStringLiteral(".png");
    QImage image(8, 6, QImage::Format_RGBA8888);
    image.fill(QColor(10, 20, 30, 200));
    QVERIFY(image.save(directory.path() + QStringLiteral("/images/") + imageName, "PNG"));

    const Document loaded = ProjectReader::load(directory.path());
    QCOMPARE(loaded.id, documentId);
    QCOMPARE(loaded.canvasSize, QSize(64, 32));
    QCOMPARE(loaded.layers.size(), 1);
    QCOMPARE(loaded.layers.constFirst().image.size(), QSize(8, 6));
    QCOMPARE(loaded.layers.constFirst().name, QStringLiteral("Pixels"));
}

void ProjectReaderTests::rejectsUnsupportedVersion()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QJsonObject manifest = baseManifest(QUuid::createUuid(), QUuid::createUuid());
    manifest[QStringLiteral("version")] = 99;
    writeManifest(directory.path(), manifest);
    QVERIFY_EXCEPTION_THROWN((void)ProjectReader::load(directory.path()), ProjectError);
}

void ProjectReaderTests::rejectsUnsafeImagePath()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QUuid layerId = QUuid::createUuid();
    QJsonObject manifest = baseManifest(QUuid::createUuid(), layerId);
    QJsonArray layers = manifest[QStringLiteral("layers")].toArray();
    QJsonObject layer = layers[0].toObject();
    layer[QStringLiteral("imageFile")] = QStringLiteral("../escape.png");
    layers[0] = layer;
    manifest[QStringLiteral("layers")] = layers;
    writeManifest(directory.path(), manifest);
    QVERIFY_EXCEPTION_THROWN((void)ProjectReader::load(directory.path()), ProjectError);
}

void ProjectReaderTests::appliesGrayscaleMaskAsAlpha()
{
    QImage image(2, 1, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    QImage mask(2, 1, QImage::Format_Grayscale8);
    mask.setPixelColor(0, 0, Qt::black);
    mask.setPixelColor(1, 0, Qt::white);

    const QImage result = LayerRenderer::applyMask(image, mask, Sampling::Nearest)
                              .convertToFormat(QImage::Format_RGBA8888);
    QCOMPARE(result.pixelColor(0, 0).alpha(), 0);
    QCOMPARE(result.pixelColor(1, 0).alpha(), 255);
}

void ProjectReaderTests::savedProjectRoundTripsAllLayerMetadata()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    Document source;
    source.formatVersion = 7;
    source.id = QUuid::createUuid();
    source.canvasSize = QSize(320, 180);
    source.resolution = 144;
    Layer group;
    group.id = QUuid::createUuid();
    group.name = QStringLiteral("Folder 1");
    group.group = true;
    group.transform.size = source.canvasSize;
    Layer pixels;
    pixels.id = QUuid::createUuid();
    pixels.name = QStringLiteral("Pixels");
    pixels.parentId = group.id;
    pixels.opacity = .625;
    pixels.blendMode = BlendMode::Multiply;
    pixels.transform.origin = QPointF(12, 23);
    pixels.transform.size = QSizeF(20, 10);
    pixels.transform.rotation = 17;
    pixels.image = QImage(20, 10, QImage::Format_RGBA8888_Premultiplied);
    pixels.image.fill(QColor(20, 40, 60, 200));
    pixels.mask = QImage(20, 10, QImage::Format_Grayscale8);
    pixels.mask.fill(127);
    pixels.maskPlacement = pixels.transform; pixels.maskPlacement->origin += QPointF(5, 2); pixels.maskLinked = false;
    Layer adjustment; adjustment.id = QUuid::createUuid(); adjustment.name = QStringLiteral("Exposure"); adjustment.transform.size = source.canvasSize;
    adjustment.adjustment = {{QStringLiteral("kind"), QStringLiteral("Exposure")}, {QStringLiteral("exposureSettings"), QJsonObject{
        {QStringLiteral("exposure"), 1.25}, {QStringLiteral("offset"), .1}, {QStringLiteral("gamma"), 1.2}}}};
    source.layers = {group, pixels, adjustment};
    source.activeLayerId = pixels.id;
    const QString path = directory.filePath(QStringLiteral("Roundtrip.comp"));

    ProjectWriter::save(source, path);
    const Document loaded = ProjectReader::load(path);
    QCOMPARE(loaded.id, source.id);
    QCOMPARE(loaded.canvasSize, source.canvasSize);
    QCOMPARE(loaded.resolution, source.resolution);
    QCOMPARE(loaded.activeLayerId, source.activeLayerId);
    QCOMPARE(loaded.layers.size(), 3);
    QCOMPARE(loaded.layers.at(1).parentId, std::optional<QUuid>(group.id));
    QCOMPARE(loaded.layers.at(1).opacity, .625);
    QCOMPARE(loaded.layers.at(1).blendMode, BlendMode::Multiply);
    QCOMPARE(loaded.layers.at(1).transform.rotation, 17.0);
    QCOMPARE(loaded.layers.at(1).image.pixelColor(0, 0).rgba(), QColor(20, 40, 60, 200).rgba());
    QCOMPARE(loaded.layers.at(1).mask.pixelColor(0, 0).red(), 127);
    QCOMPARE(loaded.layers.at(1).maskPlacement, pixels.maskPlacement); QVERIFY(!loaded.layers.at(1).maskLinked);
    QCOMPARE(loaded.layers.at(2).adjustment, adjustment.adjustment);
    QVERIFY(loaded.layers.at(2).image.isNull());
}

void ProjectReaderTests::downsampleLevelsAreSelectedAndReused()
{
    QCOMPARE(DownsampleCache::levelForFactor(.6), 0);
    QCOMPARE(DownsampleCache::levelForFactor(.3), 1);
    QCOMPARE(DownsampleCache::levelForFactor(.125), 3);
    QCOMPARE(DownsampleCache::levelForFactor(0), 0);
    QImage source(1024, 512, QImage::Format_RGBA8888_Premultiplied);
    source.fill(QColor(20, 40, 60));
    DownsampleCache::shared().clear();
    const QImage eighth = DownsampleCache::shared().image(source, .125);
    QCOMPARE(eighth.size(), QSize(128, 64));
    QCOMPARE(DownsampleCache::shared().image(source, .3).size(), QSize(512, 256));
    QCOMPARE(DownsampleCache::shared().image(source, .125).cacheKey(), eighth.cacheKey());
}

void ProjectReaderTests::downsampleAveragesFineStripes()
{
    QImage source(2048, 256, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        uchar *row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            const uchar value = x % 2 ? 255 : 0;
            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = value;
            row[x * 4 + 3] = 255;
        }
    }
    const QImage reduced = DownsampleCache::shared().image(source, .125).convertToFormat(QImage::Format_RGBA8888);
    double sum = 0, squared = 0;
    const uchar *row = reduced.constScanLine(reduced.height() / 2);
    for (int x = 4; x < reduced.width() - 4; ++x) { const double value = row[x * 4]; sum += value; squared += value * value; }
    const int count = reduced.width() - 8;
    const double mean = sum / count;
    const double deviation = std::sqrt(std::max(0.0, squared / count - mean * mean));
    QVERIFY2(std::abs(mean - 127.5) < 8, qPrintable(QStringLiteral("mean %1").arg(mean)));
    QVERIFY2(deviation < 6, qPrintable(QStringLiteral("deviation %1").arg(deviation)));
}

void ProjectReaderTests::subjectModelProducesAFullSizeMask()
{
    QImage source(96, 64, QImage::Format_RGBA8888_Premultiplied); source.fill(QColor(15, 25, 35));
    QPainter painter(&source); painter.setPen(Qt::NoPen); painter.setBrush(Qt::white); painter.drawEllipse(QRect(28, 8, 40, 50)); painter.end();
    QString error; const QImage mask = SubjectRemoval::rawMask(source, &error);
    QVERIFY2(!mask.isNull(), qPrintable(error)); QCOMPARE(mask.size(), source.size());
    const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8);
    int minimum = 255, maximum = 0;
    for (int y = 0; y < gray.height(); ++y) for (int x = 0; x < gray.width(); ++x) { minimum = std::min(minimum, int(gray.constScanLine(y)[x])); maximum = std::max(maximum, int(gray.constScanLine(y)[x])); }
    QVERIFY(maximum - minimum > 32);
}

void ProjectReaderTests::jpegEncodingUsesMatteQualityAndResolution()
{
    QImage source(128, 128, QImage::Format_RGBA8888_Premultiplied); source.fill(Qt::transparent);
    for (int y = 16; y < 112; ++y) for (int x = 16; x < 112; ++x)
        source.setPixelColor(x, y, QColor((x * 37 + y * 17) % 256, (x * 11 + y * 53) % 256, (x * 79 + y * 7) % 256));
    QString error;
    const auto low = ImageExporter::jpeg(source, 10, Qt::blue, 300, &error); QVERIFY2(low, qPrintable(error));
    const auto high = ImageExporter::jpeg(source, 100, Qt::blue, 300, &error); QVERIFY2(high, qPrintable(error));
    QVERIFY(low->data.size() < high->data.size());
    QCOMPARE(high->preview.size(), source.size());
    QCOMPARE(high->preview.pixelColor(0, 0).alpha(), 255);
    QVERIFY(high->preview.pixelColor(0, 0).blue() > 220);
    QVERIFY(std::abs(high->preview.dotsPerMeterX() - qRound(300 / .0254)) < 20);
}

QTEST_GUILESS_MAIN(ProjectReaderTests)
#include "ProjectReaderTests.moc"
