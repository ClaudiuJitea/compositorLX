// Filter › Lens Effects timings, CPU against Vulkan, on a night scene with a few bright lights:
//   lens_benchmark [width height]   (default 6000 × 4000)
// Prints one Markdown row per effect with the median of three runs and the largest channel difference.
#include "rendering/GpuRenderer.h"
#include "rendering/LensEffects.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>
using namespace compositor;

namespace {
QImage night(int w, int h) {
    QImage image(w, h, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < h; ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = uchar(20 + (x * 40) / w);
            row[x * 4 + 1] = uchar(24 + (y * 30) / h);
            row[x * 4 + 2] = uchar(40);
            row[x * 4 + 3] = 255;
        }
    }
    // Street lights: small white and colored discs.
    const int side = std::max(w, h), lights = 9;
    for (int i = 0; i < lights; ++i) {
        const int cx = w * (i + 1) / (lights + 1), cy = h / 3 + (i % 3) * h / 6, r = std::max(2, side / (300 + i * 40));
        const int tint = i % 3;
        for (int y = std::max(0, cy - r); y < std::min(h, cy + r); ++y)
            for (int x = std::max(0, cx - r); x < std::min(w, cx + r); ++x)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) {
                    uchar *p = image.scanLine(y) + x * 4;
                    p[0] = tint == 1 ? 200 : 255; p[1] = 255; p[2] = tint == 2 ? 190 : 255;
                }
    }
    return image;
}
double timed(const std::function<QImage()> &run, QImage *out) {
    std::vector<double> times;
    for (int i = 0; i < 3; ++i) {
        QElapsedTimer t; t.start();
        *out = run();
        times.push_back(t.nsecsElapsed() / 1e6);
    }
    std::sort(times.begin(), times.end());
    return times[1];
}
int difference(const QImage &a, const QImage &b) {
    if (a.size() != b.size()) return 255;
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888), y = b.convertToFormat(QImage::Format_RGBA8888);
    int most = 0;
    for (int row = 0; row < x.height(); ++row) {
        const uchar *p = x.constScanLine(row), *q = y.constScanLine(row);
        for (int i = 0; i < x.width() * 4; ++i) most = std::max(most, std::abs(int(p[i]) - int(q[i])));
    }
    return most;
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const int w = argc > 2 ? std::atoi(argv[1]) : 6000, h = argc > 2 ? std::atoi(argv[2]) : 4000;
    const QImage image = night(w, h);
    std::printf("%s\n\n", qPrintable(GpuRenderer::diagnostics().section('\n', 0, 0)));
    std::printf("Lens Effects at %d × %d\n\n| Effect | CPU ms | Vulkan ms | Speed-up | Max difference |\n|---|---:|---:|---:|---:|\n", w, h);
    struct Case { const char *name; std::function<void(LensEffectSettings &)> set; };
    const std::vector<Case> cases{
        {"Anamorphic Flare", [](auto &s) { s.effect = LensEffect::AnamorphicFlare; }},
        {"Anamorphic Flare, threshold 98", [](auto &s) { s.effect = LensEffect::AnamorphicFlare; s.threshold = 98; }},
        {"Starburst", [](auto &s) { s.effect = LensEffect::Starburst; }},
        {"Starburst, 14 points", [](auto &s) { s.effect = LensEffect::Starburst; s.starPoints = 14; s.starLength = 30; }},
        {"Mist", [](auto &s) { s.effect = LensEffect::Mist; }},
        {"Mist, size 100", [](auto &s) { s.effect = LensEffect::Mist; s.mistSize = 100; }},
        {"Prism", [](auto &s) { s.effect = LensEffect::Prism; }},
        {"Prism, blur 50", [](auto &s) { s.effect = LensEffect::Prism; s.prismBlur = 50; }},
        {"Bokeh", [](auto &s) { s.effect = LensEffect::Bokeh; }},
    };
    for (const auto &c : cases) {
        LensEffectSettings s; c.set(s);
        QImage cpu, gpu;
        qputenv("COMPOSITOR_RENDER_BACKEND", "cpu");
        const double cpuMs = timed([&] { return s.apply(image); }, &cpu);
        qputenv("COMPOSITOR_RENDER_BACKEND", "vulkan");
        bool accelerated = false;
        const double gpuMs = timed([&] { return s.apply(image, RenderBackend::Auto, &accelerated); }, &gpu);
        double graphMs = 0;
        QString reason;
        for (const auto &run : GpuRenderer::recentExecutions()) { graphMs += run.milliseconds; if (!run.reason.isEmpty()) reason = run.reason; }
        std::printf("| %s | %.0f | %.0f | %.1f× | %d |%s\n", c.name, cpuMs, gpuMs, cpuMs / gpuMs, difference(cpu, gpu),
                    accelerated ? "" : qPrintable(QStringLiteral(" CPU fallback: ") + reason));
        if (qEnvironmentVariableIsSet("LENS_BENCHMARK_GRAPHS")) std::printf("  (last 20 graphs %.0f ms on the GPU)\n", graphMs);
        std::fflush(stdout);
    }
    return 0;
}
