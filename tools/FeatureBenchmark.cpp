// CPU against Vulkan for the features moved to the GPU after Lens Effects, on a large picture:
//   feature_benchmark [width height]   (default 6000 × 4000)
// One Markdown row per case: the median of three end-to-end runs on each backend and the largest channel difference.
#include "core/CameraRaw.h"
#include "core/Document.h"
#include "core/EditorSession.h"
#include "rendering/Dither.h"
#include "rendering/RasterOperations.h"
#include "rendering/SubjectRemoval.h"
#include "rendering/Scanlines.h"
#include "rendering/GpuRenderer.h"
#include "rendering/LayerEffectsRenderer.h"
#include "rendering/LayerRenderer.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <array>
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
    const QImage picture = photo(w, h, 2);
    CameraRawSettings develop; develop.exposure = .5; develop.contrast = 20; develop.temperature = 10; develop.vibrance = 20;
    cases.push_back({QStringLiteral("Camera RAW (Light and Color)"), [&] { return RasterOperations::cameraRaw(picture, develop); }});
    CameraRawSettings shadowsDevelop = develop; shadowsDevelop.shadows = 40;
    cases.push_back({QStringLiteral("Camera RAW (with Shadows)"), [&] { return RasterOperations::cameraRaw(picture, shadowsDevelop); }});
    Document noisy = document;
    Layer noise;
    noise.id = QUuid::createUuid();
    noise.adjustment = QJsonObject{{"kind", "Add Noise"}, {"noiseAmount", 20}, {"noiseSeed", 9}};
    noisy.layers.push_back(noise);
    cases.push_back({QStringLiteral("Canvas: two layers and Add Noise"), [&] { return LayerRenderer::flattened(noisy, RenderPurpose::Display); }});
    Document toned = document;
    Layer hue;
    hue.id = QUuid::createUuid();
    hue.adjustment = QJsonObject{{"kind", "Hue/Saturation"}, {"hsvSettings", QJsonObject{{"hue", 20}, {"saturation", 15}}}};
    toned.layers.push_back(hue);
    cases.push_back({QStringLiteral("Canvas: two layers and Hue/Saturation"), [&] { return LayerRenderer::flattened(toned, RenderPurpose::Display); }});
    // Layer effects on a large cut-out: a disc with a stroke, a drop shadow and both glows.
    QImage cutout(w / 2, h / 2, QImage::Format_RGBA8888_Premultiplied);
    cutout.fill(Qt::transparent);
    for (int y = 0; y < cutout.height(); ++y)
        for (int x = 0; x < cutout.width(); ++x)
            if (std::hypot(x - cutout.width() / 2.0, y - cutout.height() / 2.0) < cutout.height() * .4) {
                uchar *p = cutout.scanLine(y) + x * 4; p[0] = 200; p[1] = uchar(x % 256); p[2] = 90; p[3] = 255;
            }
    LayerEffects effects;
    effects.stroke = StrokeEffect{}; effects.stroke->size = 12;
    effects.shadow = ShadowEffect{}; effects.shadow->blur = 40;
    effects.outerGlow = OuterGlowEffect{};
    effects.innerGlow = InnerGlowEffect{};
    cases.push_back({QStringLiteral("Layer effects (stroke, shadow, glows)"), [&] { return LayerEffectsRenderer::render(cutout, {}, effects).first; }});
    cases.push_back({QStringLiteral("Canvas: two layers and Levels"), [&] { return LayerRenderer::flattened(adjusted, RenderPurpose::Display); }});

    // --subject: Remove Background's detection on a large photo.
    if (app.arguments().contains(QStringLiteral("--subject"))) {
        const QImage image = photo(w, h, 4);
        for (int i = 0; i < 3; ++i) {
            QElapsedTimer t;
            t.start();
            QString error;
            const QImage mask = SubjectRemoval::rawMask(image, &error);
            std::printf("rawMask: %.0f ms %s\n", t.nsecsElapsed() / 1e6, qPrintable(error));
        }
        const QImage mask = SubjectRemoval::rawMask(image);
        for (const auto &[name, settings] : std::vector<std::pair<const char *, SubjectRemovalSettings>>{
                 {"Basic", SubjectRemovalSettings{false, 12, 25, 0}}, {"Advanced, refine 12", SubjectRemovalSettings{true, 12, 25, 0}},
                 {"Advanced, refine 40, shift 4", SubjectRemovalSettings{true, 40, 25, 4}}}) {
            QImage out;
            for (double limit : {1e9, 1400.0}) {
                const double ms = timed([&] { return SubjectRemoval::refined(mask, image, settings, limit); }, &out, nullptr);
                quint64 hash = 1469598103934665603ull;
                for (int y = 0; y < out.height(); ++y)
                    for (const uchar *p = out.constScanLine(y), *e = p + out.width(); p < e; ++p) hash = (hash ^ *p) * 1099511628211ull;
                std::printf("refined (%s, %s): %.0f ms, %016llx\n", name, limit < 1e9 ? "preview" : "full size", ms,
                            static_cast<unsigned long long>(hash));
            }
        }
        QElapsedTimer t;
        t.start();
        const QImage small = image.scaled(320, 320, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::printf("  scaling down: %.0f ms\n", t.nsecsElapsed() / 1e6);
        t.restart();
        const QImage gray(320, 320, QImage::Format_Grayscale8);
        const QImage up = gray.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::printf("  scaling the mask up: %.0f ms\n", t.nsecsElapsed() / 1e6);
        return 0;
    }
    // --strokes: what one pointer move of Liquify and Smudge costs with a large brush (the dabs, not the canvas).
    if (app.arguments().contains(QStringLiteral("--strokes"))) {
        for (int mode : {0, 1, 2, 3})
            for (double diameter : {100.0, 400.0, 1000.0}) {
                EditorSession session;
                session.createDocument(w, h);
                session.insertImage(photo(w, h, 3), QStringLiteral("Photo"));
                QElapsedTimer t;
                t.start();
                // 0 Liquify, 1 Smudge, 2 a soft brush, 3 a hard one.
                if (mode < 2) session.beginWarpStroke(QPointF(w / 3.0, h / 2.0), mode, diameter, .5, .8);
                else session.beginBrushStroke(QPointF(w / 3.0, h / 2.0), QColor(200, 40, 30), diameter, mode == 2 ? .3 : 1, .8);
                const double begin = t.nsecsElapsed() / 1e6;
                std::vector<double> moves;
                for (int i = 1; i <= 40; ++i) {
                    t.restart();
                    session.continueBrushStroke(QPointF(w / 3.0 + i * diameter / 20, h / 2.0 + i * diameter / 40));
                    moves.push_back(t.nsecsElapsed() / 1e6);
                }
                t.restart();
                session.endBrushStroke();
                const double end = t.nsecsElapsed() / 1e6;
                quint64 hash = 1469598103934665603ull;
                const QImage &result = session.activeLayer()->image;
                for (int y = 0; y < result.height(); ++y)
                    for (const uchar *p = result.constScanLine(y), *e = p + result.width() * result.depth() / 8; p < e; ++p)
                        hash = (hash ^ *p) * 1099511628211ull;
                std::sort(moves.begin(), moves.end());
                std::printf("%s, %.0f px brush: begin %.0f ms, pointer move median %.1f ms (max %.1f), end %.0f ms, result %016llx\n",
                            std::array{"Liquify", "Smudge", "Soft brush", "Hard brush"}[size_t(mode)], diameter, begin, moves[moves.size() / 2], moves.back(), end,
                            static_cast<unsigned long long>(hash));
                std::fflush(stdout);
            }
        return 0;
    }
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
            {"Layer effects", [&] {
                 QImage cutout(w / 2, h / 2, QImage::Format_RGBA8888_Premultiplied);
                 cutout.fill(Qt::transparent);
                 for (int y = 0; y < cutout.height(); ++y)
                     for (int x = 0; x < cutout.width(); ++x)
                         if (std::hypot(x - cutout.width() / 2.0, y - cutout.height() / 2.0) < cutout.height() * .4) {
                             uchar *p = cutout.scanLine(y) + x * 4; p[0] = 200; p[1] = uchar(x % 256); p[2] = 90; p[3] = 255;
                         }
                 LayerEffects all;
                 all.stroke = StrokeEffect{}; all.stroke->size = 12;
                 all.shadow = ShadowEffect{}; all.shadow->blur = 40;
                 all.outerGlow = OuterGlowEffect{}; all.innerGlow = InnerGlowEffect{};
                 all.innerShadow = InnerShadowEffect{}; all.colorOverlay = ColorOverlayEffect{}; all.colorOverlay->opacity = .3;
                 return LayerEffectsRenderer::render(cutout, {}, all).first;
             }},
        };
        std::printf("| Filter (CPU, %d × %d) | ms | Output checksum |\n|---|---:|---|\n", w, h);
        for (const auto &[name, run] : survey) {
            QImage out;
            const double ms = timed(run, &out, nullptr);
            // A checksum, so runs with different thread counts (OMP_NUM_THREADS) can be compared for identical output.
            quint64 hash = 1469598103934665603ull;
            for (int y = 0; y < out.height(); ++y)
                for (const uchar *p = out.constScanLine(y), *end = p + out.width() * out.depth() / 8; p < end; ++p)
                    hash = (hash ^ *p) * 1099511628211ull;
            std::printf("| %s | %.0f | %016llx |\n", name, ms, static_cast<unsigned long long>(hash));
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
