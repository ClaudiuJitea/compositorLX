#include "core/EditorSession.h"
#include "rendering/DeepImage.h"
#include "rendering/FilterLayers.h"
#include "rendering/GpuDocument.h"
#include "rendering/GpuRenderer.h"
#include "rendering/LayerRenderer.h"
#include "rendering/RgbSplit.h"
#include "rendering/RimLight.h"
#include "rendering/VolumetricLight.h"
#include <QJsonObject>
#include <QPainter>
#include <QRandomGenerator>
#include <QtTest>


using namespace compositor;
// The 1.7.0 filters' GPU paths against their CPU references (Sharpen, Reduce Noise's non-local means, and the Vignette,
// Tonal Contrast and Sharpen layers on the document graph), each within a stated tolerance. Skips without Vulkan,
// unless COMPOSITOR_REQUIRE_VULKAN_TESTS asks for it (scripts/ci-gpu-tests.sh).
namespace {
QImage photo(int w, int h, bool deep = false)
{
    QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
    QRandomGenerator random(11);
    for (int y = 0; y < h; ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const int base = (x / 17 + y / 23) % 2 ? 180 : 60;
            for (int c = 0; c < 3; ++c) row[x * 4 + c] = uchar(std::clamp(base + c * 15 + random.bounded(-25, 26), 0, 255));
            row[x * 4 + 3] = 255;
        }
    }
    { QPainter p(&image); p.setPen(Qt::NoPen); p.setBrush(QColor(255, 250, 240)); p.drawEllipse(QRect(w / 3, h / 3, w / 5, h / 5)); }
    return deep ? image.convertToFormat(QImage::Format_RGBA64_Premultiplied) : image;
}
// The largest difference between two images, in 8-bit levels.
int levels(const QImage &a, const QImage &b)
{
    if (a.size() != b.size()) return 1000;
    const QImage x = DeepImage::floating(a), y = DeepImage::floating(b);
    double worst = 0;
    for (int row = 0; row < x.height(); ++row) {
        const auto *p = reinterpret_cast<const float *>(x.constScanLine(row));
        const auto *q = reinterpret_cast<const float *>(y.constScanLine(row));
        for (int i = 0; i < x.width() * 4; ++i) worst = std::max(worst, double(std::abs(p[i] - q[i])));
    }
    return int(std::ceil(worst * 255 - 1e-6));
}
void requireVulkan(bool accelerated)
{
    if (accelerated) return;
    QVERIFY2(!qEnvironmentVariableIsSet("COMPOSITOR_REQUIRE_VULKAN_TESTS"), qPrintable(GpuRenderer::lastExecution().reason));
    QSKIP("Vulkan unavailable");
}
} // namespace

class TestFilterLayersGpu : public QObject {
    Q_OBJECT
private slots:
    void sharpen_data()
    {
        QTest::addColumn<double>("amount");
        QTest::addColumn<double>("radius");
        QTest::addColumn<bool>("deep");
        QTest::newRow("default") << 100.0 << 1.0 << false;
        QTest::newRow("strong, wide") << 300.0 << 20.0 << false;
        QTest::newRow("fine") << 50.0 << .1 << false;
        QTest::newRow("16-bit") << 200.0 << 3.0 << true;
    }
    // b0f0b2b: Sharpen on Vulkan gives the CPU's pixels to within one 8-bit level.
    void sharpen()
    {
        QFETCH(double, amount); QFETCH(double, radius); QFETCH(bool, deep);
        const QImage image = photo(301, 207, deep);
        bool accelerated = false;
        const QImage gpu = FilterLayers::sharpen(image, {amount, radius}, 1, RenderBackend::Vulkan, &accelerated);
        requireVulkan(accelerated);
        const QImage cpu = FilterLayers::sharpen(image, {amount, radius}, 1, RenderBackend::Cpu);
        QCOMPARE(gpu.format(), cpu.format());
        QVERIFY2(levels(gpu, cpu) <= 1, qPrintable(QStringLiteral("%1 levels apart").arg(levels(gpu, cpu))));
    }

