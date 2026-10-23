#include "core/CameraRaw.h"
#include "rendering/DeepImage.h"
#include "rendering/GpuOperations.h"
#include "rendering/GpuRenderer.h"
#include "rendering/LayerEffectsRenderer.h"
#include "rendering/LayerRenderer.h"
#include "rendering/ProgressiveBlur.h"
#include "rendering/RasterOperations.h"
#include <QColorSpace>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTest>
#include <future>
#include <iostream>
using namespace compositor;
static QImage fixture(QSize size, QImage::Format format) {
    QImage image(size, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        auto *p = reinterpret_cast<float *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            float a = ((x * 13 + y * 7) % 23) / 22.f;
            for (int c = 0; c < 3; ++c)
                p[x * 4 + c] = a * ((x * (c + 5) + y * 11 + c * 3) % 31) / 30.f;
            p[x * 4 + 3] = a;
        }
    }
    return image.convertToFormat(format);
}
static double error(const QImage &a, const QImage &b) {
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return INFINITY;
    auto x = DeepImage::floating(a), y = DeepImage::floating(b);
    double worst = 0;
    for (int row = 0; row < x.height(); ++row) {
        const auto *p = reinterpret_cast<const float *>(x.constScanLine(row)),
                   *q = reinterpret_cast<const float *>(y.constScanLine(row));
        for (int i = 0; i < x.width() * 4; ++i) {
            if (!std::isfinite(p[i]) || !std::isfinite(q[i]))
                return INFINITY;
            worst = std::max(worst, double(std::abs(p[i] - q[i])));
        }
    }
    return worst;
}
static QImage operation(const QImage &image, int op) {
    switch (op) {
    case 0:
        return RasterOperations::inverted(image);
    case 1:
        return RasterOperations::exposure(image, .75, .05, 1.2);
    case 2: {
        LevelsSettings s;
        s.ranges[0] = {12, 1.2, 239, 3, 250};
        return RasterOperations::levels(image, s);
    }
    case 3: {
        CurvesSettings s;
        s.channels[0] = {{0, 0}, {80, 100}, {170, 180}, {255, 255}};
        return RasterOperations::curves(image, s);
    }
    case 4:
        return RasterOperations::gaussianBlur(image, 2.5);
    case 5:
        return RasterOperations::gaussianBlur(image, 12.5);
    case 6:
        return RasterOperations::gaussianBlur(image, 60);
    case 7:
        return DeepImage::gaussianBlur(image, 3.5);
    case 8:
        return RasterOperations::bloomGlow(image, 35, 4);
    case 9:
        return RasterOperations::bloomGlow(image, 80, 15);
    case 10:
    case 11: {
        ProgressiveBlurSettings s;
        s.from = 2;
        s.to = 15;
        s.start = {9.5, 4};
        s.end = {33, 27};
        s.radial = op == 11;
        return s.apply(image);
    }
    case 12:
        return DeepImage::motionBlur(image, 31, 13);
    case 13: {
        CameraRawSettings s;
        s.exposure = .7;
        s.temperature = 15;
        return RasterOperations::cameraRaw(image, s);
    }
    case 14: {
        float weights[6] = {.4, .6, .4, .6, .2, .8};
        return RasterOperations::blackWhite(image, weights, true, 40, .2);
    }
    case 15: {
        float a[3] = {.2, -.15, .1}, b[3] = {.1, .1, -.1}, c[3] = {-.1, .1, .15};
        return RasterOperations::colorBalance(image, a, b, c, true);
    }
    case 16: {
        HueSaturationSettings s;
        s.adjustments[0] = {25, 30, -12};
        return RasterOperations::hueSaturation(image, s);
    }
    case 17: {
        HueSaturationSettings s;
        s.colorize = true;
        s.adjustments[0] = {220, 45, 15};
        return RasterOperations::hueSaturation(image, s);
    }
    case 18:
        return RasterOperations::gradientMap(image, QColor(37, 5, 65), QColor(240, 210, 176), true);
    case 19:
        return RasterOperations::motionBlur(image, -31, 13);
    default:
        return {};
    }
}
class TestGpuRenderer : public QObject {
    Q_OBJECT
  private:
    void imageMetadataTest(QImage image) {
        image.setColorSpace(QColorSpace::SRgb);
        image.setDotsPerMeterX(11811);
        image.setDotsPerMeterY(11812);
        image.setText("test", "preserved");
        const auto gpu = GpuOperations::gaussian(image, 2.5, true);
        QVERIFY(!gpu.isNull());
        QCOMPARE(gpu.colorSpace(), image.colorSpace());
        QCOMPARE(gpu.dotsPerMeterX(), 11811);
        QCOMPARE(gpu.dotsPerMeterY(), 11812);
        QCOMPARE(gpu.text("test"), QString("preserved"));
    }
  private slots:
    void initTestCase() {
        qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
        const auto image = fixture({9, 9}, QImage::Format_RGBA8888_Premultiplied);
        const auto gpu = GpuOperations::simple(image, GpuOp::Invert);
        if (gpu.isNull()) {
            QVERIFY2(!qEnvironmentVariableIsSet("COMPOSITOR_REQUIRE_VULKAN_TESTS"),
                     qPrintable(GpuRenderer::lastExecution().reason));
            QSKIP(qPrintable(GpuRenderer::lastExecution().reason));
        }
    }
    void transparentConversion() {
        QImage image(9, 7, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(120, 180, 200, 128));
        const auto wide = DeepImage::floating(image);
        QVERIFY(std::abs(wide.pixelColor(1, 1).alphaF() - 128. / 255) < .00001);
        const auto inverted = RasterOperations::inverted(image);
        QCOMPARE(inverted.pixelColor(1, 1).alpha(), 128);
    }
    void parity_data() {
        QTest::addColumn<int>("op");
        QTest::addColumn<int>("format");
        for (int op = 0; op < 20; ++op)
            for (auto format : {QImage::Format_RGBA8888_Premultiplied, QImage::Format_RGBA64_Premultiplied,
                                QImage::Format_RGBA32FPx4_Premultiplied})
                QTest::newRow(qPrintable(QString("op%1-format%2").arg(op).arg(int(format)))) << op << int(format);
    }
    void parity() {
        QFETCH(int, op);
        QFETCH(int, format);
        const auto image = fixture({37, 29}, QImage::Format(format));
        QImage cpu;
        {
            GpuRenderer::CpuScope forceCpu;
            cpu = operation(image, op);
        }
        GpuGraph{}.execute(-1, QImage::Format_RGBA8888_Premultiplied);
        const auto gpu = operation(image, op);
        const double difference = error(cpu, gpu);
        const double tolerance = format == QImage::Format_RGBA8888_Premultiplied ? 1.01 / 255
                                 : op == 10 || op == 11                          ? .001
                                                                                 : .00015;
        if (difference > tolerance)
            qWarning() << "pixels" << image.pixelColor(1, 1) << cpu.pixelColor(1, 1) << gpu.pixelColor(1, 1)
                       << "formats" << cpu.format() << gpu.format();
        QVERIFY2(difference <= tolerance,
                 qPrintable(QString("maximum channel error %1 (limit %2)").arg(difference, 0, 'g', 10).arg(tolerance)));
        if (!(op == 1 && format == QImage::Format_RGBA32FPx4_Premultiplied) &&
            !((op == 16 || op == 17) && format != QImage::Format_RGBA8888_Premultiplied))
            QVERIFY2(GpuRenderer::lastExecution().accelerated, qPrintable(GpuRenderer::lastExecution().reason));
    }
    void graphResidencyAndCache() {
        const auto image = fixture({257, 129}, QImage::Format_RGBA32FPx4_Premultiplied);
        GpuGraph graph;
        int in = graph.source(image);
        GpuParameters p{};
        p[0] = 1.5;
        p[2] = 1;
        p[3] = 1;
        int at = graph.pass(GpuOp::Exposure, in, -1, p);
        at = graph.gaussian(at, 3.5);
        at = graph.pass(GpuOp::Invert, at);
        GpuExecution first, second;
        const auto a = graph.execute(at, image.format(), RenderBackend::Vulkan, &first);
        const auto b = graph.execute(at, image.format(), RenderBackend::Vulkan, &second);
        QVERIFY(first.accelerated);
        QCOMPARE(a, b);
        QCOMPARE(first.dispatches, 4);
        QCOMPARE(second.uploadedBytes, quint64(0));
        QCOMPARE(second.downloadedBytes, quint64(image.width()) * image.height() * 16);
        QImage input = image;
        for (int y = 0; y < input.height(); ++y) {
            auto *p = reinterpret_cast<float *>(input.scanLine(y));
            for (int x = 0; x < input.width(); ++x)
                for (int c = 0; c < 3; ++c)
                    p[x * 4 + c] *= 1.5;
        }
        QImage cpu;
        {
            GpuRenderer::CpuScope forceCpu;
            cpu = DeepImage::gaussianBlur(input, 3.5);
        }
        for (int y = 0; y < cpu.height(); ++y) {
            auto *p = reinterpret_cast<float *>(cpu.scanLine(y));
            for (int x = 0; x < cpu.width(); ++x)
                for (int c = 0; c < 3; ++c)
                    p[x * 4 + c] = p[x * 4 + 3] - p[x * 4 + c];
        }
        QVERIFY(error(a, cpu) < .0001);
        const auto changed = fixture({129, 257}, image.format());
        GpuGraph other;
        int o = other.source(changed);
        o = other.pass(GpuOp::Invert, o);
        QVERIFY(!other.execute(o, image.format()).isNull());
        QCOMPARE(graph.execute(at, image.format()),
                 a); // Same byte count, different source layout must invalidate correctly.
    }
    void transformsAndMasks() {
        const auto image = fixture({31, 19}, QImage::Format_RGBA32FPx4_Premultiplied);
        GpuGraph graph;
        GpuParameters t{};
        t[GpuParam::Transform::m11] = t[GpuParam::Transform::m22] = 1;
        t[GpuParam::Transform::sourceWidth] = image.width();
        t[GpuParam::Transform::sourceHeight] = image.height();
        t[GpuParam::Transform::opacity] = 1;
        const auto result =
            graph.execute(graph.pass(GpuOp::Transform, graph.source(image), -1, t, 0, {53, 37}), image.format());
        QVERIFY(!result.isNull());
        QCOMPARE(result.copy(image.rect()), image);
        QCOMPARE(result.pixelColor(50, 30).alpha(), 0);
        QImage mask(image.size(), QImage::Format_Grayscale8);
        mask.fill(128);
        GpuGraph masking;
        GpuParameters m{};
        m[GpuParam::Mask::factor] = 1;
        const auto covered = masking.execute(
            masking.pass(GpuOp::Mask, masking.source(image), masking.source(mask), m), image.format());
        QImage expected = image;
        for (int y = 0; y < image.height(); ++y) {
            auto *p = reinterpret_cast<float *>(expected.scanLine(y));
            for (int x = 0; x < image.width() * 4; ++x)
                p[x] *= 128.f / 255;
        }
        QVERIFY(error(covered, expected) < .00001);
    }
    void document_data() {
        QTest::addColumn<int>("blend");
        QTest::addColumn<bool>("deep");
        for (int blend = 0; blend < 24; ++blend)
            for (bool deep : {false, true})
                QTest::newRow(qPrintable(QString("blend%1-%2").arg(blend).arg(deep))) << blend << deep;
    }
    void document() {
        QFETCH(int, blend);
        QFETCH(bool, deep);
        Document document;
        document.canvasSize = {47, 39};
        Layer base;
        base.id = QUuid::createUuid();
        base.image =
            fixture({47, 39}, deep ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
        base.transform.size = base.image.size();
        Layer top = base;
        top.id = QUuid::createUuid();
        top.image = fixture({31, 23}, base.image.format());
        top.transform.size = top.image.size();
        top.transform.origin = {3, 7};
        top.opacity = .7;
        top.blendMode = BlendMode(blend);
        document.layers = {base, top};
        QImage cpu;
        {
            GpuRenderer::CpuScope scope;
            cpu = LayerRenderer::flattened(document);
        }
        const auto gpu = LayerRenderer::flattened(document);
        const double delta = error(cpu, gpu);
        QVERIFY2(delta <= (deep ? .00015 : 2.01 / 255),
                 qPrintable(QString("document error %1").arg(delta, 0, 'g', 10)));
        QVERIFY2(GpuRenderer::lastExecution().accelerated, qPrintable(GpuRenderer::lastExecution().reason));
    }
    void filterChain() {
        Document document;
        document.canvasSize = {69, 53};
        Layer base;
        base.id = QUuid::createUuid();
        base.image = fixture(document.canvasSize, QImage::Format_RGBA8888_Premultiplied);
        base.transform.size = base.image.size();
        document.layers.push_back(base);
        for (const auto &kind : {QString("Progressive Blur"), QString("RGB Split")}) {
            Layer filter;
            filter.id = QUuid::createUuid();
            filter.adjustment = {{"kind", kind},
                                 {"filterLayerSettings",
                                  QJsonObject{{"progressiveTo", 12}, {"rgbSplitDistance", 4}, {"rgbSplitBlur", 3}}}};
            document.layers.push_back(filter);
        }
        QImage cpu;
        {
            GpuRenderer::CpuScope scope;
            cpu = LayerRenderer::flattened(document);
        }
        const auto gpu = LayerRenderer::flattened(document);
        QVERIFY(error(cpu, gpu) <= 2.01 / 255);
        QVERIFY(GpuRenderer::lastExecution().dispatches > 40);
        QCOMPARE(GpuRenderer::lastExecution().downloadedBytes, quint64(69 * 53 * 4)); // Read back as RGBA8.
    }
    void adjustmentChain() {
        Document document;
        document.canvasSize = {257, 129};
        Layer base;
        base.id = QUuid::createUuid();
        base.image = fixture(document.canvasSize, QImage::Format_RGBA8888_Premultiplied);
        base.transform.size = base.image.size();
        document.layers.push_back(base);
        Layer exposure;
        exposure.id = QUuid::createUuid();
        exposure.adjustment =
            QJsonObject{{"kind", "Exposure"}, {"exposureSettings", QJsonObject{{"exposure", .7}, {"gamma", 1.1}}}};
        document.layers.push_back(exposure);
        Layer blur;
        blur.id = QUuid::createUuid();
        blur.adjustment = QJsonObject{{"kind", "Gaussian Blur"}, {"blurRadius", 3.5}};
        document.layers.push_back(blur);
        Layer invert;
        invert.id = QUuid::createUuid();
        invert.adjustment = QJsonObject{{"kind", "Invert"}};
        document.layers.push_back(invert);
        QImage cpu;
        {
            GpuRenderer::CpuScope scope;
            cpu = LayerRenderer::flattened(document);
        }
        auto gpu = LayerRenderer::flattened(document);
        QVERIFY(error(cpu, gpu) <= 2.01 / 255);
        QVERIFY(GpuRenderer::lastExecution().dispatches >= 10);
        QCOMPARE(GpuRenderer::lastExecution().downloadedBytes,
                 quint64(document.canvasSize.width()) * document.canvasSize.height() * 4);
    }
    void largeBlurAndTiling() {
        const auto image = fixture({1537, 1041}, QImage::Format_RGBA8888_Premultiplied);
        imageMetadataTest(image);
        qputenv("COMPOSITOR_GPU_MEMORY_MIB", "64");
        QImage cpu;
        {
            GpuRenderer::CpuScope scope;
            cpu = RasterOperations::gaussianBlur(image, 12.5);
        }
        const auto gpu = GpuOperations::gaussian(image, 12.5, true);
        QVERIFY2(!gpu.isNull(), qPrintable(GpuRenderer::lastExecution().reason));
        QVERIFY(error(cpu, gpu) <= 1.01 / 255);
        QVERIFY(GpuRenderer::lastExecution().workingBytes <= 64 * 1024 * 1024);
        qunsetenv("COMPOSITOR_GPU_MEMORY_MIB");
    }
    void effectCoverage() {
        const auto image = fixture({257, 131}, QImage::Format_RGBA8888_Premultiplied);
        for (double radius : {3.5, 23.}) {
            std::vector<float> cpu;
            {
                GpuRenderer::CpuScope scope;
                cpu = LayerEffectsRenderer::coverage(image, QRectF(3, 5, 257, 131), {269, 149}, radius);
            }
            const auto gpu = LayerEffectsRenderer::coverage(image, QRectF(3, 5, 257, 131), {269, 149}, radius);
            QCOMPARE(cpu.size(), gpu.size());
            for (size_t i = 0; i < cpu.size(); ++i)
                QVERIFY(std::abs(cpu[i] - gpu[i]) < .0001);
        }
        std::vector<float> coverage(257 * 131);
        for (size_t i = 0; i < coverage.size(); ++i)
            coverage[i] = (i % 29) / 28.f;
        for (bool smallest : {false, true}) {
            std::vector<float> cpu;
            {
                GpuRenderer::CpuScope scope;
                cpu = LayerEffectsRenderer::extreme(coverage, 257, 131, 7, smallest);
            }
            QCOMPARE(LayerEffectsRenderer::extreme(coverage, 257, 131, 7, smallest), cpu);
        }
    }
    void groupsAndMasks() {
        Document document;
        document.canvasSize = {79, 61};
        Layer base;
        base.id = QUuid::createUuid();
        base.image = fixture(document.canvasSize, QImage::Format_RGBA8888_Premultiplied);
        base.transform.size = base.image.size();
        Layer group;
        group.id = QUuid::createUuid();
        group.group = true;
        group.opacity = .6;
        group.transform.size = document.canvasSize;
        group.mask = QImage(document.canvasSize, QImage::Format_Grayscale8);
        group.mask.fill(191);
        group.maskEnabled = true;
        Layer top = base;
        top.id = QUuid::createUuid();
        top.parentId = group.id;
        top.transform.origin = {3, 5};
        top.mask = QImage(top.image.size(), QImage::Format_Grayscale8);
        top.mask.fill(151);
        top.maskEnabled = true;
        document.layers = {base, group, top};
        QImage cpu;
        {
            GpuRenderer::CpuScope scope;
            cpu = LayerRenderer::flattened(document);
        }
        const auto gpu = LayerRenderer::flattened(document);
        QVERIFY(error(cpu, gpu) <= 2.01 / 255);
    }
    void concurrency() {
        const auto image = fixture({127, 63}, QImage::Format_RGBA8888_Premultiplied);
        auto run = [&] { return GpuOperations::gaussian(image, 4, true); };
        auto a = std::async(std::launch::async, run), b = std::async(std::launch::async, run);
        QCOMPARE(a.get(), b.get());
    }
    void fallbackLimits() {
        const auto image = fixture({17, 15}, QImage::Format_RGBA8888_Premultiplied);
        GpuGraph graph;
        int at = graph.source(image);
        GpuParameters p{};
        p[0] = INFINITY;
        at = graph.pass(GpuOp::Exposure, at, -1, p);
        GpuExecution r;
        QVERIFY(graph.execute(at, image.format(), RenderBackend::Vulkan, &r).isNull());
        QVERIFY(!r.reason.isEmpty());
        QVERIFY(graph.execute(-1, image.format()).isNull());
    }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--fallback")) {
        qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
        const auto image = fixture({33, 31}, QImage::Format_RGBA8888_Premultiplied);
        const auto actual = operation(image, 4);
        const auto r = GpuRenderer::lastExecution();
        QImage reference;
        {
            GpuRenderer::CpuScope cpu;
            reference = operation(image, 4);
        }
        std::cout << qPrintable(r.reason) << std::endl;
        return !r.accelerated && !r.reason.isEmpty() && actual == reference ? 0 : 1;
    }
    if (app.arguments().contains("--benchmark")) {
        for (QSize size : {QSize(512, 512), QSize(1920, 1080)}) {
            const auto image = fixture(size, QImage::Format_RGBA8888_Premultiplied);
            for (int op : {1, 4, 5, 7, 8, 10, 13, 16, 19})
                for (const char *backend : {"cpu", "vulkan"}) {
                    qputenv("COMPOSITOR_RENDER_BACKEND", backend);
                    operation(image, op);
                    double timings[3];
                    for (double &t : timings) {
                        QElapsedTimer timer;
                        timer.start();
                        operation(image, op);
                        t = timer.nsecsElapsed() / 1e6;
                    }
                    std::sort(std::begin(timings), std::end(timings));
                    std::cout << size.width() << 'x' << size.height() << " op=" << op << ' ' << backend
                              << " median_ms=" << timings[1] << std::endl;
                }
        }
        return 0;
    }
    TestGpuRenderer test;
    return QTest::qExec(&test, argc, argv);
}
#include "TestGpuRenderer.moc"
