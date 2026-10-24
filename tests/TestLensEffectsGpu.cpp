#include "rendering/DeepImage.h"
#include "rendering/GpuRenderer.h"
#include "rendering/LensEffects.h"
#include <QCoreApplication>
#include <QTest>
#include <algorithm>
#include <cmath>
using namespace compositor;

// Filter › Lens Effects on Vulkan (as Core Image runs them on the Mac's GPU) against the CPU planes they mirror: every
// effect, in 8-bit, 16-bit and HDR layers, within a stated tolerance; and the graph features they rely on.
namespace {
// A dusky picture with lights of a few sizes and colors, some at the edges, over a soft gradient.
QImage night(QSize size, QImage::Format format)
{
    QImage image(size, QImage::Format_RGBA32FPx4_Premultiplied);
    const bool hdr = format == QImage::Format_RGBA32FPx4_Premultiplied;
    for (int y = 0; y < size.height(); ++y) {
        auto *p = reinterpret_cast<float *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const float a = x < 6 ? .6f : 1.f; // a partly transparent strip keeps the layer's alpha honest
            p[4 * x] = a * (.08f + .1f * x / size.width());
            p[4 * x + 1] = a * (.1f + .08f * y / size.height());
            p[4 * x + 2] = a * .16f;
            p[4 * x + 3] = a;
        }
    }
    struct Light { double x, y, r; float red, green, blue; };
    const Light lights[] = {{.5, .45, 6, 1, 1, 1}, {.2, .3, 3, 1, .8, .6}, {.8, .7, 10, .7, .9, 1}, {.02, .5, 4, 1, 1, 1},
                            {.95, .1, 2, 1, .5, .5}, {.6, .9, 5, 1, 1, .7}};
    for (const Light &l : lights)
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x) {
                const double d = std::hypot(x + .5 - l.x * size.width(), y + .5 - l.y * size.height());
                if (d > l.r) continue;
                auto *p = reinterpret_cast<float *>(image.scanLine(y)) + 4 * x;
                const float boost = hdr ? 3.f : 1.f; // brighter than white in an HDR layer
                p[0] = l.red * boost * p[3]; p[1] = l.green * boost * p[3]; p[2] = l.blue * boost * p[3];
            }
    return image.convertToFormat(format);
}
// The largest channel difference, relative to the value past 1 (so an HDR layer's half floats count in their steps).
double difference(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull()) return INFINITY;
    const QImage x = DeepImage::floating(a), y = DeepImage::floating(b);
    double worst = 0;
    for (int row = 0; row < x.height(); ++row) {
        const auto *p = reinterpret_cast<const float *>(x.constScanLine(row));
        const auto *q = reinterpret_cast<const float *>(y.constScanLine(row));
        for (int i = 0; i < x.width() * 4; ++i) {
            if (!std::isfinite(q[i])) return INFINITY;
            worst = std::max(worst, double(std::abs(p[i] - q[i])) / std::max(1.0, double(std::abs(p[i]))));
        }
    }
    return worst;
}
void requireVulkan(bool accelerated)
{
    if (accelerated) return;
    QVERIFY2(!qEnvironmentVariableIsSet("COMPOSITOR_REQUIRE_VULKAN_TESTS"), qPrintable(GpuRenderer::lastExecution().reason));
    QSKIP("Vulkan unavailable");
}
} // namespace