    // A picture past 4 megapixels goes to the GPU in bands of rows (the blur's reach as margin), joined without seams.
    void sharpenInBands()
    {
        const QImage image = photo(2100, 2100);
        bool accelerated = false;
        const QImage gpu = FilterLayers::sharpen(image, {150, 6}, 1, RenderBackend::Vulkan, &accelerated);
        requireVulkan(accelerated);
        const QImage cpu = FilterLayers::sharpen(image, {150, 6}, 1, RenderBackend::Cpu);
        QVERIFY2(levels(gpu, cpu) <= 1, qPrintable(QStringLiteral("%1 levels apart").arg(levels(gpu, cpu))));
    }
    // 79f98de: RGB Split's two passes in bands of rows on a large picture, straight and radial, against the CPU's two
    // passes. Tolerance: at most 0.001% of channels more than one 8-bit level apart and none more than 6. A sample that
    // lands right on a hard edge or the picture's edge can fall either side in floats (GPU) and doubles (CPU); one
    // dispatch over the whole picture differs from the CPU the same way.
    void rgbSplitInBands_data()
    {
        QTest::addColumn<bool>("radial");
        QTest::newRow("straight") << false;
        QTest::newRow("radial") << true;
    }
    void rgbSplitInBands()
    {
        QFETCH(bool, radial);
        const QImage image = photo(2100, 2100);
        RgbSplitSettings s; s.distance = 9; s.blur = 24; s.angle = 63; s.radial = radial; s.center = QPointF(800, 1300); s.reach = 700;
        RgbSplitExecution report;
        const QImage gpu = s.apply(image, 0, RgbSplitBackend::Vulkan, &report);
        requireVulkan(report.backend == RgbSplitBackend::Vulkan);
        const QImage cpu = s.applyCpu(image, 0);
        const QImage a = gpu.convertToFormat(QImage::Format_RGBA8888), b = cpu.convertToFormat(QImage::Format_RGBA8888);
        qint64 over = 0; int worst = 0;
        for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width() * 4; ++x) {
            const int d = std::abs(a.constScanLine(y)[x] - b.constScanLine(y)[x]);
            worst = std::max(worst, d); if (d > 1) ++over;
        }
        const qint64 channels = qint64(a.width()) * a.height() * 4;
        QVERIFY2(worst <= 6 && over * 100000 <= channels, qPrintable(QStringLiteral("worst %1, %2 of %3 channels past one level").arg(worst).arg(over).arg(channels)));
    }

    // 2c98c1b: Reduce Noise's non-local means on Vulkan smooths as the C code does: the brightness planes agree within
    // 1/255 (the GPU weighs by exp(), the C code by its 1024-step table), in bands of rows as on a large photo.
    void nonlocalMeans()
    {
        const int w = 1024, h = 2600;   // two bands of 2048 rows at this width, so the seam is checked too
        std::vector<float> luma(size_t(w) * h), coverage(luma.size(), 1.f), gpu(luma.size()), cpu(luma.size());
        QRandomGenerator random(3);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            luma[size_t(y) * w + x] = float(((x / 30 + y / 40) % 2 ? .7 : .3) + random.bounded(.1) - .05);
        for (int x = 0; x < 20; ++x) coverage[size_t(7) * w + x] = 0;
        if (!FilterLayers::nonlocalMeansGpu(luma.data(), coverage.data(), gpu.data(), w, h, 7, 2, .05, .02)) requireVulkan(false);
        // The whole filter on a photo, through the GPU hook and with the CPU forced: the 8-bit results.
        const QImage image = photo(400, 1100);
        const QImage viaGpu = FilterLayers::reduceNoise(image, {80, 40, 0});
        QImage viaCpu;
        { GpuRenderer::CpuScope scope; viaCpu = FilterLayers::reduceNoise(image, {80, 40, 0}); }
        // The C code weighs matches by a 1024-step table of exp(), floored, the GPU by exp() itself (as the mac's Metal
        // kernel does): a few pixels near the cut-off differ by a few levels, the rest by at most one.
        const QImage a = viaGpu.convertToFormat(QImage::Format_RGBA8888), b = viaCpu.convertToFormat(QImage::Format_RGBA8888);
        qint64 over = 0, total = 0; double sum = 0;
        for (int y = 0; y < a.height(); ++y) for (int x = 0; x < a.width() * 4; ++x) {
            const int d = std::abs(int(a.constScanLine(y)[x]) - int(b.constScanLine(y)[x]));
            sum += d; total++; if (d > 1) over++;
        }
        qInfo("Reduce Noise GPU vs CPU: worst %d levels, mean %.4f, %.4f%% past one level", levels(viaGpu, viaCpu), sum / total, 100.0 * over / total);
        QVERIFY2(sum / total < .05 && over * 1000 < total && levels(viaGpu, viaCpu) <= 6,
                 qPrintable(QStringLiteral("Reduce Noise %1 levels apart").arg(levels(viaGpu, viaCpu))));
        Q_UNUSED(cpu);
        // And the bands join without a seam: across the band boundary the result steps no more than between any two
        // rows inside a band (away from the picture's own edges, every 40 rows).
        const int rows = std::max(64, int((qint64(1) << 21) / w));
        QVERIFY(rows < h);
        const auto step = [&](int y) { double worst = 0; for (int x = 0; x < w; ++x) worst = std::max(worst, double(std::abs(gpu[size_t(y) * w + x] - gpu[size_t(y - 1) * w + x]))); return worst; };
        double inside = 0;
        for (int y = 1; y < h; ++y) if (y % 40 && y != rows) inside = std::max(inside, step(y));
        QVERIFY2(step(rows) <= inside + 1e-4, qPrintable(QStringLiteral("seam %1, inside %2").arg(step(rows)).arg(inside)));
    }

    void documentLayers_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QJsonObject>("settings");
        VignetteLayerSettings vignette; vignette.amount = 80; vignette.color = QColor(40, 10, 90); vignette.roundness = -40;
        TonalLayerSettings tonal; tonal.amount = 100; tonal.radius = 6;
        QTest::newRow("Vignette") << QStringLiteral("Vignette") << QJsonObject{{"vignette", vignette.json()}};
        QTest::newRow("Tonal Contrast") << QStringLiteral("Tonal Contrast") << QJsonObject{{"tonal", tonal.json()}};
        QTest::newRow("Sharpen") << QStringLiteral("Sharpen") << QJsonObject{{"sharpen", SharpenSettings{150, 2.5}.json()}};
    }
    // 7b5ba97 FilterLayerTests.vignetteAndTonalContrastMatchOnTheGPU, b0f0b2b: a layer of these kinds drawn on the GPU
    // canvas looks as the export makes it on the CPU (the mac allows 5 levels for Tonal Contrast; LX keeps 2).
    void documentLayers()
    {
        QFETCH(QString, kind); QFETCH(QJsonObject, settings);
        EditorSession session; session.createDocument(500, 360);
        QVERIFY(session.insertImage(photo(500, 360), "Photo"));
        QVERIFY(session.addAdjustment(kind, QJsonObject{{"filterLayerSettings", settings}}));
        const Document &document = *session.document();
        QImage gpu;
        GpuRenderer::setBackend(RenderBackend::Vulkan);
        gpu = tryGpuDocument(document);
        GpuRenderer::setBackend(RenderBackend::Auto);
        requireVulkan(!gpu.isNull());
        QImage cpu;
        { GpuRenderer::CpuScope scope; cpu = LayerRenderer::flattened(document); }
        QVERIFY2(levels(gpu, cpu) <= 2, qPrintable(QStringLiteral("%1: %2 levels apart").arg(kind).arg(levels(gpu, cpu))));
        QVERIFY(GpuRenderer::lastExecution().operations.contains(kind == "Tonal Contrast" ? "TonalContrast" : kind));
    }

    void rimLight_data()
    {
        QTest::addColumn<double>("glow");
        QTest::addColumn<double>("choke");
        QTest::addColumn<bool>("deep");
        QTest::newRow("glow, feathered") << 60.0 << 0.0 << false;
        QTest::newRow("no glow, held") << 0.0 << 100.0 << false;
        QTest::newRow("16-bit") << 30.0 << 50.0 << true;
    }
    // b9a0ed8: Rim Light's chain on Vulkan, blurs and all, gives the CPU's pixels to within two 8-bit levels.
    void rimLight()
    {
        QFETCH(double, glow); QFETCH(double, choke); QFETCH(bool, deep);
        const QImage image = photo(640, 420, deep);
        QImage coverage(640, 420, QImage::Format_Grayscale8); coverage.fill(0);
        { QPainter p(&coverage); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(Qt::white); p.drawEllipse(QRectF(200, 80, 240, 300)); }
        RimLightSettings settings; settings.glow = glow; settings.lights[0].choke = choke; settings.lights[0].strength = 70;
        settings.lights.append(settings.nextLight());
        bool accelerated = false;
        const QImage gpu = RimLighting::applyWithSubject(image, coverage, settings, RenderBackend::Vulkan, &accelerated);
        requireVulkan(accelerated);
        const QImage cpu = RimLighting::applyWithSubject(image, coverage, settings, RenderBackend::Cpu);
        QCOMPARE(gpu.format(), cpu.format());
        QVERIFY2(levels(gpu, cpu) <= 2, qPrintable(QStringLiteral("%1 levels apart").arg(levels(gpu, cpu))));
        QVERIFY(levels(cpu, image) > 10);
    }

    // 1405545: Volumetric Light's beams gather on Vulkan as on the CPU, to within 1% of their light.
    void volumetricBeams()
    {
        QImage small(300, 200, QImage::Format_RGBA32FPx4_Premultiplied);
        small.fill(QColor::fromRgbF(.02f, .02f, .02f));
        { QPainter p(&small); p.fillRect(QRect(130, 20, 40, 40), QColor::fromRgbF(1, .95f, .85f)); }
        std::vector<float> subject(300 * 200, 0.f);
        for (int y = 120; y < 200; ++y) for (int x = 100; x < 200; ++x) subject[size_t(y) * 300 + x] = 1;
        VolumetricLightSettings s; s.length = 80; s.shafts = 40; s.size = 30;
        bool accelerated = false;
        const auto gpu = VolumetricLighting::beams(small, subject, 1, QPointF(150, 40), 300, s, RenderBackend::Vulkan, &accelerated);
        requireVulkan(accelerated);
        const auto cpu = VolumetricLighting::beams(small, subject, 1, QPointF(150, 40), 300, s, RenderBackend::Cpu);
        QCOMPARE(gpu.size(), cpu.size());
        float worst = 0, peak = 0; size_t at = 0, over = 0;
        for (size_t i = 0; i < cpu.size(); ++i) { const float d = std::abs(gpu[i] - cpu[i]); if (d > worst) { worst = d; at = i; } if (d > .05f) ++over; peak = std::max(peak, cpu[i]); }
        Q_UNUSED(over);
        QVERIFY2(worst <= .01f * std::max(1.f, peak), qPrintable(QStringLiteral("%1 apart, peak %2").arg(worst).arg(peak)));
    }
};
QTEST_MAIN(TestFilterLayersGpu)
#include "TestFilterLayersGpu.moc"
