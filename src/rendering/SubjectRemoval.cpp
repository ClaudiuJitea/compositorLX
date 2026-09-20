#include "rendering/SubjectRemoval.h"

#include "rendering/RasterOperations.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

namespace compositor {
namespace {

QString modelPath()
{
    const QString environment = qEnvironmentVariable("COMPOSITOR_U2NETP_MODEL");
    if (QFileInfo::exists(environment)) return environment;
    const QDir executable(QCoreApplication::applicationDirPath());
    const QString installed = executable.absoluteFilePath(QStringLiteral("../share/compositor-lx/models/u2netp.onnx"));
    if (QFileInfo::exists(installed)) return installed;
    const QString buildTree = executable.absoluteFilePath(QStringLiteral("models/u2netp.onnx"));
    if (QFileInfo::exists(buildTree)) return buildTree;
    return installed;
}

struct Runtime {
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "CompositorLX"};
    Ort::SessionOptions options;
    std::unique_ptr<Ort::Session> session;
    std::string error;

    Runtime()
    {
        try {
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.SetIntraOpNumThreads(std::max(1u, std::thread::hardware_concurrency() / 2));
            const QByteArray path = QFile::encodeName(modelPath());
            session = std::make_unique<Ort::Session>(environment, path.constData(), options);
        } catch (const Ort::Exception &exception) { error = exception.what(); }
    }
};

Runtime &runtime()
{
    static Runtime value;
    return value;
}

std::vector<float> boxFilter(const std::vector<float> &source, int width, int height, int radius)
{
    const float span = float(radius * 2 + 1);
    std::vector<float> pass(size_t(width) * height), result(size_t(width) * height);
    for (int y = 0; y < height; ++y) {
        const int row = y * width; float sum = 0;
        for (int x = -radius; x <= radius; ++x) sum += source[size_t(row + std::clamp(x, 0, width - 1))];
        for (int x = 0; x < width; ++x) {
            pass[size_t(row + x)] = sum / span;
            sum -= source[size_t(row + std::clamp(x - radius, 0, width - 1))];
            sum += source[size_t(row + std::clamp(x + radius + 1, 0, width - 1))];
        }
    }
    for (int x = 0; x < width; ++x) {
        float sum = 0;
        for (int y = -radius; y <= radius; ++y) sum += pass[size_t(std::clamp(y, 0, height - 1) * width + x)];
        for (int y = 0; y < height; ++y) {
            result[size_t(y * width + x)] = sum / span;
            sum -= pass[size_t(std::clamp(y - radius, 0, height - 1) * width + x)];
            sum += pass[size_t(std::clamp(y + radius + 1, 0, height - 1) * width + x)];
        }
    }
    return result;
}

std::vector<float> grayLevels(const QImage &source, const QSize &size)
{
    const QImage image = source.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    std::vector<float> result(size_t(size.width()) * size.height());
    for (int y = 0; y < size.height(); ++y) {
        const uchar *row = image.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) result[size_t(y * size.width() + x)] = row[x] / 255.0f;
    }
    return result;
}

QImage levelsImage(const std::vector<float> &levels, const QSize &size)
{
    QImage result(size, QImage::Format_Grayscale8);
    for (int y = 0; y < size.height(); ++y) {
        uchar *row = result.scanLine(y);
        for (int x = 0; x < size.width(); ++x)
            row[x] = uchar(std::clamp(qRound(levels[size_t(y * size.width() + x)] * 255), 0, 255));
    }
    return result;
}

} // namespace

