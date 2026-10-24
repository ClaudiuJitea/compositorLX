#pragma once
#include "rendering/GpuContract.h"
#include <QImage>
#include <QString>
#include <QStringList>
#include <array>
#include <memory>
#include <vector>

namespace compositor {
enum class RenderBackend { Auto, Cpu, Vulkan };
// Who is waiting for the GPU. Higher priorities are served first; GUI-thread work never
// waits for the queue and falls back to the CPU when the GPU is busy.
enum class GpuPriority { Export, Canvas, Interactive };
struct GpuExecution {
    bool accelerated = false;
    QString device, reason, operations;
    quint64 uploadedBytes = 0, downloadedBytes = 0, workingBytes = 0;
    int dispatches = 0;
    double milliseconds = 0;
    GpuPriority priority = GpuPriority::Export;
};
using GpuParameters = std::array<float, gpuParameterSlots>;
struct GpuNode {
    GpuOp operation = GpuOp::Source;
    QImage image;
    // Single-channel float source (GpuFormat::Red32), e.g. layer-effect coverage.
    std::shared_ptr<const std::vector<float>> channel;
    qint64 key = 0; // Upload-cache identity of a source.
    QSize size;
    int input = -1, auxiliary = -1;
    unsigned quantization = 0; // GpuQuantization; also selects compact storage for the result.
    GpuParameters parameters{};
    // GpuGraph::update: the pass writes into its input's storage, over `region` only.
    bool inPlace = false;
    // A source used only by this graph: once read, its memory may hold later passes (it is uploaded again next time).
    bool transient = false;
    QRect region;
};
class GpuGraph {
  public:
    int source(const QImage &image);
    int channel(std::shared_ptr<const std::vector<float>> values, QSize size);
    // Marks a source as used only by this graph (GpuNode::transient), so its memory is not held for the next one.
    void transient(int source);
    // A transparent surface, stored as `quantization` asks (GpuQuantization; float when 0).
    int clear(QSize size, unsigned quantization = 0);
    int pass(GpuOp op, int input, int auxiliary = -1, const GpuParameters &parameters = {}, unsigned quantization = 0,
             QSize size = {});
    // A Lens pass that updates `input` in place over `area` only, the rest left as it is. `input` must be a pass of
    // the same size that nothing after this reads.
    int update(GpuOp op, int input, int auxiliary, const GpuParameters &parameters, QRect area);
    int boxBlur(int input, int reach, int axis, unsigned quantization = 0);
    int gaussian(int input, double radius, bool byteAlgorithm = false);
    // Separable min (erode) or max over a square of side 2r+1; erosion treats the outside as empty.
    int morphology(int input, int radius, bool erode);
    // The same along one axis only (0 rows, 1 columns).
    int morphologyAxis(int input, int radius, int axis, bool erode);
    QImage execute(int output, QImage::Format format, RenderBackend backend = RenderBackend::Auto,
                   GpuExecution *report = nullptr) const;
    std::vector<float> executeChannel(int output, RenderBackend backend = RenderBackend::Auto,
                                      GpuExecution *report = nullptr) const;
    const std::vector<GpuNode> &nodes() const { return nodes_; }

  private:
    std::vector<GpuNode> nodes_;
};
class GpuRenderer {
  public:
    // The effective backend: CpuScope, then COMPOSITOR_RENDER_BACKEND, then the saved preference.
    static RenderBackend backend();
    static RenderBackend savedBackend();
    static void setBackend(RenderBackend mode);
    static bool backendOverridden();
    static bool shouldAttempt(QSize size, double cost = 1);
    static int memoryBudgetMiB();
    static int savedMemoryBudgetMiB();
    static void setMemoryBudgetMiB(int value);
    static bool memoryBudgetOverridden();
    static quint64 memoryBudgetBytes() { return quint64(memoryBudgetMiB()) * 1024 * 1024; }
    static QString diagnostics();
    // The calling thread's most recent graph.
    static GpuExecution lastExecution();
    // The last 20 graphs from any thread, newest last.
    static std::vector<GpuExecution> recentExecutions();
    static void record(const GpuExecution &);
    // Compile every pipeline on a worker so the first filter does not pay for it.
    static void warmUp();
    // Free pooled device memory now (it is also trimmed after the GPU has been idle).
    static void trim();
    // Destroy the Vulkan device, e.g. when CPU rendering is chosen; it is recreated on demand.
    static void release();
    // Thread-local suppression for CPU references and CPU-only work inside a graph builder.
    class CpuScope {
      public:
        CpuScope();
        ~CpuScope();
        CpuScope(const CpuScope &) = delete;
        CpuScope &operator=(const CpuScope &) = delete;
    };
    // Thread-local scheduling priority for graphs executed in this scope.
    class PriorityScope {
      public:
        explicit PriorityScope(GpuPriority);
        ~PriorityScope();
        PriorityScope(const PriorityScope &) = delete;
        PriorityScope &operator=(const PriorityScope &) = delete;

      private:
        GpuPriority previous;
    };
    static GpuPriority priority();
};
struct GpuDeviceInfo {
    bool available = false;
    QString name, type, driver, apiVersion, error;
    quint64 deviceLocalBytes = 0, hostVisibleBytes = 0;
    QStringList devices; // Every hardware compute device, for the device picker.
};
QImage executeVulkanGraph(const GpuGraph &, int output, QImage::Format, GpuExecution &);
std::vector<float> executeVulkanChannel(const GpuGraph &, int output, GpuExecution &);
QString vulkanDeviceDescription();
GpuDeviceInfo vulkanDeviceInfo(bool open);
void warmUpVulkan();
void trimVulkan();
void releaseVulkan();
} // namespace compositor
