// End-to-end rendering benchmark (B0). Prints Markdown tables for docs/GPU_ROLLOUT.md:
//   gpu_benchmark            all scenarios
//   gpu_benchmark --quick    smaller documents, fewer repetitions
//   gpu_benchmark --first-use  (internal) time the first GPU graph of a fresh process
// Medians of several runs; "cold" runs give every operation a new image, so nothing is
// served from the GPU upload cache. Automatic = the shipped default.
#include "core/CameraRaw.h"
#include "rendering/DeepImage.h"
#include "rendering/GpuOperations.h"
#include "rendering/GpuRenderer.h"
#include "rendering/LayerEffectsRenderer.h"
#include "rendering/LayerRenderer.h"
#include "rendering/ProgressiveBlur.h"
#include "rendering/RasterOperations.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QProcess>
#include <QSysInfo>
#include <QThreadPool>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <sys/resource.h>
using namespace compositor;

namespace {
bool quick = false;
QImage pattern(QSize size, QImage::Format format, int seed) {
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        uchar *p = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const int a = 128 + (x * 3 + y * 5 + seed) % 128;
            p[x * 4 + 0] = uchar(((x * 7 + seed) % 256) * a / 255);
            p[x * 4 + 1] = uchar(((y * 3 + seed * 5) % 256) * a / 255);
            p[x * 4 + 2] = uchar(((x + y) % 256) * a / 255);
            p[x * 4 + 3] = uchar(a);
        }
    }
    return image.convertToFormat(format);
}
Layer layer(const QImage &image, QPointF origin = {}) {
    Layer l;
    l.id = QUuid::createUuid();
    l.image = image;
    l.transform.origin = origin;
    l.transform.size = image.size();
    return l;
}
Layer adjustment(const QJsonObject &settings) {
    Layer l;
    l.id = QUuid::createUuid();
    l.adjustment = settings;
    return l;
}
void backend(const char *name) { qputenv("COMPOSITOR_RENDER_BACKEND", name); }
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
double percentile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * (v.size() - 1) + .5))];
}
double timeMs(const std::function<void()> &run, int repeats, bool warm = true) {
    if (warm)
        run();
    std::vector<double> t;
    for (int i = 0; i < repeats; ++i) {
        QElapsedTimer timer;
        timer.start();
        run();
        t.push_back(timer.nsecsElapsed() / 1e6);
    }
    return median(t);
}
QString ms(double v) { return v >= 100 ? QString::number(v, 'f', 0) : QString::number(v, 'f', 1); }
QString ratio(double cpu, double other) { return QStringLiteral("%1×").arg(cpu / other, 0, 'f', 2); }
void print(const QString &line) { std::printf("%s\n", line.toUtf8().constData()); }

