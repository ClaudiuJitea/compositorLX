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
#include <queue>
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
    const QString testBuildTree = executable.absoluteFilePath(QStringLiteral("../models/u2netp.onnx"));
    if (QFileInfo::exists(testBuildTree)) return testBuildTree;
    const QString currentDir = QDir::current().absoluteFilePath(QStringLiteral("models/u2netp.onnx"));
    if (QFileInfo::exists(currentDir)) return currentDir;
    const QString buildDir = QDir::current().absoluteFilePath(QStringLiteral("build/models/u2netp.onnx"));
    if (QFileInfo::exists(buildDir)) return buildDir;
    const QString parentBuildDir = QDir::current().absoluteFilePath(QStringLiteral("../build/models/u2netp.onnx"));
    if (QFileInfo::exists(parentBuildDir)) return parentBuildDir;
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

static std::mutex s_providerMutex;
static SubjectRemoval::SegmentationProvider s_customProvider;

} // namespace

void SubjectRemoval::setSegmentationProvider(SegmentationProvider provider)
{
    std::lock_guard<std::mutex> lock(s_providerMutex);
    s_customProvider = std::move(provider);
}

void SubjectRemoval::resetSegmentationProvider()
{
    std::lock_guard<std::mutex> lock(s_providerMutex);
    s_customProvider = nullptr;
}

bool SubjectRemoval::isModelAvailable()
{
    return QFileInfo::exists(modelPath());
}

QString SubjectRemoval::modelFilePath()
{
    return modelPath();
}

QImage SubjectRemoval::rawMask(const QImage &source, QString *error, std::atomic<bool> *cancelled)
{
    if (cancelled && cancelled->load()) return {};
    {
        std::lock_guard<std::mutex> lock(s_providerMutex);
        if (s_customProvider) return s_customProvider(source, error, cancelled);
    }
    if (source.isNull()) { if (error) *error = QObject::tr("The selected layer has no pixels."); return {}; }

    const QImage converted = source.convertToFormat(QImage::Format_RGBA8888);
    bool hasOpaque = false;
    for (int y = 0; y < converted.height(); ++y) {
        const uchar *row = converted.constScanLine(y);
        for (int x = 0; x < converted.width(); ++x) {
            if (row[x * 4 + 3] > 0) { hasOpaque = true; break; }
        }
        if (hasOpaque) break;
    }
    if (!hasOpaque) {
        if (error) *error = QObject::tr("The selected layer has no pixels.");
        return {};
    }

    if (cancelled && cancelled->load()) return {};
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
        // Cancellation checkpoint: before native inference
        if (cancelled && cancelled->load()) return {};
        // Note: ONNX Runtime Session::Run executes synchronously as an atomic compute kernel on CPU.
        // In-flight cancellation currently takes effect immediately after Run() completes or at stage boundaries.
        auto values = engine.session->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        // Cancellation checkpoint: immediately after native inference (takes effect here if cancelled during Run)
        if (cancelled && cancelled->load()) return {};
        const float *prediction = values.front().GetTensorData<float>();
        const size_t count = size_t(side * side);
        const auto bounds = std::minmax_element(prediction, prediction + count);
        const float range = *bounds.second - *bounds.first;
        if (!std::isfinite(range) || range < 1e-6f) { if (error) *error = QObject::tr("No foreground subject was detected in this layer."); return {}; }
        std::vector<float> normalized(count);
        for (size_t i = 0; i < count; ++i) normalized[i] = std::clamp((prediction[i] - *bounds.first) / range, 0.0f, 1.0f);
        if (cancelled && cancelled->load()) return {};
        return levelsImage(normalized, QSize(side, side)).scaled(source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } catch (const Ort::Exception &exception) {
        if (error) *error = QString::fromUtf8(exception.what());
        return {};
    }
}

