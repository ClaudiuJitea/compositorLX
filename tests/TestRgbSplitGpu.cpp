#include "rendering/DeepImage.h"
#include "rendering/RgbSplit.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <future>
#include <iostream>
using namespace compositor;

static QImage fixture(QSize size, QImage::Format format)
{
    QImage image(size, QImage::Format_RGBA32FPx4_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        auto *p = reinterpret_cast<float *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const float a = ((x * 13 + y * 17) % 29) / 28.f;
            p[4*x] = a * ((x * 7 + y * 3) % 31) / 30.f;
            p[4*x+1] = a * ((x + y * 11) % 23) / 22.f;
            p[4*x+2] = a * ((x * 19 + y) % 37) / 36.f;
            if (format == QImage::Format_RGBA32FPx4_Premultiplied) p[4*x] *= 3; // HDR
            p[4*x+3] = a;
        }
    }
    return image.convertToFormat(format);
}
static double difference(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull()) return INFINITY;
    const auto x = DeepImage::floating(a);
    const auto y = DeepImage::floating(b);
    double worst = 0;
    for (int row = 0; row < x.height(); ++row) {
        const auto *p = reinterpret_cast<const float *>(x.constScanLine(row));
        const auto *q = reinterpret_cast<const float *>(y.constScanLine(row));
        for (int i = 0; i < x.width()*4; ++i) {
            if (!std::isfinite(q[i])) return INFINITY;
            worst = std::max(worst, double(std::abs(p[i]-q[i])));
        }
    }
    return worst;
}
class TestRgbSplitGpu : public QObject {
    Q_OBJECT
private slots:
    void cpuAndNoOp() {
        const auto image = fixture({17,13}, QImage::Format_RGBA8888_Premultiplied);
        RgbSplitSettings s; RgbSplitExecution report;
        QCOMPARE(s.apply(image, 15, RgbSplitBackend::Cpu, &report), s.applyCpu(image, 15));
        QCOMPARE(report.backend, RgbSplitBackend::Cpu);
        s.distance = s.blur = 0;
        QCOMPARE(s.apply(image, 15, RgbSplitBackend::Vulkan, &report), image);
        QCOMPARE(report.backend, RgbSplitBackend::Cpu);
        QVERIFY(s.apply({}).isNull());
    }
    void parity_data() {
        QTest::addColumn<int>("format"); QTest::addColumn<int>("edges"); QTest::addColumn<int>("mode");
        for (auto f : {QImage::Format_RGBA8888_Premultiplied, QImage::Format_RGBA64_Premultiplied, QImage::Format_RGBA32FPx4_Premultiplied})
            for (int edge = 0; edge < 16; ++edge)
                for (int mode = 0; mode < 3; ++mode)
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(int(f)).arg(edge).arg(mode))) << int(f) << edge << mode;
    }
    void parity() {
        QFETCH(int, format); QFETCH(int, edges); QFETCH(int, mode);
        const auto image = fixture({37,29}, QImage::Format(format));
        RgbSplitSettings s; s.angle = 37.25; s.center = {13.7,18.2}; s.reach = 29;
        s.distance = mode == 2 ? 40 : 7.25; s.blur = mode == 0 ? 0 : mode == 1 ? 9.5 : 75;
        s.radial = mode != 0;
        RgbSplitExecution report;
        const auto gpu = s.apply(image, edges, RgbSplitBackend::Vulkan, &report);
        if (report.backend != RgbSplitBackend::Vulkan) {
            QVERIFY2(!qEnvironmentVariableIsSet("COMPOSITOR_REQUIRE_VULKAN_TESTS"), qPrintable(report.fallbackReason));
            QSKIP(qPrintable(report.fallbackReason));
        }
        QCOMPARE(gpu.format(), image.format());
        const double error = difference(s.applyCpu(image, edges), gpu);
        const double tolerance = format == QImage::Format_RGBA8888_Premultiplied ? 1.01/255 : 0.0001;
        QVERIFY2(error <= tolerance, qPrintable(QString("maximum channel error %1 > %2").arg(error,0,'g',10).arg(tolerance)));
    }
    void largeCoordinates() {
        const auto image = fixture({2049,17}, QImage::Format_RGBA32FPx4_Premultiplied);
        RgbSplitSettings s; s.distance = 10; s.angle = 23.5;
        RgbSplitExecution report;
        const auto gpu = s.apply(image,15,RgbSplitBackend::Vulkan,&report);
        const double error = difference(gpu,s.applyCpu(image,15));
        QVERIFY2(error < .0001, qPrintable(QString("large-coordinate error %1").arg(error,0,'g',10)));
    }
    void automaticAndOverrides() {
        const QByteArray saved = qgetenv("COMPOSITOR_RGB_SPLIT_BACKEND");
        qunsetenv("COMPOSITOR_RGB_SPLIT_BACKEND");
        const auto image = fixture({512,512}, QImage::Format_RGBA8888_Premultiplied);
        RgbSplitSettings s; RgbSplitExecution automatic, forced;
        const auto a = s.apply(image,15,RgbSplitBackend::Auto,&automatic);
        const auto b = s.apply(image,15,RgbSplitBackend::Vulkan,&forced);
        QCOMPARE(automatic.backend,forced.backend); QCOMPARE(a,b);
        qputenv("COMPOSITOR_RGB_SPLIT_BACKEND","cpu");
        QCOMPARE(s.apply(image,15),s.applyCpu(image,15));
        qputenv("COMPOSITOR_RGB_SPLIT_BACKEND","vulkan");
        QCOMPARE(s.apply(image,15),b);
        qunsetenv("COMPOSITOR_RGB_SPLIT_BACKEND");
        s.distance = .1;
        s.apply(image,15,RgbSplitBackend::Auto,&automatic);
        QCOMPARE(automatic.backend,RgbSplitBackend::Cpu);
        s.distance = 10;
        s.apply(image.scaled(32,32),15,RgbSplitBackend::Auto,&automatic);
        QCOMPARE(automatic.backend,RgbSplitBackend::Cpu);
        if (saved.isNull()) qunsetenv("COMPOSITOR_RGB_SPLIT_BACKEND");
        else qputenv("COMPOSITOR_RGB_SPLIT_BACKEND",saved);
    }
    void invalidSettings() {
        const auto image = fixture({9,7},QImage::Format_RGBA8888_Premultiplied);
        RgbSplitSettings s; s.distance = std::numeric_limits<double>::quiet_NaN();
        QCOMPARE(s.apply(image),image);
        s.distance = 10; s.angle = INFINITY;
        QCOMPARE(s.apply(image,15,RgbSplitBackend::Vulkan),image);
    }
    void tinyAndConcurrent() {
        RgbSplitSettings s; s.distance = 1; s.blur = 3;
        for (QSize size : {QSize(1,1), QSize(1,19), QSize(23,1), QSize(8,8)}) {
            const auto image = fixture(size, QImage::Format_RGBA32FPx4_Premultiplied);
            RgbSplitExecution report;
            const auto actual = s.apply(image, 15, RgbSplitBackend::Vulkan, &report);
            QVERIFY(difference(actual, s.applyCpu(image,15)) < .0001);
        }
        const auto image = fixture({97,65}, QImage::Format_RGBA8888_Premultiplied);
        auto render = [&] { return s.apply(image, 0, RgbSplitBackend::Vulkan); };
        auto a = std::async(std::launch::async, render);
        auto b = std::async(std::launch::async, render);
        QCOMPARE(a.get(), b.get());
    }
};
static int benchmark()
{
    for (QSize size : {QSize(128,128), QSize(512,512), QSize(1920,1080)}) {
        const auto image = fixture(size, QImage::Format_RGBA8888_Premultiplied);
        for (bool heavy : {false, true}) {
            RgbSplitSettings s; s.distance = heavy ? 30 : 10; s.blur = heavy ? 12 : 0;
            s.radial = heavy; s.center = QPointF(size.width()/2.,size.height()/2.); s.reach = size.width();
            double timings[2]{};
            for (int i = 0; i < 2; ++i) {
                const auto backend = i ? RgbSplitBackend::Vulkan : RgbSplitBackend::Cpu;
                QElapsedTimer timer; timer.start(); RgbSplitExecution report;
                s.apply(image,15,backend,&report);
                const double cold = timer.nsecsElapsed()/1e6;
                if (i && report.backend != backend) { std::cerr << qPrintable(report.fallbackReason) << '\n'; return 1; }
                double runs[3];
                for (double &run : runs) { timer.restart(); s.apply(image,15,backend); run = timer.nsecsElapsed()/1e6; }
                std::sort(std::begin(runs),std::end(runs)); timings[i] = runs[1];
                std::cout << size.width() << 'x' << size.height() << (heavy ? " heavy " : " normal ") << (i ? "Vulkan " : "CPU ")
                          << "cold_ms=" << cold << " median_ms=" << timings[i] << ' ' << qPrintable(report.deviceName) << std::endl;
            }
            std::cout << "speedup=" << timings[0]/timings[1] << std::endl;
        }
    }
    return 0;
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc,argv);
    if (app.arguments().contains("--benchmark")) return benchmark();
    if (app.arguments().contains("--fallback")) {
        const auto image = fixture({31,21},QImage::Format_RGBA8888_Premultiplied);
        RgbSplitSettings s; RgbSplitExecution report;
        const auto actual = s.apply(image,15,RgbSplitBackend::Vulkan,&report);
        std::cout << qPrintable(report.fallbackReason) << std::endl;
        return report.backend == RgbSplitBackend::Cpu && !report.fallbackReason.isEmpty() && actual == s.applyCpu(image,15) ? 0 : 1;
    }
    TestRgbSplitGpu test; return QTest::qExec(&test,argc,argv);
}
#include "TestRgbSplitGpu.moc"
