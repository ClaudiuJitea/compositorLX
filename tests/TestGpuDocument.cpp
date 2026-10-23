// Document-level GPU tests: parity of the whole-document graph with the CPU reference across
// placements, blend modes, HDR and many-layer stacks; Automatic-mode decisions; committed
// output determinism; tiling; the canvas prefix cache; scheduling and memory lifecycle.
#include "rendering/DeepImage.h"
#include "rendering/GpuDocument.h"
#include "rendering/GpuOperations.h"
#include "rendering/GpuRenderer.h"
#include "rendering/LayerEffectsRenderer.h"
#include "rendering/LayerRenderer.h"
#include "rendering/ProgressiveBlur.h"
#include "rendering/RasterOperations.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QTest>
#include <QThreadPool>
#include <future>
using namespace compositor;

namespace {
QImage fixture(QSize size, QImage::Format format, int seed = 0) {
    QImage image(size, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        auto *p = reinterpret_cast<float *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            float a = ((x * 13 + y * 7 + seed * 5) % 23) / 22.f;
            for (int c = 0; c < 3; ++c)
                p[x * 4 + c] = a * ((x * (c + 5) + y * 11 + c * 3 + seed) % 31) / 30.f;
            p[x * 4 + 3] = a;
        }
    }
    return image.convertToFormat(format);
}
QImage opaque(QSize size, int seed = 0) {
    QImage image = fixture(size, QImage::Format_RGBA8888_Premultiplied, seed);
    for (int y = 0; y < image.height(); ++y) {
        uchar *p = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x)
            p[x * 4 + 3] = 255, p[x * 4] = uchar((x * 7 + seed) % 256), p[x * 4 + 1] = uchar((y * 5) % 256),
                      p[x * 4 + 2] = uchar((x + y) % 256);
    }
    return image;
}
// Largest channel difference; infinity for any non-finite value or a size mismatch.
double error(const QImage &a, const QImage &b) {
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
int nonFinite(const QImage &image) {
    const QImage f = DeepImage::floating(image);
    int count = 0;
    for (int y = 0; y < f.height(); ++y) {
        const auto *p = reinterpret_cast<const float *>(f.constScanLine(y));
        for (int i = 0; i < f.width() * 4; ++i)
            count += !std::isfinite(p[i]);
    }
    return count;
}
Layer raster(const QImage &image, QPointF origin = {}) {
    Layer layer;
    layer.id = QUuid::createUuid();
    layer.image = image;
    layer.transform.origin = origin;
    layer.transform.size = image.size();
    return layer;
}
Layer filter(const QJsonObject &settings) {
    Layer layer;
    layer.id = QUuid::createUuid();
    layer.adjustment = settings;
    return layer;
}
QImage cpuRender(const Document &document) {
    GpuRenderer::CpuScope cpu;
    return LayerRenderer::flattened(document);
}
void resetReport() { GpuGraph{}.execute(-1, QImage::Format_RGBA8888_Premultiplied); }
} // namespace

