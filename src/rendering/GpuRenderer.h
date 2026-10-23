#pragma once
#include <QImage>
#include <QString>
#include <array>
#include <vector>

namespace compositor {
enum class RenderBackend { Auto, Cpu, Vulkan };
struct GpuExecution {
    bool accelerated = false;
    QString device, reason;
    quint64 uploadedBytes = 0, downloadedBytes = 0, workingBytes = 0;
    int dispatches = 0;
};
// Source nodes own implicitly shared immutable snapshots. Passes refer only to earlier
// nodes; execution retains intermediate surfaces on the device and reads the result once.
enum class GpuOp : unsigned {
    Source,
    Copy,
    Invert,
    Exposure,
    Lookup,
    Cube,
    BoxBlur,
    Gaussian,
    Mask,
    Bloom,
    MotionBlur,
    ProgressiveMix,
    Composite,
    Transform,
    GradientMap,
    Morphology,
    RgbSplit,
    Mix,
    HueSaturation,
    BlackWhite,
    ColorBalance
};
struct GpuNode {
    GpuOp operation = GpuOp::Source;
    QImage image;
    QSize size;
    int input = -1, auxiliary = -1;
    unsigned quantization = 0; // 0=float, 1=UNORM8, 2=UNORM16, 3=half
    std::array<float, 28> parameters{};
};
class GpuGraph {
  public:
    int source(const QImage &image);
    int pass(GpuOp op, int input, int auxiliary = -1, const std::array<float, 28> &parameters = {},
             unsigned quantization = 0, QSize size = {});
    int gaussian(int input, double radius, bool byteAlgorithm = false);
    QImage execute(int output, QImage::Format format, RenderBackend backend = RenderBackend::Auto,
                   GpuExecution *report = nullptr) const;
    const std::vector<GpuNode> &nodes() const { return nodes_; }

  private:
    std::vector<GpuNode> nodes_;
};
class GpuRenderer {
  public:
    static RenderBackend backend();
    static void setBackend(RenderBackend mode);
    static bool shouldAttempt(QSize size, double cost = 1);
    static int memoryBudgetMiB();
    static void setMemoryBudgetMiB(int value);
    static QString diagnostics();
    static GpuExecution lastExecution();
    // Thread-local suppression for CPU references and CPU-only work inside a graph builder.
    class CpuScope {
      public:
        CpuScope();
        ~CpuScope();
        CpuScope(const CpuScope &) = delete;
        CpuScope &operator=(const CpuScope &) = delete;
    };
};
QImage executeVulkanGraph(const GpuGraph &, int output, QImage::Format, GpuExecution &);
QString vulkanDeviceDescription();
} // namespace compositor