QImage SubjectRemoval::connectedInstance(const QImage &mask, const QPoint &clickPoint, std::atomic<bool> *cancelled)
{
    if (mask.isNull() || !QRect(QPoint(), mask.size()).contains(clickPoint)) return {};
    const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8);
    const int clickX = clickPoint.x(), clickY = clickPoint.y();
    if (gray.constScanLine(clickY)[clickX] < 128) return {};

    const int width = gray.width(), height = gray.height();
    QImage instance(width, height, QImage::Format_Grayscale8);
    instance.fill(0);

    std::vector<bool> visited(size_t(width) * height, false);
    std::queue<QPoint> queue;

    visited[size_t(clickY) * width + clickX] = true;
    instance.scanLine(clickY)[clickX] = 255;
    queue.push(clickPoint);

    constexpr int dx[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    constexpr int dy[8] = {0, 0, -1, 1, -1, -1, 1, 1};

    while (!queue.empty()) {
        if (cancelled && cancelled->load()) return {};
        const QPoint current = queue.front();
        queue.pop();

        for (int i = 0; i < 8; ++i) {
            const int nx = current.x() + dx[i];
            const int ny = current.y() + dy[i];
            if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                const size_t idx = size_t(ny) * width + nx;
                if (!visited[idx]) {
                    visited[idx] = true;
                    if (gray.constScanLine(ny)[nx] >= 128) {
                        instance.scanLine(ny)[nx] = 255;
                        queue.push(QPoint(nx, ny));
                    }
                }
            }
        }
    }
    return instance;
}

QImage SubjectRemoval::adjustEdgeOffset(const QImage &mask, int edgeOffset)
{
    const int steps = std::min(10, std::abs(edgeOffset));
    if (steps == 0 || mask.isNull()) return mask;
    QImage current = mask.convertToFormat(QImage::Format_Grayscale8);
    const int width = current.width(), height = current.height();

    for (int step = 0; step < steps; ++step) {
        QImage next = current;
        if (edgeOffset > 0) {
            for (int y = 0; y < height; ++y) {
                const uchar *prev = y > 0 ? current.constScanLine(y - 1) : nullptr;
                const uchar *cur = current.constScanLine(y);
                const uchar *nextRow = y < height - 1 ? current.constScanLine(y + 1) : nullptr;
                uchar *out = next.scanLine(y);
                for (int x = 0; x < width; ++x) {
                    if (cur[x] != 0) {
                        if ((x > 0 && cur[x - 1] == 0) ||
                            (x < width - 1 && cur[x + 1] == 0) ||
                            (prev && prev[x] == 0) ||
                            (nextRow && nextRow[x] == 0)) {
                            out[x] = 0;
                        }
                    }
                }
            }
        } else {
            for (int y = 0; y < height; ++y) {
                const uchar *prev = y > 0 ? current.constScanLine(y - 1) : nullptr;
                const uchar *cur = current.constScanLine(y);
                const uchar *nextRow = y < height - 1 ? current.constScanLine(y + 1) : nullptr;
                uchar *out = next.scanLine(y);
                for (int x = 0; x < width; ++x) {
                    if (cur[x] == 0) {
                        if ((x > 0 && cur[x - 1] != 0) ||
                            (x < width - 1 && cur[x + 1] != 0) ||
                            (prev && prev[x] != 0) ||
                            (nextRow && nextRow[x] != 0)) {
                            out[x] = 255;
                        }
                    }
                }
            }
        }
        current = next;
    }
    return current;
}

QImage SubjectRemoval::smoothBinaryMask(const QImage &mask)
{
    if (mask.isNull()) return {};
    const QImage gray = mask.convertToFormat(QImage::Format_Grayscale8);
    const int width = gray.width(), height = gray.height();
    QImage softened(width, height, QImage::Format_Grayscale8);
    softened.fill(0);

    for (int y = 0; y < height; ++y) {
        uchar *out = softened.scanLine(y);
        for (int x = 0; x < width; ++x) {
            if (gray.constScanLine(y)[x] == 0) continue;
            int sum = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int sy = y + dy;
                if (sy < 0 || sy >= height) continue;
                const uchar *row = gray.constScanLine(sy);
                for (int dx = -1; dx <= 1; ++dx) {
                    const int sx = x + dx;
                    if (sx < 0 || sx >= width) continue;
                    const int weight = (dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1);
                    sum += row[sx] * weight;
                }
            }
            out[x] = uchar((sum + 8) / 16);
        }
    }
    return softened;
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