Document document(QSize size, int layers, QImage::Format format, bool extras) {
    Document d;
    d.id = QUuid::createUuid();
    d.canvasSize = size;
    d.layers.push_back(layer(pattern(size, format, 0)));
    for (int i = 1; i < layers; ++i) {
        const QSize part(size.width() * 2 / 3, size.height() * 2 / 3);
        Layer l = layer(pattern(part, format, i), QPointF(size.width() / 7 * (i % 3), size.height() / 9 * (i % 4)));
        l.blendMode = BlendMode(std::array{2, 10, 6, 9, 16, 0, 11, 20, 1}[size_t(i % 9)]);
        if (extras && i % 4 == 1) {
            l.transform.rotation = 7;
            l.transform.origin += QPointF(.5, .25);
        }
        if (extras && i % 5 == 3) {
            l.mask = QImage(part, QImage::Format_Grayscale8);
            l.mask.fill(170);
        }
        d.layers.push_back(l);
    }
    if (extras) {
        d.layers.push_back(adjustment({{"kind", "Gaussian Blur"}, {"blurRadius", 4}}));
        d.layers.push_back(adjustment({{"kind", "Exposure"}, {"exposureSettings", QJsonObject{{"exposure", .3}}}}));
    }
    return d;
}
void documents() {
    print("### Documents (canvas render = `RenderPurpose::Display`, export = `Commit`)\n");
    print("| Document | CPU (ms) | Automatic canvas (ms) | Speedup | Automatic export (ms) | GPU working memory |");
    print("|---|---:|---:|---:|---:|---:|");
    struct Case {
        QString name;
        Document doc;
    };
    std::vector<Case> cases;
    const QSize hd(1920, 1080), mp12 = quick ? QSize(2000, 1500) : QSize(4000, 3000),
                mp24 = quick ? QSize(3000, 2000) : QSize(6000, 4000);
    cases.push_back({"1080p, 3 layers", document(hd, 3, QImage::Format_RGBA8888_Premultiplied, false)});
    cases.push_back({QStringLiteral("%1×%2, 5 layers + rotated/masked/filters").arg(mp12.width()).arg(mp12.height()),
                     document(mp12, 5, QImage::Format_RGBA8888_Premultiplied, true)});
    {
        Document d = document(mp24, 10, QImage::Format_RGBA8888_Premultiplied, true);
        LayerEffects fx;
        ShadowEffect shadow;
        shadow.blur = 12;
        fx.shadow = shadow;
        d.layers[2].effects = fx;
        cases.push_back({QStringLiteral("%1×%2, 10 layers + effects").arg(mp24.width()).arg(mp24.height()), d});
        Document clipped = d;
        clipped.layers[4].maskSourceId = clipped.layers[3].id;
        cases.push_back({QStringLiteral("%1×%2, 10 layers + clipping").arg(mp24.width()).arg(mp24.height()), clipped});
    }
    cases.push_back({QStringLiteral("%1×%2, 16-bit, 3 layers").arg(mp12.width()).arg(mp12.height()),
                     document(mp12, 3, QImage::Format_RGBA64_Premultiplied, false)});
    cases.push_back({QStringLiteral("%1×%2, HDR float, 3 layers").arg(mp12.width()).arg(mp12.height()),
                     document(mp12, 3, QImage::Format_RGBA32FPx4_Premultiplied, false)});
    const int repeats = quick ? 2 : 3;
    for (auto &c : cases) {
        // A new document id per run keeps the canvas prefix cache out of these numbers.
        const auto fresh = [&] {
            c.doc.id = QUuid::createUuid();
            return c.doc;
        };
        backend("cpu");
        const double cpu = timeMs([&] { (void)LayerRenderer::flattened(fresh(), RenderPurpose::Display); }, repeats);
        backend("auto");
        quint64 working = 0;
        const double canvas = timeMs(
            [&] {
                (void)LayerRenderer::flattened(fresh(), RenderPurpose::Display);
                for (const auto &r : GpuRenderer::recentExecutions())
                    working = std::max(working, r.workingBytes);
            },
            repeats);
        const double exported = timeMs([&] { (void)LayerRenderer::flattened(fresh(), RenderPurpose::Commit); }, repeats);
        print(QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 MiB |")
                  .arg(c.name, ms(cpu), ms(canvas), ratio(cpu, canvas), ms(exported))
                  .arg(working >> 20));
    }
    // B8: re-render after an edit to one layer near the top of a 12 MP, 10-layer document.
    Document d = document(mp12, 10, QImage::Format_RGBA8888_Premultiplied, false);
    int stroke = 0;
    for (const char *name : {"cpu", "auto"}) {
        backend(name);
        (void)LayerRenderer::flattened(d, RenderPurpose::Display);
        const double incremental = timeMs(
            [&] {
                d.layers[8].image.setPixelColor(10 + stroke % 50, 10 + stroke / 50 % 50, QColor::fromHsv(stroke * 7 % 360, 255, 255));
                ++stroke;
                (void)LayerRenderer::flattened(d, RenderPurpose::Display);
            },
            repeats + 2);
        const double full = timeMs(
            [&] {
                d.id = QUuid::createUuid();
                (void)LayerRenderer::flattened(d, RenderPurpose::Display);
            },
            repeats);
        print(QStringLiteral("| %1×%2, 10 layers, edit layer 9 (%3, prefix cache) | %4 full | %5 | %6 | — | — |")
                  .arg(mp12.width())
                  .arg(mp12.height())
                  .arg(QLatin1String(name), ms(full), ms(incremental), ratio(full, incremental)));
    }
    print("");
}
void filters() {
    print("### Filters at 1920×1080, 8-bit (cold = new image each run; warm = repeated image)\n");
    print("| Operation | CPU (ms) | GPU cold (ms) | GPU warm (ms) | Speedup cold |");
    print("|---|---:|---:|---:|---:|");
    const QImage image = pattern({1920, 1080}, QImage::Format_RGBA8888_Premultiplied, 3);
    struct Op {
        const char *name;
        std::function<QImage(const QImage &)> run;
    };
    ProgressiveBlurSettings progressive;
    progressive.to = 15;
    progressive.start = {960, 300};
    progressive.end = {960, 1080};
    const std::vector<Op> ops{
        {"Exposure", [](const QImage &i) { return RasterOperations::exposure(i, .75, .05, 1.2); }},
        {"Hue/Saturation", [](const QImage &i) { return RasterOperations::hueSaturation(i, HueSaturationSettings(25, 30, -12)); }},
        {"Gaussian 2.5", [](const QImage &i) { return RasterOperations::gaussianBlur(i, 2.5); }},
        {"Gaussian 12.5", [](const QImage &i) { return RasterOperations::gaussianBlur(i, 12.5); }},
        {"Gaussian 60", [](const QImage &i) { return RasterOperations::gaussianBlur(i, 60); }},
        {"Bloom 35/4", [](const QImage &i) { return RasterOperations::bloomGlow(i, 35, 4); }},
        {"Progressive Blur 0→15", [&](const QImage &i) { return progressive.apply(i); }},
        {"Motion Blur 13 px", [](const QImage &i) { return RasterOperations::motionBlur(i, -31, 13); }},
        {"Invert", [](const QImage &i) { return RasterOperations::inverted(i); }},
        {"Levels", [](const QImage &i) {
             LevelsSettings s;
             s.ranges[0] = {12, 1.2, 239, 3, 250};
             return RasterOperations::levels(i, s);
         }},
        {"Gradient Map", [](const QImage &i) { return RasterOperations::gradientMap(i, QColor(37, 5, 65), QColor(240, 210, 176), false); }},
        {"Black & White", [](const QImage &i) {
             const float w[6] = {.4f, .6f, .4f, .6f, .2f, .8f};
             return RasterOperations::blackWhite(i, w, true, 40, .2);
         }},
        {"Color Balance", [](const QImage &i) {
             const float a[3] = {.2f, -.15f, .1f}, b[3] = {.1f, .1f, -.1f}, c[3] = {-.1f, .1f, .15f};
             return RasterOperations::colorBalance(i, a, b, c, true);
         }},
    };
    const int repeats = quick ? 3 : 5;
    for (const auto &op : ops) {
        backend("cpu");
        const double cpu = timeMs([&] { op.run(image.copy()); }, op.name[0] == 'P' ? 1 : repeats);
        backend("vulkan");
        const double cold = timeMs([&] { op.run(image.copy()); }, repeats);
        const double warm = timeMs([&] { op.run(image); }, repeats);
        print(QStringLiteral("| %1 | %2 | %3 | %4 | %5 |")
                  .arg(QString::fromUtf8(op.name), ms(cpu), ms(cold), ms(warm), ratio(cpu, cold)));
    }
    // Layer-effect morphology (stroke) at a large radius.
    std::vector<float> coverage(2000 * 2000);
    for (size_t i = 0; i < coverage.size(); ++i)
        coverage[i] = float((i * 37) % 101) / 100.f;
    for (int radius : {3, 25, 250}) {
        backend("cpu");
        const double cpu = timeMs([&] { LayerEffectsRenderer::extreme(coverage, 2000, 2000, radius, false); }, 2);
        backend("vulkan");
        const double gpu = timeMs([&] { LayerEffectsRenderer::extreme(coverage, 2000, 2000, radius, false); }, 3);
        print(QStringLiteral("| Morphology r=%1, 2000×2000 coverage | %2 | %3 | — | %4 |")
                  .arg(radius)
                  .arg(ms(cpu), ms(gpu), ratio(cpu, gpu)));
    }
    print("");
}
void cameraRawAndBrush() {
    print("### Camera RAW develop and blur-brush latency\n");
    print("| Scenario | CPU | Automatic | Speedup |");
    print("|---|---:|---:|---:|");
    const QSize size = quick ? QSize(2000, 1500) : QSize(4000, 3000);
    const QImage photo = pattern(size, QImage::Format_RGBA8888_Premultiplied, 7);
    CameraRawSettings s;
    s.exposure = .7;
    s.temperature = 15;
    s.contrast = 20;
    backend("cpu");
    const double cpu = timeMs([&] { RasterOperations::cameraRaw(photo, s); }, 2);
    backend("auto");
    const double automatic = timeMs([&] { RasterOperations::cameraRaw(photo, s); }, 2);
    print(QStringLiteral("| Camera RAW develop, %1×%2 | %3 ms | %4 ms | %5 |")
              .arg(size.width())
              .arg(size.height())
              .arg(ms(cpu), ms(automatic), ratio(cpu, automatic)));
    // A blur-brush dab blurs the stroke's region on the GUI thread (here: the main thread).
    const QImage region = pattern({640, 640}, QImage::Format_RGBA8888_Premultiplied, 9);
    for (const char *name : {"cpu", "auto"}) {
        backend(name);
        std::vector<double> t;
        for (int i = 0; i < (quick ? 15 : 40); ++i) {
            const QImage dab = region.copy();
            QElapsedTimer timer;
            timer.start();
            RasterOperations::gaussianBlur(dab, 6);
            t.push_back(timer.nsecsElapsed() / 1e6);
        }
        print(QStringLiteral("| Blur-brush dab 640×640 r=6 (%1): p50 / p95 | %2 / %3 ms | | |")
                  .arg(QLatin1String(name), ms(percentile(t, .5)), ms(percentile(t, .95))));
    }
    print("");
}
void firstUse(const QString &program) {
    print("### First GPU use in a fresh process\n");
    print("| Pipeline cache | Device + first graph (ms) |");
    print("|---|---:|");
    for (bool cache : {false, true}) {
        // Run twice with the cache enabled: the first run writes it.
        for (int pass = 0; pass < (cache ? 2 : 1); ++pass) {
            QProcess process;
            auto env = QProcessEnvironment::systemEnvironment();
            if (!cache)
                env.insert("COMPOSITOR_GPU_NO_PIPELINE_CACHE", "1");
            process.setProcessEnvironment(env);
            process.start(program, {"--first-use"});
            process.waitForFinished(60000);
            if (cache && pass == 0)
                continue;
            print(QStringLiteral("| %1 | %2 |").arg(cache ? "warm (second launch)" : "disabled",
                                                   QString::fromUtf8(process.readAllStandardOutput()).trimmed()));
        }
    }
    print("");
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("CompositorLX");
    if (app.arguments().contains("--first-use")) {
        backend("vulkan");
        QElapsedTimer timer;
        timer.start();
        const QImage image = pattern({512, 512}, QImage::Format_RGBA8888_Premultiplied, 1);
        GpuGraph graph;
        int at = graph.gaussian(graph.source(image), 3, true);
        GpuParameters p{};
        p[GpuParam::Exposure::multiplier] = 1.2;
        p[GpuParam::Exposure::inverseGamma] = 1;
        at = graph.pass(GpuOp::Exposure, at, -1, p, 1);
        graph.execute(at, image.format());
        std::printf("%.1f\n", timer.nsecsElapsed() / 1e6);
        return 0;
    }
    quick = app.arguments().contains("--quick");
    backend("vulkan");
    print(QStringLiteral("Machine: %1; %2; %3 threads; GPU: %4")
              .arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture())
              .arg(QThreadPool::globalInstance()->maxThreadCount())
              .arg(vulkanDeviceDescription()));
    print(QStringLiteral("Budget %1 MiB%2\n").arg(GpuRenderer::memoryBudgetMiB()).arg(quick ? ", quick mode" : ""));
    firstUse(app.applicationFilePath());
    filters();
    documents();
    cameraRawAndBrush();
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    print(QStringLiteral("Peak resident memory: %1 MiB").arg(usage.ru_maxrss / 1024));
    return 0;
}
