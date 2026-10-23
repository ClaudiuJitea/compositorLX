#include "rendering/GpuRenderer.h"
#include <QSettings>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace compositor {
namespace {
thread_local int cpuDepth = 0;
std::atomic<int> &mode() {
    static std::atomic<int> value{std::clamp(QSettings().value("rendering/backend", 0).toInt(), 0, 2)};
    return value;
}
std::atomic<int> &budget() {
    static std::atomic<int> value{std::clamp(QSettings().value("rendering/memoryMiB", 512).toInt(), 64, 2048)};
    return value;
}
std::mutex reportMutex;
GpuExecution lastReport;
} // namespace
RenderBackend GpuRenderer::backend() {
    if (cpuDepth)
        return RenderBackend::Cpu;
    const auto env = qgetenv("COMPOSITOR_RENDER_BACKEND").toLower();
    if (env == "cpu")
        return RenderBackend::Cpu;
    if (env == "vulkan" || env == "gpu")
        return RenderBackend::Vulkan;
    if (env == "auto")
        return RenderBackend::Auto;
    return RenderBackend(mode().load());
}
void GpuRenderer::setBackend(RenderBackend value) {
    mode() = int(value);
    QSettings().setValue("rendering/backend", int(value));
}
int GpuRenderer::memoryBudgetMiB() {
    bool valid = false;
    const int env = qEnvironmentVariableIntValue("COMPOSITOR_GPU_MEMORY_MIB", &valid);
    return valid ? std::clamp(env, 64, 2048) : budget().load();
}
void GpuRenderer::setMemoryBudgetMiB(int value) {
    budget() = std::clamp(value, 64, 2048);
    QSettings().setValue("rendering/memoryMiB", budget().load());
}
bool GpuRenderer::shouldAttempt(QSize size, double cost) {
    const auto b = backend();
    return b != RenderBackend::Cpu && !size.isEmpty() &&
           (b == RenderBackend::Vulkan || (qint64(size.width()) * size.height() >= 262144 && cost >= 8));
}
GpuRenderer::CpuScope::CpuScope() { ++cpuDepth; }
GpuRenderer::CpuScope::~CpuScope() { --cpuDepth; }
GpuExecution GpuRenderer::lastExecution() {
    std::lock_guard lock(reportMutex);
    return lastReport;
}
QString GpuRenderer::diagnostics() {
    const auto r = lastExecution();
    return QStringLiteral("%1\nWorking-memory budget: %2 MiB\nLast graph: %3 dispatches, %4 bytes uploaded, %5 bytes "
                          "read back\n%6")
        .arg(vulkanDeviceDescription())
        .arg(memoryBudgetMiB())
        .arg(r.dispatches)
        .arg(r.uploadedBytes)
        .arg(r.downloadedBytes)
        .arg(r.reason);
}
int GpuGraph::source(const QImage &image) {
    for (int i = 0; i < int(nodes_.size()); ++i)
        if (nodes_[i].operation == GpuOp::Source && nodes_[i].image.cacheKey() == image.cacheKey())
            return i;
    GpuNode node;
    node.image = image;
    node.size = image.size();
    nodes_.push_back(node);
    return int(nodes_.size()) - 1;
}
int GpuGraph::pass(GpuOp op, int input, int auxiliary, const std::array<float, 28> &parameters, unsigned quantization,
                   QSize size) {
    if (input < 0 || input >= int(nodes_.size()) || auxiliary >= int(nodes_.size()))
        return -1;
    GpuNode node;
    node.operation = op;
    node.input = input;
    node.auxiliary = auxiliary;
    node.size = size.isEmpty() ? nodes_[input].size : size;
    node.parameters = parameters;
    node.quantization = quantization;
    nodes_.push_back(node);
    return int(nodes_.size()) - 1;
}
int GpuGraph::gaussian(int input, double radius, bool bytes) {
    if (input < 0 || !std::isfinite(radius))
        return -1;
    if (radius <= 0)
        return input;
    radius = std::clamp(radius, .1, 250.);
    if (!bytes && radius <= 10) {
        const int reach = std::max(1, int(std::ceil(radius * 3)));
        QImage table(reach * 2 + 1, 1, QImage::Format_RGBA32FPx4_Premultiplied);
        auto *weights = reinterpret_cast<float *>(table.bits());
        double total = 0;
        for (int i = -reach; i <= reach; ++i)
            total += std::exp(-.5 * i * i / (radius * radius));
        for (int i = -reach; i <= reach; ++i)
            for (int c = 0; c < 4; ++c)
                weights[(i + reach) * 4 + c] = float(std::exp(-.5 * i * i / (radius * radius)) / total);
        const int lut = source(table);
        for (int axis = 0; axis < 2; ++axis) {
            std::array<float, 28> p{};
            p[0] = reach;
            p[1] = axis;
            input = pass(GpuOp::Gaussian, input, lut, p);
        }
    } else {
        int lower = int(std::floor(std::sqrt(4 * radius * radius + 1)));
        if (!(lower & 1))
            --lower;
        const int count = qRound((12 * radius * radius - 3 * lower * lower - 12 * lower - 9) / (-4. * lower - 4));
        for (int iteration = 0; iteration < 3; ++iteration)
            for (int axis = 0; axis < 2; ++axis) {
                const int reach = std::max(0, ((iteration < count ? lower : lower + 2) - 1) / 2);
                if (!reach)
                    continue;
                std::array<float, 28> p{};
                p[0] = reach;
                p[1] = axis;
                input = pass(GpuOp::BoxBlur, input, -1, p, bytes ? 1 : 0);
            }
    }
    return input;
}
QImage GpuGraph::execute(int output, QImage::Format format, RenderBackend requested, GpuExecution *report) const {
    GpuExecution r;
    QImage result;
    if (output < 0 || output >= int(nodes_.size()))
        r.reason = QStringLiteral("Invalid GPU graph output");
    else if (nodes_[output].operation == GpuOp::Source)
        result = nodes_[output].image.convertToFormat(format);
    else if (requested == RenderBackend::Cpu ||
             (requested == RenderBackend::Auto && GpuRenderer::backend() == RenderBackend::Cpu))
        r.reason = QStringLiteral("CPU rendering selected");
    else
        result = executeVulkanGraph(*this, output, format, r);
    {
        std::lock_guard lock(reportMutex);
        lastReport = r;
    }
    if (report)
        *report = r;
    return result;
}
} // namespace compositor