class TestGpuDocument : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
        const auto probe = GpuOperations::simple(fixture({9, 9}, QImage::Format_RGBA8888_Premultiplied), GpuOp::Invert);
        if (probe.isNull()) {
            QVERIFY2(!qEnvironmentVariableIsSet("COMPOSITOR_REQUIRE_VULKAN_TESTS"),
                     qPrintable(GpuRenderer::lastExecution().reason));
            QSKIP(qPrintable(GpuRenderer::lastExecution().reason));
        }
    }
    void init() {
        qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
        qunsetenv("COMPOSITOR_GPU_MEMORY_MIB");
    }

    // C1/A2: every sampling, rotation, scale and fractional origin, in every blend mode.
    void placementMatrix_data() {
        QTest::addColumn<int>("sampling");
        QTest::addColumn<double>("rotation");
        QTest::addColumn<double>("scale");
        QTest::addColumn<bool>("fractional");
        for (int sampling = 0; sampling < 3; ++sampling)
            for (double rotation : {0., 11.})
                for (double scale : {1., 1.3})
                    for (bool fractional : {false, true})
                        QTest::newRow(qPrintable(QString("s%1-r%2-x%3-%4").arg(sampling).arg(rotation).arg(scale).arg(fractional)))
                            << sampling << rotation << scale << fractional;
    }
    void placementMatrix() {
        QFETCH(int, sampling);
        QFETCH(double, rotation);
        QFETCH(double, scale);
        QFETCH(bool, fractional);
        for (int mode = 0; mode < 24; ++mode) {
            Document document;
            document.canvasSize = {300, 200};
            Layer base = raster(fixture({300, 200}, QImage::Format_RGBA8888_Premultiplied));
            Layer top = raster(fixture({120, 90}, QImage::Format_RGBA8888_Premultiplied, 3),
                               fractional ? QPointF(10.5, 3.25) : QPointF(10, 3));
            top.transform.size = QSizeF(120 * scale, 90 * scale);
            top.transform.rotation = rotation;
            top.transform.sampling = Sampling(sampling);
            top.blendMode = BlendMode(mode);
            document.layers = {base, top};
            const QImage cpu = cpuRender(document);
            const QImage gpu = LayerRenderer::flattened(document);
            QVERIFY2(GpuRenderer::lastExecution().accelerated, qPrintable(GpuRenderer::lastExecution().reason));
            const double delta = error(cpu, gpu);
            QVERIFY2(delta <= 2.01 / 255, qPrintable(QString("mode %1: %2 levels").arg(mode).arg(delta * 255)));
        }
    }
    // C2: many-layer stacks stay within the document tolerance on the canvas path.
    void manyLayerStack() {
        Document document;
        document.canvasSize = {300, 200};
        document.layers.push_back(raster(opaque({300, 200})));
        for (int i = 0; i < 20; ++i) {
            Layer layer = raster(fixture({150, 100}, QImage::Format_RGBA8888_Premultiplied, i), QPointF(11 * i % 150, 7 * i % 100));
            layer.blendMode = BlendMode((5 * i) % 24);
            document.layers.push_back(layer);
        }
        const double delta = error(cpuRender(document), LayerRenderer::flattened(document));
        qInfo() << "20-layer stack, largest difference (levels):" << delta * 255;
        QVERIFY2(delta <= 4.01 / 255, qPrintable(QString("%1 levels").arg(delta * 255)));
    }
    // A3/OD1: exports and other committed renders are byte-identical in CPU and Automatic modes.
    void committedOutputIsDeterministic() {
        qputenv("COMPOSITOR_RENDER_BACKEND", "auto");
        Document document;
        document.canvasSize = {640, 520};
        document.layers.push_back(raster(opaque({640, 520})));
        for (int i = 0; i < 6; ++i) {
            Layer layer = raster(fixture({300, 260}, QImage::Format_RGBA8888_Premultiplied, i), QPointF(40 * i, 30 * i));
            layer.blendMode = BlendMode((7 * i + 3) % 24);
            layer.opacity = .8;
            document.layers.push_back(layer);
        }
        resetReport();
        const QImage automatic = LayerRenderer::flattened(document);
        QVERIFY(!GpuRenderer::lastExecution().accelerated);
        qputenv("COMPOSITOR_RENDER_BACKEND", "cpu");
        QCOMPARE(automatic, LayerRenderer::flattened(document));
        qputenv("COMPOSITOR_RENDER_BACKEND", "auto");
        const QImage display = LayerRenderer::flattened(document, RenderPurpose::Display);
        QVERIFY2(GpuRenderer::lastExecution().accelerated, qPrintable(GpuRenderer::lastExecution().reason));
        QVERIFY2(error(display, automatic) <= 3.01 / 255, qPrintable(QString("%1 levels").arg(error(display, automatic) * 255)));
    }
    // C4/A5/OD3: HDR component blends are finite on both backends and agree.
    void hdrBlendModes() {
        Document document;
        document.canvasSize = {120, 90};
        QImage low(120, 90, QImage::Format_RGBA32FPx4_Premultiplied), high = low;
        for (int y = 0; y < 90; ++y) {
            auto *a = reinterpret_cast<float *>(low.scanLine(y));
            auto *b = reinterpret_cast<float *>(high.scanLine(y));
            for (int x = 0; x < 120; ++x) {
                const float alpha = x % 17 == 0 ? 0.f : ((x + y) % 5 + 1) / 5.f;
                const bool gray = (x + y) % 3 == 0;
                for (int c = 0; c < 3; ++c) {
                    const float v = gray ? 2.5f : (1 + ((x * (c + 2) + y) % 9) / 3.f); // Up to 4.
                    a[x * 4 + c] = v * alpha;
                    b[x * 4 + c] = (gray ? 1.7f : v * .6f) * alpha;
                }
                a[x * 4 + 3] = b[x * 4 + 3] = alpha;
            }
        }
        for (int mode = 0; mode < 24; ++mode) {
            Layer base = raster(low), top = raster(high);
            top.blendMode = BlendMode(mode);
            document.layers = {base, top};
            const QImage cpu = cpuRender(document), gpu = LayerRenderer::flattened(document);
            QVERIFY2(GpuRenderer::lastExecution().accelerated, qPrintable(GpuRenderer::lastExecution().reason));
            QCOMPARE(nonFinite(cpu), 0);
            QCOMPARE(nonFinite(gpu), 0);
            const double delta = error(cpu, gpu);
            QVERIFY2(delta <= .0005, qPrintable(QString("mode %1: %2").arg(mode).arg(delta)));
        }
    }
    // The audit probes (C5), kept as regressions.
    void auditProbes() {
        const auto compare = [](const Document &document, double tolerance) {
            const QImage cpu = cpuRender(document), gpu = LayerRenderer::flattened(document);
            QVERIFY2(GpuRenderer::lastExecution().accelerated, qPrintable(GpuRenderer::lastExecution().reason));
            QVERIFY2(error(cpu, gpu) <= tolerance, qPrintable(QString("%1 levels").arg(error(cpu, gpu) * 255)));
        };
        Document document;
        document.canvasSize = {300, 200};
        const Layer base = raster(fixture({300, 200}, QImage::Format_RGBA8888_Premultiplied));
        {
            Layer flipped = raster(fixture({180, 120}, QImage::Format_RGBA8888_Premultiplied, 2), {20, 30});
            flipped.transform.flipX = flipped.transform.flipY = true;
            flipped.blendMode = BlendMode::Multiply;
            document.layers = {base, flipped};
            compare(document, 2.01 / 255);
        }
        {
            Layer outer, inner;
            outer.id = QUuid::createUuid();
            outer.group = true;
            outer.opacity = .8;
            outer.mask = QImage(300, 200, QImage::Format_Grayscale8);
            outer.mask.fill(200);
            inner.id = QUuid::createUuid();
            inner.group = true;
            inner.parentId = outer.id;
            inner.opacity = .5;
            inner.mask = QImage(300, 200, QImage::Format_Grayscale8);
            for (int y = 0; y < 200; ++y)
                for (int x = 0; x < 300; ++x)
                    inner.mask.scanLine(y)[x] = uchar(x * 255 / 299);
            outer.transform.size = inner.transform.size = QSizeF(300, 200);
            Layer child = raster(fixture({200, 150}, QImage::Format_RGBA8888_Premultiplied, 4), {40, 20});
            child.parentId = inner.id;
            child.mask = QImage(200, 150, QImage::Format_Grayscale8);
            child.mask.fill(170);
            child.blendMode = BlendMode::Screen;
            document.layers = {base, outer, inner, child};
            compare(document, 2.01 / 255);
        }
        {
            Layer effects = raster(fixture({120, 90}, QImage::Format_RGBA8888_Premultiplied, 5), {60, 40});
            LayerEffects fx;
            StrokeEffect stroke;
            stroke.size = 6;
            stroke.red = 1;
            fx.stroke = stroke;
            ShadowEffect shadow;
            shadow.blur = 9;
            shadow.distance = 7;
            fx.shadow = shadow;
            effects.effects = fx;
            effects.blendMode = BlendMode::Overlay;
            document.layers = {base, effects};
            compare(document, 2.01 / 255);
        }
        {
            QJsonArray ranges;
            ranges.append(QJsonObject{{"black", 20}, {"gamma", 1.4}, {"white", 230}, {"outputBlack", 5}, {"outputWhite", 250}});
            Layer levels = filter({{"kind", "Levels"}, {"levels", QJsonObject{{"ranges", ranges}}}});
            levels.opacity = .6;
            document.layers = {base, levels};
            compare(document, 1.01 / 255);
        }
    }
    // Automatic sends each listed path to the GPU at ≥ 512² and keeps cheap ones on the CPU.
    void automaticDecisions() {
        qputenv("COMPOSITOR_RENDER_BACKEND", "auto");
        const QImage image = fixture({640, 512}, QImage::Format_RGBA8888_Premultiplied);
        const auto accelerated = [&](const std::function<void()> &run) {
            resetReport();
            run();
            return GpuRenderer::lastExecution().accelerated;
        };
        QVERIFY(accelerated([&] { RasterOperations::exposure(image, .5, 0, 1); }));
        QVERIFY(accelerated([&] { RasterOperations::hueSaturation(image, HueSaturationSettings(20, 10, 0)); }));
        QVERIFY(accelerated([&] { RasterOperations::motionBlur(image, 20, 15); }));
        QVERIFY(accelerated([&] { RasterOperations::gaussianBlur(image, 4); }));
        QVERIFY(accelerated([&] { RasterOperations::bloomGlow(image, 40, 6); }));
        QVERIFY(accelerated([&] { DeepImage::gaussianBlur(DeepImage::floating(image), 3); }));
        QVERIFY(accelerated([&] {
            ProgressiveBlurSettings s;
            s.to = 6;
            s.end = {0, 500};
            s.apply(image);
        }));
        QVERIFY(accelerated([&] {
            QImage mask(640, 512, QImage::Format_Grayscale8);
            mask.fill(0);
            mask.scanLine(100)[100] = 255;
            RasterOperations::featherMask(mask, 6);
        }));
        QVERIFY(!accelerated([&] { RasterOperations::inverted(image); }));
        QVERIFY(accelerated([&] {
            LevelsSettings s;
            s.ranges[0] = {10, 1.1, 240, 0, 255};
            RasterOperations::levels(image, s);
        }));
        QVERIFY(accelerated([&] { RasterOperations::gradientMap(image, Qt::black, Qt::white, false); }));
        QVERIFY(accelerated([&] {
            const float w[6] = {.4f, .6f, .4f, .6f, .2f, .8f};
            RasterOperations::blackWhite(image, w, false, 0, 0);
        }));
        Document document;
        document.canvasSize = image.size();
        document.layers = {raster(image), raster(fixture({300, 200}, image.format(), 1), {5, 9})};
        QVERIFY(accelerated([&] { (void)LayerRenderer::flattened(document, RenderPurpose::Display); }));
        QVERIFY(!accelerated([&] { (void)LayerRenderer::flattened(document, RenderPurpose::Commit); }));
    }
    // A1: a document that cannot fit (and cannot be tiled) returns before any CPU preparation.
    void overBudgetReturnsEarly() {
        qputenv("COMPOSITOR_GPU_MEMORY_MIB", "64");
        Document document;
        document.canvasSize = {6000, 4000};
        QImage big(6000, 4000, QImage::Format_RGBA8888_Premultiplied);
        big.fill(QColor(30, 60, 90));
        Layer effects = raster(big);
        LayerEffects fx;
        ShadowEffect shadow;
        shadow.blur = 40;
        fx.shadow = shadow;
        effects.effects = fx;
        effects.transform.rotation = 7;
        document.layers = {effects, filter({{"kind", "Gaussian Blur"}, {"blurRadius", 240}})};
        QVERIFY(estimateGpuDocumentBytes(document, document.canvasSize) > 64ull << 20);
        LayerEffectsRenderer::clearCache();
        QElapsedTimer timer;
        timer.start();
        QVERIFY(tryGpuDocument(document).isNull());
        QVERIFY2(timer.elapsed() < 150, qPrintable(QString("%1 ms").arg(timer.elapsed())));
    }
    // B7: a document over the budget renders in canvas tiles with overlap and matches the CPU.
    void tiledDocument() {
        qputenv("COMPOSITOR_GPU_MEMORY_MIB", "64");
        Document document;
        document.canvasSize = {2400, 1700};
        Layer base = raster(opaque({2400, 1700}));
        Layer rotated = raster(fixture({900, 700}, QImage::Format_RGBA8888_Premultiplied, 1), {300.5, 200.25});
        rotated.transform.rotation = 9;
        rotated.blendMode = BlendMode::Multiply;
        Layer group;
        group.id = QUuid::createUuid();
        group.group = true;
        group.transform.size = QSizeF(2400, 1700);
        group.mask = QImage(2400, 1700, QImage::Format_Grayscale8);
        for (int y = 0; y < 1700; ++y)
            for (int x = 0; x < 2400; ++x)
                group.mask.scanLine(y)[x] = uchar((x / 7 + y / 5) % 256);
        Layer child = raster(fixture({1300, 900}, QImage::Format_RGBA8888_Premultiplied, 2), {1000, 700});
        child.parentId = group.id;
        child.mask = QImage(1300, 900, QImage::Format_Grayscale8);
        child.mask.fill(180);
        child.blendMode = BlendMode::SoftLight;
        QJsonArray ranges;
        ranges.append(QJsonObject{{"black", 10}, {"gamma", 1.2}, {"white", 240}});
        document.layers = {base, rotated, group, child, filter({{"kind", "Gaussian Blur"}, {"blurRadius", 5}}),
                           filter({{"kind", "Levels"}, {"levels", QJsonObject{{"ranges", ranges}}}}),
                           filter({{"kind", "RGB Split"}, {"filterLayerSettings", QJsonObject{{"rgbSplitDistance", 4}}}}),
                           filter({{"kind", "Progressive Blur"},
                                   {"filterLayerSettings", QJsonObject{{"progressiveTo", 3}}}})};
        QVERIFY(estimateGpuDocumentBytes(document, document.canvasSize) > (64ull << 20) * 8 / 10);
        const auto before = GpuRenderer::recentExecutions().size();
        Q_UNUSED(before)
        const QImage gpu = tryGpuDocument(document);
        QVERIFY2(!gpu.isNull(), qPrintable(GpuRenderer::lastExecution().reason));
        int graphs = 0;
        for (const auto &r : GpuRenderer::recentExecutions())
            graphs += r.accelerated && r.operations.contains("Composite");
        QVERIFY(graphs > 1);
        const double delta = error(cpuRender(document), gpu);
        QVERIFY2(delta <= 2.01 / 255, qPrintable(QString("%1 levels").arg(delta * 255)));
    }
    // B1: 8-bit sources upload at 4 bytes per pixel, so 3000×2000 with three layers is one graph within 512 MiB.
    void compactStorage() {
        Document document;
        document.canvasSize = {3000, 2000};
        for (int i = 0; i < 3; ++i) {
            Layer layer = raster(opaque({3000, 2000}, i));
            layer.blendMode = BlendMode(std::array{0, 2, 10}[size_t(i)]);
            document.layers.push_back(layer);
        }
        QVERIFY(estimateGpuDocumentBytes(document, document.canvasSize) < (512ull << 20) * 8 / 10);
        const QImage gpu = tryGpuDocument(document);
        const GpuExecution report = GpuRenderer::lastExecution();
        QVERIFY2(report.accelerated, qPrintable(report.reason));
        QCOMPARE(report.uploadedBytes, quint64(3) * 3000 * 2000 * 4); // No canvas upload: the graph starts from Clear.
        QCOMPARE(report.downloadedBytes, quint64(3000) * 2000 * 4);
        QVERIFY(report.workingBytes <= 512ull << 20);
        QVERIFY(error(cpuRender(document), gpu) <= 1.01 / 255);
    }
    // B8: the canvas keeps the composite below the edited layer; results equal a full render.
    void prefixCache() {
        qputenv("COMPOSITOR_RENDER_BACKEND", "cpu");
        Document document;
        document.id = QUuid::createUuid();
        document.canvasSize = {400, 300};
        document.layers.push_back(raster(opaque({400, 300})));
        for (int i = 0; i < 6; ++i) {
            Layer layer = raster(fixture({200, 150}, QImage::Format_RGBA8888_Premultiplied, i), QPointF(30 * i, 20 * i));
            layer.blendMode = BlendMode((3 * i + 1) % 24);
            document.layers.push_back(layer);
        }
        Layer clipped = raster(fixture({100, 80}, QImage::Format_RGBA8888_Premultiplied, 9), {70, 50});
        clipped.maskSourceId = document.layers.last().id;
        document.layers.push_back(clipped);
        for (int edit = 0; edit < 4; ++edit) {
            // Paint on layer 4, then the clipped layer (whose base must re-render with it).
            Layer &target = document.layers[edit < 2 ? 4 : document.layers.size() - 1];
            target.image.setPixelColor(10 + edit, 10, QColor(255, 0, edit * 60));
            const QImage display = LayerRenderer::flattened(document, RenderPurpose::Display);
            QCOMPARE(display, LayerRenderer::flattened(document));
        }
        qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
        document.layers[5].opacity = .5;
        const QImage display = LayerRenderer::flattened(document, RenderPurpose::Display);
        QVERIFY(error(display, cpuRender(document)) <= 2.01 / 255);
    }
    // B2/C2: GUI-thread work never waits behind a long graph on another thread.
    void guiThreadDoesNotWait() {
        const QImage large = fixture({2000, 2000}, QImage::Format_RGBA32FPx4_Premultiplied);
        auto background = std::async(std::launch::async, [&] {
            GpuRenderer::PriorityScope exportPriority(GpuPriority::Export);
            GpuGraph graph;
            int at = graph.source(large);
            for (int i = 0; i < 12; ++i)
                at = graph.gaussian(at, 120);
            return graph.execute(at, large.format());
        });
        QTest::qWait(30); // Let the export take the queue.
        QVERIFY2(background.wait_for(std::chrono::seconds(0)) == std::future_status::timeout,
                 "the background graph must still be running");
        const QImage small = fixture({64, 64}, QImage::Format_RGBA8888_Premultiplied);
        QElapsedTimer timer;
        timer.start();
        GpuGraph graph;
        graph.execute(graph.pass(GpuOp::Invert, graph.source(small)), small.format());
        const qint64 waited = timer.elapsed();
        QVERIFY2(waited < 50, qPrintable(QString("%1 ms").arg(waited)));
        QVERIFY(!background.get().isNull());
    }
    // B6: pooled memory is released on trim and the device can be closed and reopened.
    void memoryLifecycle() {
        const QImage image = fixture({257, 129}, QImage::Format_RGBA8888_Premultiplied);
        GpuGraph graph;
        const int at = graph.gaussian(graph.source(image), 3, true);
        GpuExecution first, second, third;
        QVERIFY(!graph.execute(at, image.format(), RenderBackend::Vulkan, &first).isNull());
        graph.execute(at, image.format(), RenderBackend::Vulkan, &second);
        QCOMPARE(second.uploadedBytes, quint64(0));
        GpuRenderer::trim();
        graph.execute(at, image.format(), RenderBackend::Vulkan, &third);
        QVERIFY(third.uploadedBytes > 0);
        GpuRenderer::release();
        GpuExecution reopened;
        QVERIFY(!graph.execute(at, image.format(), RenderBackend::Vulkan, &reopened).isNull());
        QVERIFY(reopened.accelerated);
        GpuRenderer::warmUp();
        QThreadPool::globalInstance()->waitForDone();
    }
    // A4: the 8-bit Gaussian sums and divides in integers; it matches the CPU exactly.
    void exactByteGaussian() {
        const QImage image = fixture({301, 257}, QImage::Format_RGBA8888_Premultiplied);
        for (double radius : {2.5, 12.5, 60.}) {
            QImage cpu;
            {
                GpuRenderer::CpuScope scope;
                cpu = RasterOperations::gaussianBlur(image, radius);
            }
            const QImage gpu = GpuOperations::gaussian(image, radius, true);
            QVERIFY(!gpu.isNull());
            QCOMPARE(gpu, cpu);
        }
    }
    // B5: large-radius morphology (van Herk) matches the CPU's deque implementation exactly.
    void morphology() {
        const int width = 211, height = 173;
        std::vector<float> coverage(size_t(width) * height);
        for (size_t i = 0; i < coverage.size(); ++i)
            coverage[i] = float((i * 37) % 101) / 100.f;
        for (int radius : {3, 7, 25, 120, 250})
            for (bool smallest : {false, true}) {
                std::vector<float> cpu;
                {
                    GpuRenderer::CpuScope scope;
                    cpu = LayerEffectsRenderer::extreme(coverage, width, height, radius, smallest);
                }
                const auto gpu = LayerEffectsRenderer::extreme(coverage, width, height, radius, smallest);
                QVERIFY(GpuRenderer::lastExecution().accelerated);
                QCOMPARE(gpu, cpu);
            }
    }
    // B9: the faster CPU paths (exposure lookup table, threaded Hue/Saturation) give exactly the per-pixel results.
    // Bands of at most 32 rows and 21845 pixels take the original serial, per-pixel code.
    void cpuReferenceSpeedupsAreExact() {
        GpuRenderer::CpuScope cpu;
        QImage image(256, 256, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                uchar *p = image.scanLine(y) + x * 4;
                p[3] = uchar(y);
                p[0] = uchar(std::min(x, y));
                p[1] = uchar(std::min(255 - x, y));
                p[2] = uchar(y / 2);
            }
        const auto bands = [&](const std::function<QImage(const QImage &)> &op) {
            QImage out(image.size(), image.format());
            for (int y = 0; y < image.height(); y += 16) {
                const QImage part = op(image.copy(0, y, 256, 16));
                for (int row = 0; row < 16; ++row)
                    std::memcpy(out.scanLine(y + row), part.constScanLine(row), 256 * 4);
            }
            return out;
        };
        const auto exposure = [](const QImage &i) { return RasterOperations::exposure(i, .8, .02, 1.3); };
        QCOMPARE(exposure(image), bands(exposure));
        const auto hue = [](const QImage &i) { return RasterOperations::hueSaturation(i, HueSaturationSettings(35, -20, 10)); };
        QCOMPARE(hue(image), bands(hue));
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    TestGpuDocument test;
    return QTest::qExec(&test, argc, argv);
}
#include "TestGpuDocument.moc"
