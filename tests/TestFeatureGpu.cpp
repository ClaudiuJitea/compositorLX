#include "core/CameraRaw.h"
#include "rendering/GpuOperations.h"
#include "rendering/GpuRenderer.h"
#include "rendering/RasterOperations.h"
#include "rendering/SubjectRemoval.h"
#include "NoisePixels.h"
#include <QCoreApplication>
#include <QTest>
#include <algorithm>
#include <cmath>
using namespace compositor;

// The features moved to the GPU after Lens Effects, against their CPU kernels: Camera Raw's color table (exact, as
// macOS CameraRawGPUTests), and Add Noise and Grain (macOS GPUNoise, the same hashes).
namespace {
bool vulkan()
{
    if (GpuRenderer::lastExecution().accelerated) return true;
    if (qEnvironmentVariableIsSet("COMPOSITOR_REQUIRE_VULKAN_TESTS")) qFatal("%s", qPrintable(GpuRenderer::lastExecution().reason));
    return false;
}
bool ran(const char *operation)
{
    for (const auto &run : GpuRenderer::recentExecutions())
        if (run.accelerated && run.operations.contains(QLatin1String(operation))) return true;
    return false;
}
std::vector<CameraRawSettings> profiles()
{
    CameraRawSettings warm; warm.temperature = 37; warm.tint = -19;
    CameraRawSettings light = warm;
    light.exposure = .7; light.contrast = 23; light.highlights = -45; light.whites = 12; light.blacks = -18;
    light.vibrance = 37; light.saturation = -12;
    CameraRawSettings extreme = light;
    extreme.temperature = -100; extreme.tint = 100; extreme.exposure = 5; extreme.contrast = 100;
    extreme.highlights = -100; extreme.whites = 100; extreme.blacks = -100;
    CameraRawSettings color = light;
    color.curve.rgb = CameraRawCurveSettings::strongContrast;
    color.curve.shadows = 30;
    color.mixer.hue[0] = 45; color.mixer.saturation[2] = -25; color.mixer.luminance[4] = 30;
    color.grading.shadows.hue = 220; color.grading.shadows.saturation = 35;
    color.grading.highlights.hue = 40; color.grading.highlights.saturation = 20;
    CameraRawSettings shadows = light; shadows.shadows = 45;
    return {warm, light, extreme, color, shadows};
}
} // namespace

class TestFeatureGpu : public QObject {
    Q_OBJECT
private slots:
    void init() { qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan"); }

    // Every opaque 8-bit color, byte for byte (macOS composedLookupMatchesEveryOpaqueEightBitColorExactly).
    void cameraRawEveryOpaqueColor()
    {
        QImage colors(4096, 4096, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < 4096; ++y) {
            auto *row = reinterpret_cast<quint32 *>(colors.scanLine(y));
            for (int x = 0; x < 4096; ++x) {
                const quint32 index = quint32(y) * 4096 + quint32(x);
                uchar *p = reinterpret_cast<uchar *>(row + x);
                p[0] = uchar(index >> 16); p[1] = uchar(index >> 8); p[2] = uchar(index); p[3] = 255;
            }
        }
        for (const CameraRawSettings &settings : profiles()) {
            GpuGraph{}.execute(-1, QImage::Format_RGBA8888_Premultiplied); // forget the last report
            const QImage gpu = RasterOperations::cameraRaw(colors, settings);
            if (!ran("CameraRawLookup")) { if (!vulkan()) QSKIP("Vulkan unavailable"); QFAIL("the lookup did not run on the GPU"); }
            qputenv("COMPOSITOR_RENDER_BACKEND", "cpu");
            const QImage cpu = RasterOperations::cameraRaw(colors, settings);
            qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
            QVERIFY2(gpu == cpu, "all 16,777,216 opaque colors must match byte for byte");
        }
    }
    // Every alpha with a spread of colors, and padded rows (macOS composedLookupPreservesAllAlphaValues…).
    void cameraRawEveryAlpha()
    {
        QImage image(1031, 1024, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < image.height(); ++y) {
            uchar *row = image.scanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                const int alpha = (x + y * 7) % 256;
                for (int c = 0; c < 3; ++c) row[x * 4 + c] = uchar(alpha ? ((x * (c + 3) + y * 5 + c * 17) % (alpha + 1)) : 0);
                row[x * 4 + 3] = uchar(alpha);
            }
        }
        for (const CameraRawSettings &settings : profiles()) {
            const QImage gpu = RasterOperations::cameraRaw(image, settings);
            qputenv("COMPOSITOR_RENDER_BACKEND", "cpu");
            const QImage cpu = RasterOperations::cameraRaw(image, settings);
            qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
            QVERIFY2(gpu == cpu, "every alpha must match byte for byte");
        }
    }
    // A table with values out of range is left to the CPU, unchanged (camera_raw_lookup_is_valid).
    void cameraRawRejectsInvalidTables()
    {
        QImage image(8, 8, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::red);
        std::vector<float> table(size_t(33) * 33 * 33 * 3, .5f);
        table[7] = 1.5f;
        QImage copy = image;
        QVERIFY(!GpuOperations::cameraRawLookup(copy, table.data(), 33, RenderBackend::Vulkan));
        QCOMPARE(copy, image);
    }

