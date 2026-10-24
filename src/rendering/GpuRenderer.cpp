#include "rendering/GpuRenderer.h"
#include <QCoreApplication>
#include <QSettings>
#include <QThread>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>

namespace compositor {
namespace {
thread_local int cpuDepth = 0;
thread_local int priorityDepth = 0;
thread_local GpuPriority scopedPriority = GpuPriority::Export;
thread_local GpuExecution threadReport;
std::atomic<int> &mode() {
    static std::atomic<int> value{std::clamp(QSettings().value("rendering/backend", 0).toInt(), 0, 2)};
    return value;
}
std::atomic<int> &budget() {
    static std::atomic<int> value{std::clamp(QSettings().value("rendering/memoryMiB", 512).toInt(), 64, 2048)};
    return value;
}
std::mutex reportMutex;
std::deque<GpuExecution> recentReports;
std::atomic<qint64> channelKeys{-1};
bool environmentBackend(RenderBackend *value) {
    const auto env = qgetenv("COMPOSITOR_RENDER_BACKEND").toLower();
    if (env == "cpu")
        *value = RenderBackend::Cpu;
    else if (env == "vulkan" || env == "gpu")
        *value = RenderBackend::Vulkan;
    else if (env == "auto")
        *value = RenderBackend::Auto;
    else
        return false;
    return true;
}
QString summary(const std::vector<GpuNode> &nodes, int output) {
    static const char *names[] = {"Source", "Copy", "Invert", "Exposure", "Lookup", "Cube", "BoxBlur", "Gaussian",
                                  "Mask", "Bloom", "MotionBlur", "ProgressiveMix", "Composite", "Transform",
                                  "GradientMap", "Morphology", "RgbSplit", "Mix", "HueSaturation", "BlackWhite",
                                  "ColorBalance", "Clear", "MorphologyStep", "MorphologyCombine", "Bokeh",
                                  "LensPoint", "LensCombine", "LensSqueeze", "LensStretch", "LensStreakBack",
                                  "LensResample", "LensPrism", "LensReduce", "Noise", "Grain"};
    static_assert(std::size(names) == gpuOperationCount);
    std::map<QString, int> counts;
    for (int i = 0; i <= output && i < int(nodes.size()); ++i)
        if (nodes[i].operation != GpuOp::Source)
            ++counts[QString::fromLatin1(names[unsigned(nodes[i].operation)])];
    QStringList parts;
    for (const auto &[name, count] : counts)
        parts << (count > 1 ? QStringLiteral("%1×%2").arg(name).arg(count) : name);
    return parts.join(QStringLiteral(", "));
}
} // namespace
RenderBackend GpuRenderer::backend() {
    if (cpuDepth)
        return RenderBackend::Cpu;
    RenderBackend value;
    if (environmentBackend(&value))
        return value;
    return RenderBackend(mode().load());
}
RenderBackend GpuRenderer::savedBackend() { return RenderBackend(mode().load()); }
bool GpuRenderer::backendOverridden() {
    RenderBackend ignored;
    return environmentBackend(&ignored);
}
void GpuRenderer::setBackend(RenderBackend value) {
    mode() = int(value);
    QSettings().setValue("rendering/backend", int(value));
    if (backend() == RenderBackend::Cpu)
        release(); // Do not keep a (possibly discrete) GPU powered for nothing.
}
int GpuRenderer::memoryBudgetMiB() {
    bool valid = false;
    const int env = qEnvironmentVariableIntValue("COMPOSITOR_GPU_MEMORY_MIB", &valid);
    return valid ? std::clamp(env, 64, 2048) : budget().load();
}
int GpuRenderer::savedMemoryBudgetMiB() { return budget().load(); }
bool GpuRenderer::memoryBudgetOverridden() {
    bool valid = false;
    qEnvironmentVariableIntValue("COMPOSITOR_GPU_MEMORY_MIB", &valid);
    return valid;
}
void GpuRenderer::setMemoryBudgetMiB(int value) {
    const int clamped = std::clamp(value, 64, 2048);
    const bool changed = budget().exchange(clamped) != clamped;
    QSettings().setValue("rendering/memoryMiB", clamped);
    if (changed)
        trim();
}
bool GpuRenderer::shouldAttempt(QSize size, double cost) {
    const auto b = backend();
    return b != RenderBackend::Cpu && !size.isEmpty() &&
           (b == RenderBackend::Vulkan || (qint64(size.width()) * size.height() >= 262144 && cost >= 8));
}
GpuRenderer::CpuScope::CpuScope() { ++cpuDepth; }
GpuRenderer::CpuScope::~CpuScope() { --cpuDepth; }
GpuRenderer::PriorityScope::PriorityScope(GpuPriority value) : previous(scopedPriority) {
    scopedPriority = value;
    ++priorityDepth;
}
GpuRenderer::PriorityScope::~PriorityScope() {
    scopedPriority = previous;
    --priorityDepth;
}
GpuPriority GpuRenderer::priority() {
    if (priorityDepth)
        return scopedPriority;
    const auto *app = QCoreApplication::instance();
    return app && QThread::currentThread() == app->thread() ? GpuPriority::Interactive : GpuPriority::Export;
}
GpuExecution GpuRenderer::lastExecution() { return threadReport; }
std::vector<GpuExecution> GpuRenderer::recentExecutions() {
    std::lock_guard lock(reportMutex);
    return {recentReports.begin(), recentReports.end()};
}
void GpuRenderer::record(const GpuExecution &report) {
    threadReport = report;
    std::lock_guard lock(reportMutex);
    recentReports.push_back(report);
    while (recentReports.size() > 20)
        recentReports.pop_front();
}
void GpuRenderer::warmUp() {
    if (backend() == RenderBackend::Cpu)
        return;
    QThreadPool::globalInstance()->start([] { warmUpVulkan(); });
}
void GpuRenderer::trim() { trimVulkan(); }
void GpuRenderer::release() { releaseVulkan(); }
QString GpuRenderer::diagnostics() {
    const auto info = vulkanDeviceInfo(backend() != RenderBackend::Cpu);
    QStringList lines;
    if (info.available) {
        lines << QStringLiteral("Vulkan: %1 (%2)").arg(info.name, info.type)
              << QStringLiteral("Driver %1, Vulkan API %2").arg(info.driver, info.apiVersion)
              << QStringLiteral("Device-local memory: %1 MiB, host-visible: %2 MiB")
                     .arg(info.deviceLocalBytes >> 20)
                     .arg(info.hostVisibleBytes >> 20);
        if (info.devices.size() > 1)
            lines << QStringLiteral("Compute devices: %1").arg(info.devices.join(QStringLiteral("; ")));
    } else
        lines << info.error;
    lines << QStringLiteral("Working-memory budget: %1 MiB").arg(memoryBudgetMiB());
    const auto recent = recentExecutions();
    if (!recent.empty()) {
        static const char *priorities[] = {"export", "canvas", "interactive"};
        lines << QString() << QStringLiteral("Recent graphs (newest first):");
        for (auto it = recent.rbegin(); it != recent.rend(); ++it)
            lines << QStringLiteral("• %1 — %2, %3 ms, %4 KiB up, %5 KiB down, %6%7")
                         .arg(it->accelerated ? QStringLiteral("GPU") : QStringLiteral("CPU"),
                              it->operations.isEmpty() ? QStringLiteral("(no passes)") : it->operations)
                         .arg(it->milliseconds, 0, 'f', 1)
                         .arg(it->uploadedBytes >> 10)
                         .arg(it->downloadedBytes >> 10)
                         .arg(QLatin1String(priorities[int(it->priority)]),
                              it->reason.isEmpty() ? QString() : QStringLiteral(" — ") + it->reason);
    }
    return lines.join('\n');
}
int GpuGraph::source(const QImage &image) {
    for (int i = 0; i < int(nodes_.size()); ++i)
        if (nodes_[i].operation == GpuOp::Source && !nodes_[i].channel && nodes_[i].key == image.cacheKey())
            return i;
    GpuNode node;
    node.image = image;
    node.key = image.cacheKey();
    node.size = image.size();
    nodes_.push_back(node);
    return int(nodes_.size()) - 1;
}
int GpuGraph::channel(std::shared_ptr<const std::vector<float>> values, QSize size) {
    if (!values || size.isEmpty() || values->size() != size_t(size.width()) * size.height())
        return -1;
    // A stable identity per vector so repeated graphs over the same buffer reuse its upload.
    static std::mutex mutex;
    static std::map<const void *, std::pair<std::weak_ptr<const std::vector<float>>, qint64>> keys;
    qint64 key;
    {
        std::lock_guard lock(mutex);
        auto &entry = keys[values.get()];
        if (entry.first.lock() != values)
            entry = {values, channelKeys.fetch_sub(1)};
        key = entry.second;
        for (auto it = keys.begin(); it != keys.end();)
            it = it->second.first.expired() ? keys.erase(it) : std::next(it);
    }
    GpuNode node;
    node.channel = std::move(values);
    node.key = key;
    node.size = size;
    nodes_.push_back(node);
    return int(nodes_.size()) - 1;
}
void GpuGraph::transient(int source) {
    if (source >= 0 && source < int(nodes_.size()) && nodes_[source].operation == GpuOp::Source)
        nodes_[source].transient = true;
}
int GpuGraph::clear(QSize size, unsigned quantization) {
    if (size.isEmpty())
        return -1;
    GpuNode node;
    node.operation = GpuOp::Clear;
    node.size = size;
    node.quantization = quantization;
    nodes_.push_back(node);
    return int(nodes_.size()) - 1;
}
int GpuGraph::pass(GpuOp op, int input, int auxiliary, const GpuParameters &parameters, unsigned quantization,
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
int GpuGraph::update(GpuOp op, int input, int auxiliary, const GpuParameters &parameters, QRect area) {
    if (input < 0 || input >= int(nodes_.size()))
        return -1;
    area &= QRect(QPoint(), nodes_[input].size);
    if (area.isEmpty())
        return input;
    const int node = pass(op, input, auxiliary, parameters);
    if (node >= 0) {
        nodes_[node].inPlace = true;
        nodes_[node].region = area;
    }
    return node;
}
int GpuGraph::boxBlur(int input, int reach, int axis, unsigned quantization) {
    if (input < 0 || reach <= 0)
        return input;
    GpuParameters p{};
    p[GpuParam::BoxBlur::radius] = reach;
    p[GpuParam::BoxBlur::axis] = axis;
    // Each invocation slides over one span: long enough to amortize the window, short
    // enough to keep the device busy on narrow images.
    p[GpuParam::BoxBlur::span] = std::clamp(2 * reach + 1, 16, 128);
    return pass(GpuOp::BoxBlur, input, -1, p, quantization);
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
            GpuParameters p{};
            p[GpuParam::Gaussian::reach] = reach;
            p[GpuParam::Gaussian::axis] = axis;
            input = pass(GpuOp::Gaussian, input, lut, p);
        }
    } else {
        int lower = int(std::floor(std::sqrt(4 * radius * radius + 1)));
        if (!(lower & 1))
            --lower;
        const int count = qRound((12 * radius * radius - 3 * lower * lower - 12 * lower - 9) / (-4. * lower - 4));
        for (int iteration = 0; iteration < 3; ++iteration)
            for (int axis = 0; axis < 2; ++axis)
                input = boxBlur(input, std::max(0, ((iteration < count ? lower : lower + 2) - 1) / 2), axis,
                                bytes ? unsigned(GpuQuantization::Unorm8) : 0);
    }
    return input;
}
int GpuGraph::morphology(int input, int radius, bool erode) {
    for (int axis = 0; axis < 2; ++axis)
        input = morphologyAxis(input, radius, axis, erode);
    return input;
}
int GpuGraph::morphologyAxis(int input, int radius, int axis, bool erode) {
    if (input < 0 || radius <= 0)
        return input;
    const QSize size = nodes_[input].size;
    if (radius <= 6) {
        GpuParameters p{};
        p[GpuParam::Morphology::radius] = radius;
        p[GpuParam::Morphology::axis] = axis;
        p[GpuParam::Morphology::erode] = erode ? 1 : 0;
        return pass(GpuOp::Morphology, input, -1, p);
    }
    // Sparse table: each pass multiplies the run by up to 8 (fewer full-image passes); runs never exceed r + 1,
    // so the combine's lookups stay inside the window even where the line clips it.
    int run = 1;
    while (run * 2 <= radius + 1) {
        const int count = std::min(8, (radius + 1) / run);
        GpuParameters p{};
        p[GpuParam::MorphologyStep::step] = run;
        p[GpuParam::MorphologyStep::axis] = axis;
        p[GpuParam::MorphologyStep::erode] = erode ? 1 : 0;
        p[GpuParam::MorphologyStep::count] = count;
        input = pass(GpuOp::MorphologyStep, input, -1, p);
        run *= count;
    }
    GpuParameters c{};
    c[GpuParam::MorphologyCombine::radius] = radius;
    c[GpuParam::MorphologyCombine::axis] = axis;
    c[GpuParam::MorphologyCombine::erode] = erode ? 1 : 0;
    c[GpuParam::MorphologyCombine::run] = run;
    return pass(GpuOp::MorphologyCombine, input, -1, c, 0, size);
}
QImage GpuGraph::execute(int output, QImage::Format format, RenderBackend requested, GpuExecution *report) const {
    GpuExecution r;
    r.priority = GpuRenderer::priority();
    QImage result;
    if (output < 0 || output >= int(nodes_.size()))
        r.reason = QStringLiteral("Invalid GPU graph output");
    else if (nodes_[output].operation == GpuOp::Source && !nodes_[output].channel)
        result = nodes_[output].image.convertToFormat(format);
    else if (requested == RenderBackend::Cpu ||
             (requested == RenderBackend::Auto && GpuRenderer::backend() == RenderBackend::Cpu))
        r.reason = QStringLiteral("CPU rendering selected");
    else {
        r.operations = summary(nodes_, output);
        result = executeVulkanGraph(*this, output, format, r);
    }
    GpuRenderer::record(r);
    if (report)
        *report = r;
    return result;
}
std::vector<float> GpuGraph::executeChannel(int output, RenderBackend requested, GpuExecution *report) const {
    GpuExecution r;
    r.priority = GpuRenderer::priority();
    std::vector<float> result;
    if (output < 0 || output >= int(nodes_.size()))
        r.reason = QStringLiteral("Invalid GPU graph output");
    else if (requested == RenderBackend::Cpu ||
             (requested == RenderBackend::Auto && GpuRenderer::backend() == RenderBackend::Cpu))
        r.reason = QStringLiteral("CPU rendering selected");
    else {
        r.operations = summary(nodes_, output);
        result = executeVulkanChannel(*this, output, r);
    }
    GpuRenderer::record(r);
    if (report)
        *report = r;
    return result;
}
} // namespace compositor