class TestLensEffectsGpu : public QObject {
    Q_OBJECT
private slots:
    void parity_data()
    {
        QTest::addColumn<int>("format");
        QTest::addColumn<QString>("variant");
        const QStringList variants{"flare", "flare-98", "flare-blue-0", "star-6", "star-4-0", "star-14-45", "mist",
                                   "mist-100", "prism", "prism-both-blur", "prism-up"};
        for (auto format : {QImage::Format_RGBA8888_Premultiplied, QImage::Format_RGBA64_Premultiplied,
                            QImage::Format_RGBA32FPx4_Premultiplied})
            for (const QString &variant : variants)
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(int(format)).arg(variant))) << int(format) << variant;
    }
    void parity()
    {
        QFETCH(int, format);
        QFETCH(QString, variant);
        LensEffectSettings s;
        if (variant.startsWith("flare")) {
            s.effect = LensEffect::AnamorphicFlare; s.flareAmount = 80;
            if (variant == "flare-98") s.threshold = 98;
            if (variant == "flare-blue-0") s.flareBlue = 0;
        } else if (variant.startsWith("star")) {
            s.effect = LensEffect::Starburst; s.starAmount = 90;
            const QStringList parts = variant.split('-');
            s.starPoints = parts[1].toInt();
            if (parts.size() > 2) s.starAngle = parts[2].toDouble();
            s.starLength = 20;
        } else if (variant.startsWith("mist")) {
            s.effect = LensEffect::Mist; s.mistStrength = 70;
            if (variant == "mist-100") s.mistSize = 100;
        } else {
            s.effect = LensEffect::Prism; s.prismOffset = 8; s.prismCopies = 4;
            if (variant == "prism-both-blur") { s.prismBothWays = true; s.prismBlur = 60; }
            if (variant == "prism-up") s.prismAngle = 90;
        }
        // Large enough that the blurs run on their smaller copies (σ past 16) and Starburst goes in several bands.
        const QImage image = night({900, 600}, QImage::Format(format));
        bool accelerated = false;
        const QImage gpu = s.apply(image, RenderBackend::Vulkan, &accelerated);
        requireVulkan(accelerated);
        bool cpuAccelerated = true;
        const QImage cpu = s.apply(image, RenderBackend::Cpu, &cpuAccelerated);
        QVERIFY(!cpuAccelerated);
        QCOMPARE(gpu.format(), cpu.format());
        // 8-bit: a level at most (float order, and float against the CPU's doubles); 16-bit: a few of its steps;
        // HDR (stored as half floats): a couple of half-float steps.
        const double error = difference(cpu, gpu);
        const double tolerance = format == QImage::Format_RGBA8888_Premultiplied ? 1.01 / 255
                                 : format == QImage::Format_RGBA64_Premultiplied ? 2e-3 : 4e-3;
        QVERIFY2(error <= tolerance, qPrintable(QStringLiteral("maximum channel error %1 > %2").arg(error, 0, 'g', 6).arg(tolerance)));
    }
    // Automatic leaves small pictures to the CPU.
    void automatic()
    {
        LensEffectSettings s; s.effect = LensEffect::Mist;
        const QImage small = night({64, 48}, QImage::Format_RGBA8888_Premultiplied);
        bool accelerated = true;
        const QImage automatic = s.apply(small, RenderBackend::Auto, &accelerated);
        QVERIFY(!accelerated);
        QCOMPARE(automatic, s.apply(small, RenderBackend::Cpu));
    }
    // GpuGraph::update: a Lens pass that writes into its input over a region, leaving the rest as it was.
    void regionUpdate()
    {
        const QSize size(37, 23);
        auto values = std::make_shared<std::vector<float>>(size_t(size.width()) * size.height());
        for (size_t i = 0; i < values->size(); ++i) (*values)[i] = float(i % 17) / 16;
        GpuGraph graph;
        const int source = graph.channel(values, size);
        GpuParameters copy{}; copy[GpuParam::LensCombine::a] = 1;
        const int plane = graph.pass(GpuOp::LensCombine, source, source, copy);
        GpuParameters add{}; add[GpuParam::LensCombine::a] = 1; add[GpuParam::LensCombine::b] = 2;
        const QRect area(5, 3, 11, 7);
        const int updated = graph.update(GpuOp::LensCombine, plane, source, add, area);
        GpuExecution report;
        const std::vector<float> out = graph.executeChannel(updated, RenderBackend::Vulkan, &report);
        requireVulkan(report.accelerated);
        for (int y = 0; y < size.height(); ++y)
            for (int x = 0; x < size.width(); ++x) {
                const float v = (*values)[size_t(y) * size.width() + x];
                QCOMPARE(out[size_t(y) * size.width() + x], area.contains(x, y) ? 3 * v : v);
            }
        // An update of a plane that is read again later is refused (and so falls back), not silently wrong.
        GpuGraph bad;
        const int s2 = bad.channel(values, size);
        const int p2 = bad.pass(GpuOp::LensCombine, s2, s2, copy);
        const int u2 = bad.update(GpuOp::LensCombine, p2, s2, add, area);
        const int both = bad.pass(GpuOp::LensCombine, u2, p2, add);
        GpuExecution refused;
        QVERIFY(bad.executeChannel(both, RenderBackend::Vulkan, &refused).empty());
        QVERIFY(refused.reason.contains(QStringLiteral("in-place")));
    }
    // A source whose memory a later pass reuses is uploaded again by the next graph that uses it.
    void reusedSourceUploadsAgain()
    {
        const QSize size(300, 200);
        auto values = std::make_shared<std::vector<float>>(size_t(size.width()) * size.height(), .25f);
        const auto run = [&] {
            GpuGraph graph;
            const int source = graph.channel(values, size);
            GpuParameters linear{}; // LensPoint mode 0
            int node = graph.pass(GpuOp::LensPoint, source, -1, linear);
            GpuParameters twice{}; twice[GpuParam::LensCombine::a] = 1; twice[GpuParam::LensCombine::b] = 1;
            for (int i = 0; i < 4; ++i) node = graph.pass(GpuOp::LensCombine, node, node, twice);
            GpuExecution report;
            auto out = graph.executeChannel(node, RenderBackend::Vulkan, &report);
            return std::make_pair(out, report.accelerated);
        };
        const auto first = run();
        requireVulkan(first.second);
        const auto second = run();
        QCOMPARE(second.first, first.first);
        const float expected = 16 * std::pow((.25f + .055f) / 1.055f, 2.4f);
        QVERIFY(std::abs(first.first.front() - expected) < 1e-5);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Without a Vulkan driver every effect falls back to the CPU and draws exactly what the CPU does.
    if (app.arguments().contains(QStringLiteral("--fallback"))) {
        const QImage image = night({600, 450}, QImage::Format_RGBA8888_Premultiplied);
        for (LensEffect effect : LensEffectSettings::effects) {
            LensEffectSettings s; s.effect = effect;
            bool accelerated = true;
            const QImage gpu = s.apply(image, RenderBackend::Vulkan, &accelerated);
            if (accelerated || gpu != s.apply(image, RenderBackend::Cpu)) return 1;
        }
        return 0;
    }
    TestLensEffectsGpu test;
    return QTest::qExec(&test, argc, argv);
}
#include "TestLensEffectsGpu.moc"