    // Remove Background's preview refines a copy at most 1400 long (macOS GuidedMatte.refine's limit), then scales
    // it back: the layer's size, and close to the full-size refinement the commit makes.
    void removeBackgroundPreviewLimit()
    {
        const QSize size(3000, 2000);
        QImage guide(size, QImage::Format_RGBA8888_Premultiplied), mask(size, QImage::Format_Grayscale8);
        for (int y = 0; y < size.height(); ++y) {
            uchar *g = guide.scanLine(y), *m = mask.scanLine(y);
            for (int x = 0; x < size.width(); ++x) {
                const double d = std::hypot(x - 1500.0, y - 1000.0);
                const uchar v = d < 700 ? 230 : 40;
                g[x * 4] = g[x * 4 + 1] = g[x * 4 + 2] = v; g[x * 4 + 3] = 255;
                m[x] = uchar(std::clamp(int((760 - d) * 2), 0, 255)); // a soft, slightly wrong edge
            }
        }
        const SubjectRemovalSettings settings{true, 20, 0, 0};
        const QImage full = SubjectRemoval::refined(mask, guide, settings);
        const QImage preview = SubjectRemoval::refined(mask, guide, settings, 1400);
        QCOMPARE(preview.size(), size);
        double total = 0;
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x) total += std::abs(int(full.constScanLine(y)[x]) - int(preview.constScanLine(y)[x]));
        QVERIFY2(total / (size.width() * size.height()) < 1.0, qPrintable(QString::number(total / (size.width() * size.height()))));
        // Within the limit already, the preview is the commit's result exactly.
        const QImage small = mask.scaled(1200, 800), smallGuide = guide.scaled(1200, 800);
        QCOMPARE(SubjectRemoval::refined(small, smallGuide, settings, 1400), SubjectRemoval::refined(small, smallGuide, settings));
    }

    // Add Noise and Grain as GPU passes against the shared CPU kernels, within a level (float log/cos, half-up).
    void noiseAndGrain_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<bool>("gaussian");
        QTest::addColumn<bool>("monochromatic");
        for (bool gaussian : {false, true})
            for (bool monochromatic : {false, true})
                QTest::newRow(qPrintable(QStringLiteral("noise-%1-%2").arg(gaussian).arg(monochromatic))) << 0 << gaussian << monochromatic;
        QTest::newRow("grain") << 1 << false << false;
    }
    void noiseAndGrain()
    {
        QFETCH(int, kind);
        QFETCH(bool, gaussian);
        QFETCH(bool, monochromatic);
        QImage image(613, 409, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < image.height(); ++y) {
            uchar *row = image.scanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                const int alpha = x < 40 ? (x * 6) % 256 : 255;
                for (int c = 0; c < 3; ++c) row[x * 4 + c] = uchar(((x * (c + 2) + y * 3) % 256) * alpha / 255);
                row[x * 4 + 3] = uchar(alpha);
            }
        }
        const QPoint origin(1200, -37);
        GpuGraph graph;
        const int input = graph.source(image);
        const int output = kind == 0 ? GpuOperations::appendNoise(graph, input, 35, gaussian, monochromatic, 3000000007u, origin, 1)
                                     : GpuOperations::appendGrain(graph, input, 70, 3.5, 60, 3000000007u, origin, 1, 1);
        GpuExecution report;
        const QImage gpu = graph.execute(output, image.format(), RenderBackend::Vulkan, &report);
        if (!report.accelerated) { QVERIFY2(!qEnvironmentVariableIsSet("COMPOSITOR_REQUIRE_VULKAN_TESTS"), qPrintable(report.reason)); QSKIP("Vulkan unavailable"); }
        // The CPU kernels at the same document positions.
        QImage cpu = image;
        if (kind == 0)
            noise_add_at(cpu.bits(), size_t(cpu.width()), size_t(cpu.height()), size_t(cpu.bytesPerLine()), 35,
                         gaussian ? 1 : 0, monochromatic ? 1 : 0, 3000000007u, origin.x(), origin.y());
        else
            cpu = RasterOperations::grain(image, 70, 3.5, 60, 3000000007u, origin, 1);
        int most = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int i = 0; i < image.width() * 4; ++i)
                most = std::max(most, std::abs(int(gpu.constScanLine(y)[i]) - int(cpu.constScanLine(y)[i])));
        QVERIFY2(most <= 1, qPrintable(QStringLiteral("%1 levels").arg(most)));
    }
};

QTEST_GUILESS_MAIN(TestFeatureGpu)
#include "TestFeatureGpu.moc"
