// CPU against Vulkan for the features moved to the GPU after Lens Effects, on a large picture:
//   feature_benchmark [width height]   (default 6000 × 4000)
// One Markdown row per case: the median of three end-to-end runs on each backend and the largest channel difference.
#include "core/CameraRaw.h"
#include "core/Document.h"
#include "rendering/Dither.h"
#include "rendering/RasterOperations.h"
#include "rendering/Scanlines.h"
#include "rendering/GpuRenderer.h"
#include "rendering/LayerRenderer.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>
using namespace compositor;

namespace {
QImage photo(int w, int h, int seed)
{
    QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < h; ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = uchar((x * 255 / w + seed * 40) & 255);
            row[x * 4 + 1] = uchar((y * 255 / h + ((x / 37 + y / 41) & 1) * 30) & 255);
            row[x * 4 + 2] = uchar(((x + y + seed * 13) / 23) & 255);
            row[x * 4 + 3] = 255;
        }
    }
    return image;
}
Layer layer(const QImage &image, BlendMode mode = BlendMode::Normal)
{
    Layer l;
    l.id = QUuid::createUuid();
    l.image = image;
    l.transform.size = image.size();
    l.blendMode = mode;
    return l;
}
double timed(const std::function<QImage()> &run, QImage *out, bool *accelerated)
{
    std::vector<double> times;
    for (int i = 0; i < 3; ++i) {
        QElapsedTimer t;
        t.start();
        *out = run();
        times.push_back(t.nsecsElapsed() / 1e6);
    }
    if (accelerated) *accelerated = GpuRenderer::lastExecution().accelerated;
    std::sort(times.begin(), times.end());
    return times[1];
}
int difference(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull()) return 255;
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888), y = b.convertToFormat(QImage::Format_RGBA8888);
    int most = 0;
    for (int row = 0; row < x.height(); ++row) {
        const uchar *p = x.constScanLine(row), *q = y.constScanLine(row);
        for (int i = 0; i < x.width() * 4; ++i) most = std::max(most, std::abs(int(p[i]) - int(q[i])));
    }
    return most;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const int w = argc > 2 ? std::atoi(argv[1]) : 6000, h = argc > 2 ? std::atoi(argv[2]) : 4000;
    std::printf("%s\n\n| Case (%d × %d) | CPU ms | Vulkan ms | Speed-up | Max difference |\n|---|---:|---:|---:|---:|\n",
                qPrintable(GpuRenderer::diagnostics().section('\n', 0, 0)), w, h);
    struct Case { QString name; std::function<QImage()> run; };
    std::vector<Case> cases;

    Document document;
    document.id = QUuid::createUuid();
    document.canvasSize = QSize(w, h);
    document.layers = {layer(photo(w, h, 0)), layer(photo(w, h, 1), BlendMode::Multiply)};
    Document single = document;
    single.layers.resize(1);
    cases.push_back({QStringLiteral("Canvas: one full-size layer"), [&] { return LayerRenderer::flattened(single, RenderPurpose::Display); }});
    cases.push_back({QStringLiteral("Canvas: two full-size layers"), [&] { return LayerRenderer::flattened(document, RenderPurpose::Display); }});
    Document adjusted = document;
    Layer levels;
    levels.id = QUuid::createUuid();
    levels.adjustment = QJsonObject{{"kind", "Levels"}, {"inputBlack", 20}, {"inputWhite", 230}, {"gamma", 1.2}};
    adjusted.layers.push_back(levels);
    cases.push_back({QStringLiteral("Canvas: two layers and Levels"), [&] { return LayerRenderer::flattened(adjusted, RenderPurpose::Display); }});

    // --survey: CPU times of the other filters, to see which are worth moving.
    if (app.arguments().contains(QStringLiteral("--survey"))) {
        qputenv("COMPOSITOR_RENDER_BACKEND", "cpu");
        const QImage image = photo(w, h, 2);
        QImage spot(w, h, QImage::Format_Grayscale8);
        spot.fill(0);
        for (int y = h / 2 - 60; y < h / 2 + 60; ++y) std::fill_n(spot.scanLine(y) + w / 2 - 60, 120, uchar(255));
        CameraRawSettings basic; basic.exposure = .5; basic.contrast = 20; basic.temperature = 10; basic.vibrance = 20;
        CameraRawSettings effects = basic; effects.clarity = 30; effects.texture = 20; effects.dehaze = 15; effects.vignetteAmount = -30; effects.grainAmount = 20;
        CameraRawSettings detail = basic; detail.detail.sharpenAmount = 60; detail.detail.noiseLuminance = 30;
        CameraRawSettings shadows = basic; shadows.shadows = 40; shadows.highlights = -30;
        DitherSettings dither;
        ScanlinesSettings scanlines;
        const std::vector<std::pair<const char *, std::function<QImage()>>> survey{
            {"Add Noise", [&] { return RasterOperations::addNoise(image, 20, true, false, 7); }},
            {"Grain", [&] { return RasterOperations::grain(image, 30, 4, 50, 7, {}, 1); }},
            {"Lens Correction", [&] { return RasterOperations::lensDistorted(image, 30); }},
            {"Vignette", [&] { return RasterOperations::vignette(image, 50, Qt::black, 50, 0, 50, 0, std::nullopt); }},
            {"Tonal Contrast", [&] { return RasterOperations::tonalContrast(image, 50, 20, 30, 20, 10); }},
            {"Camera RAW basic", [&] { return RasterOperations::cameraRaw(image, basic); }},
            {"Camera RAW shadows", [&] { return RasterOperations::cameraRaw(image, shadows); }},
            {"Camera RAW effects", [&] { return RasterOperations::cameraRaw(image, effects); }},
            {"Camera RAW detail", [&] { return RasterOperations::cameraRaw(image, detail); }},
            {"Dither", [&] { return dither.apply(image); }},
            {"Scanlines", [&] { return scanlines.apply(image); }},
            {"Color Range", [&] { return RasterOperations::colorRangeMask(image, {200, 60, 40}, {}, 40, false); }},
            {"Magic Wand", [&] { return RasterOperations::magicWandMask(image, {w / 2, h / 2}, 32, 0, true).value_or(QImage()); }},
            {"Feather mask", [&] { return RasterOperations::featherMask(spot, 30); }},
            {"Content-Aware Fill (120 px)", [&] { return RasterOperations::contentAwareFill(image, spot).value_or(QImage()); }},
            {"Spot heal (120 px)", [&] { return RasterOperations::spotHeal(image, spot, 1, 0, 3).value_or(QImage()); }},
        };
        std::printf("| Filter (CPU, %d × %d) | ms |\n|---|---:|\n", w, h);
        for (const auto &[name, run] : survey) {
            QImage out;
            std::printf("| %s | %.0f |\n", name, timed(run, &out, nullptr));
            std::fflush(stdout);
        }
        return 0;
    }
    for (const auto &c : cases) {
        QImage cpu, gpu;
        bool accelerated = false;
        qputenv("COMPOSITOR_RENDER_BACKEND", "cpu");
        const double cpuMs = timed(c.run, &cpu, nullptr);
        qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
        const double gpuMs = timed(c.run, &gpu, &accelerated);
        std::printf("| %s | %.0f | %.0f | %.1f× | %d |%s\n", qPrintable(c.name), cpuMs, gpuMs, cpuMs / gpuMs, difference(cpu, gpu),
                    accelerated ? "" : qPrintable(QStringLiteral(" CPU fallback: ") + GpuRenderer::lastExecution().reason));
        if (qEnvironmentVariableIsSet("FEATURE_BENCHMARK_GRAPHS"))
            for (const auto &run : GpuRenderer::recentExecutions())
                std::printf("    %s %s, %.0f ms, %.0f MiB working, %s\n", run.accelerated ? "GPU" : "CPU", qPrintable(run.operations),
                            run.milliseconds, run.workingBytes / 1048576.0, qPrintable(run.reason));
        std::fflush(stdout);
    }
    return 0;
}