QImage SubjectRemoval::rawMask(const QImage &source, QString *error)
{
    if (source.isNull()) { if (error) *error = QObject::tr("The selected layer has no pixels."); return {}; }
    Runtime &engine = runtime();
    if (!engine.session) { if (error) *error = QString::fromStdString(engine.error); return {}; }
    try {
        constexpr int side = 320;
        const QImage image = source.scaled(side, side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                   .convertToFormat(QImage::Format_RGBA8888);
        float maximum = 0;
        for (int y = 0; y < side; ++y) { const uchar *row = image.constScanLine(y); for (int x = 0; x < side; ++x)
            maximum = std::max({maximum, float(row[x * 4]), float(row[x * 4 + 1]), float(row[x * 4 + 2])}); }
        maximum = std::max(maximum, 1e-6f);
        std::vector<float> input(size_t(3 * side * side));
        constexpr std::array<float, 3> mean{.485f, .456f, .406f}, deviation{.229f, .224f, .225f};
        for (int y = 0; y < side; ++y) { const uchar *row = image.constScanLine(y); for (int x = 0; x < side; ++x) for (int channel = 0; channel < 3; ++channel)
            input[size_t(channel * side * side + y * side + x)] = (row[x * 4 + channel] / maximum - mean[channel]) / deviation[channel]; }
        const std::array<int64_t, 4> shape{1, 3, side, side};
        Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        const auto inputName = engine.session->GetInputNameAllocated(0, allocator);
        const auto outputName = engine.session->GetOutputNameAllocated(0, allocator);
        const char *inputs[] = {inputName.get()}, *outputs[] = {outputName.get()};
        auto values = engine.session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        const float *prediction = values.front().GetTensorData<float>();
        const size_t count = size_t(side * side);
        const auto bounds = std::minmax_element(prediction, prediction + count);
        const float range = *bounds.second - *bounds.first;
        if (!std::isfinite(range) || range < 1e-6f) { if (error) *error = QObject::tr("No foreground subject was detected in this layer."); return {}; }
        std::vector<float> normalized(count);
        for (size_t i = 0; i < count; ++i) normalized[i] = std::clamp((prediction[i] - *bounds.first) / range, 0.0f, 1.0f);
        return levelsImage(normalized, QSize(side, side)).scaled(source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } catch (const Ort::Exception &exception) {
        if (error) *error = QString::fromUtf8(exception.what());
        return {};
    }
}

QImage SubjectRemoval::refined(const QImage &inputMask, const QImage &guide, const SubjectRemovalSettings &settings)
{
    if (inputMask.isNull() || guide.isNull()) return {};
    QImage mask = inputMask.scaled(guide.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    if (!settings.advanced) return mask;
    const int width = mask.width(), height = mask.height(), count = width * height;
    if (settings.refineEdges > 0) {
        const int radius = std::max(1, qRound(std::clamp(settings.refineEdges, 0.0, 40.0)));
        const std::vector<float> matte = grayLevels(mask, mask.size()), gray = grayLevels(guide, mask.size());
        const auto meanGray = boxFilter(gray, width, height, radius), meanMask = boxFilter(matte, width, height, radius);
        std::vector<float> squares(static_cast<size_t>(count)), products(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) { squares[size_t(i)] = gray[size_t(i)] * gray[size_t(i)]; products[size_t(i)] = gray[size_t(i)] * matte[size_t(i)]; }
        const auto meanSquares = boxFilter(squares, width, height, radius), meanProducts = boxFilter(products, width, height, radius);
        std::vector<float> slope(static_cast<size_t>(count)), offset(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) { const float variance = meanSquares[size_t(i)] - meanGray[size_t(i)] * meanGray[size_t(i)]; const float covariance = meanProducts[size_t(i)] - meanGray[size_t(i)] * meanMask[size_t(i)]; slope[size_t(i)] = covariance / (variance + 1e-4f); offset[size_t(i)] = meanMask[size_t(i)] - slope[size_t(i)] * meanGray[size_t(i)]; }
        const auto meanSlope = boxFilter(slope, width, height, radius), meanOffset = boxFilter(offset, width, height, radius);
        std::vector<float> result(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) result[size_t(i)] = std::clamp(meanSlope[size_t(i)] * gray[size_t(i)] + meanOffset[size_t(i)], 0.0f, 1.0f);
        mask = levelsImage(result, mask.size());
    }
    if (settings.shiftEdge != 0) {
        const double reach = std::abs(std::clamp(settings.shiftEdge, -10.0, 10.0));
        QImage rgba(mask.size(), QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < height; ++y) { const uchar *in = mask.constScanLine(y); uchar *out = rgba.scanLine(y); for (int x = 0; x < width; ++x) out[x*4]=out[x*4+1]=out[x*4+2]=out[x*4+3]=in[x]; }
        const QImage blurred = RasterOperations::gaussianBlur(rgba, reach / 2).convertToFormat(QImage::Format_RGBA8888);
        const int threshold = settings.shiftEdge < 0 ? 191 : 64;
        for (int y = 0; y < height; ++y) { uchar *out = mask.scanLine(y); const uchar *in = blurred.constScanLine(y); for (int x = 0; x < width; ++x) out[x] = in[x*4] <= threshold ? 0 : 255; }
    }
    if (settings.matteContrast > 0) {
        const double strength = std::clamp(settings.matteContrast, 0.0, 100.0) / 100.0;
        const double slope = 1 / std::max(.02, 1 - strength * .98);
        for (int y = 0; y < height; ++y) { uchar *row = mask.scanLine(y); for (int x = 0; x < width; ++x)
            row[x] = uchar(std::clamp(qRound(((row[x] / 255.0 - .5) * slope + .5) * 255), 0, 255)); }
    }
    return mask;
}

} // namespace compositor
