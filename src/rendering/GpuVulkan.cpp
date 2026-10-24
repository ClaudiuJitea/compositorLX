#include "rendering/DeepImage.h"
#include "rendering/GpuRenderer.h"

#ifdef COMPOSITOR_HAVE_VULKAN
#define VK_NO_PROTOTYPES
#include "rendering/shaders/RenderingSpirv.h"
#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLibrary>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>

namespace compositor {
namespace {
#define INSTANCE_FUNCTIONS(X)                                                                                          \
    X(DestroyInstance)                                                                                                 \
    X(EnumeratePhysicalDevices)                                                                                        \
    X(GetPhysicalDeviceProperties)                                                                                     \
    X(GetPhysicalDeviceMemoryProperties)                                                                               \
    X(GetPhysicalDeviceQueueFamilyProperties)                                                                          \
    X(CreateDevice)                                                                                                    \
    X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X)                                                                                            \
    X(DestroyDevice)                                                                                                   \
    X(DeviceWaitIdle)                                                                                                  \
    X(GetDeviceQueue)                                                                                                  \
    X(CreateShaderModule)                                                                                              \
    X(DestroyShaderModule)                                                                                             \
    X(CreateDescriptorSetLayout)                                                                                       \
    X(DestroyDescriptorSetLayout)                                                                                      \
    X(CreatePipelineLayout)                                                                                            \
    X(DestroyPipelineLayout)                                                                                           \
    X(CreatePipelineCache)                                                                                             \
    X(DestroyPipelineCache)                                                                                            \
    X(GetPipelineCacheData)                                                                                            \
    X(CreateComputePipelines)                                                                                          \
    X(DestroyPipeline)                                                                                                 \
    X(CreateDescriptorPool)                                                                                            \
    X(DestroyDescriptorPool)                                                                                           \
    X(AllocateDescriptorSets)                                                                                          \
    X(UpdateDescriptorSets)                                                                                            \
    X(CreateCommandPool)                                                                                               \
    X(DestroyCommandPool)                                                                                              \
    X(AllocateCommandBuffers)                                                                                          \
    X(ResetCommandPool)                                                                                                \
    X(BeginCommandBuffer)                                                                                              \
    X(EndCommandBuffer)                                                                                                \
    X(CmdBindPipeline)                                                                                                 \
    X(CmdBindDescriptorSets)                                                                                           \
    X(CmdPushConstants)                                                                                                \
    X(CmdDispatch)                                                                                                     \
    X(CmdPipelineBarrier)                                                                                              \
    X(CmdCopyBuffer)                                                                                                   \
    X(CreateBuffer)                                                                                                    \
    X(DestroyBuffer)                                                                                                   \
    X(GetBufferMemoryRequirements)                                                                                     \
    X(AllocateMemory)                                                                                                  \
    X(FreeMemory)                                                                                                      \
    X(BindBufferMemory)                                                                                                \
    X(MapMemory)                                                                                                       \
    X(UnmapMemory)                                                                                                     \
    X(FlushMappedMemoryRanges)                                                                                         \
    X(InvalidateMappedMemoryRanges)                                                                                    \
    X(CreateFence)                                                                                                     \
    X(DestroyFence)                                                                                                    \
    X(ResetFences)                                                                                                     \
    X(QueueSubmit)                                                                                                     \
    X(WaitForFences)

struct Parameters {
    uint32_t width, height, operation, flags;
    GpuParameters values;
};
static_assert(sizeof(Parameters) == 128, "Vulkan guarantees 128 bytes of push constants");

constexpr std::array<int, 6> bytesPerPixel{16, 4, 8, 8, 1, 4}; // Indexed by GpuFormat.
VkDeviceSize storageBytes(GpuFormat format, QSize size) {
    const VkDeviceSize pixels = VkDeviceSize(size.width()) * size.height();
    return format == GpuFormat::Gray8 ? (pixels + 3) / 4 * 4 : pixels * bytesPerPixel[unsigned(format)];
}
// Storage that reads back directly into a QImage of the requested format.
GpuFormat outputFormatFor(QImage::Format format) {
    switch (format) {
    case QImage::Format_RGBA8888_Premultiplied:
        return GpuFormat::Unorm8;
    case QImage::Format_RGBA64_Premultiplied:
        return GpuFormat::Unorm16;
    case QImage::Format_RGBA16FPx4_Premultiplied:
        return GpuFormat::Half;
    default:
        return GpuFormat::Float32;
    }
}
QImage::Format imageFormatFor(GpuFormat format) {
    switch (format) {
    case GpuFormat::Unorm8:
        return QImage::Format_RGBA8888_Premultiplied;
    case GpuFormat::Unorm16:
        return QImage::Format_RGBA64_Premultiplied;
    case GpuFormat::Half:
        return QImage::Format_RGBA16FPx4_Premultiplied;
    case GpuFormat::Gray8:
        return QImage::Format_Grayscale8;
    default:
        return QImage::Format_RGBA32FPx4_Premultiplied;
    }
}
// A source in a format the shader decodes directly. Conversions happen before the
// device lock is taken, so slow CPU work never blocks other GPU users.
struct Prepared {
    QImage image;
    GpuFormat format = GpuFormat::Float32;
    std::shared_ptr<const std::vector<float>> channel;
};
Prepared prepare(const GpuNode &node) {
    Prepared p;
    if (node.channel) {
        p.channel = node.channel;
        p.format = GpuFormat::Red32;
        return p;
    }
    const QImage &image = node.image;
    switch (image.format()) {
    case QImage::Format_RGBA8888_Premultiplied:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_RGBA16FPx4_Premultiplied:
    case QImage::Format_RGBA32FPx4_Premultiplied:
    case QImage::Format_Grayscale8:
        p.image = image;
        break;
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_ARGB32:
    case QImage::Format_RGB32:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGB888:
        p.image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        break;
    case QImage::Format_RGBA64:
    case QImage::Format_RGBX64:
    case QImage::Format_Grayscale16:
        p.image = image.convertToFormat(QImage::Format_RGBA64_Premultiplied);
        break;
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBX16FPx4:
        p.image = image.convertToFormat(QImage::Format_RGBA16FPx4_Premultiplied);
        break;
    default:
        p.image = DeepImage::floating(image);
        break;
    }
    switch (p.image.format()) {
    case QImage::Format_RGBA8888_Premultiplied:
        p.format = GpuFormat::Unorm8;
        break;
    case QImage::Format_RGBA64_Premultiplied:
        p.format = GpuFormat::Unorm16;
        break;
    case QImage::Format_RGBA16FPx4_Premultiplied:
        p.format = GpuFormat::Half;
        break;
    case QImage::Format_Grayscale8:
        p.format = GpuFormat::Gray8;
        break;
    default:
        p.format = GpuFormat::Float32;
        break;
    }
    return p;
}
bool singleChannelOperation(GpuOp op) {
    return op == GpuOp::Copy || op == GpuOp::BoxBlur || op == GpuOp::Gaussian || op == GpuOp::Morphology ||
           op == GpuOp::MorphologyStep || op == GpuOp::MorphologyCombine;
}
GpuFormat passFormat(const GpuNode &node, GpuFormat input) {
    switch (GpuQuantization(node.quantization)) {
    case GpuQuantization::Unorm8:
        return GpuFormat::Unorm8;
    case GpuQuantization::Unorm16:
        return GpuFormat::Unorm16;
    case GpuQuantization::Half:
        return GpuFormat::Half;
    default:
        // The Lens operations make one channel whatever they read (LensResample can pick it out of an RGBA source).
        if (node.operation >= GpuOp::LensPoint)
            return GpuFormat::Red32;
        return input == GpuFormat::Red32 && singleChannelOperation(node.operation) ? GpuFormat::Red32
                                                                                   : GpuFormat::Float32;
    }
}

// Waiters are served highest priority first. A timeout of zero is a try-lock.
class PriorityLock {
  public:
    bool acquire(GpuPriority priority, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        const int level = int(priority);
        ++waiting[level];
        const auto ready = [&] {
            if (busy)
                return false;
            for (int above = level + 1; above < int(waiting.size()); ++above)
                if (waiting[above])
                    return false;
            return true;
        };
        const bool acquired = timeout.count() < 0 ? (changed.wait(lock, ready), true)
                                                  : changed.wait_for(lock, timeout, ready);
        --waiting[level];
        if (acquired)
            busy = true;
        else
            changed.notify_all();
        return acquired;
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            busy = false;
        }
        changed.notify_all();
    }

  private:
    std::mutex mutex;
    std::condition_variable changed;
    bool busy = false;
    std::array<int, 3> waiting{};
};
struct Held {
    PriorityLock &lock;
    ~Held() { lock.release(); }
};

std::chrono::milliseconds idleTrimDelay() {
    bool valid = false;
    const int ms = qEnvironmentVariableIntValue("COMPOSITOR_GPU_IDLE_TRIM_MS", &valid);
    return std::chrono::milliseconds(valid && ms > 0 ? ms : 30000);
}

// One compute queue shared by previews, canvas workers and exports. Vulkan objects never
// depend on the Qt window system, so the same path works in headless tests.
class Context {
  public:
    using Consumer = std::function<void(const unsigned char *, GpuFormat)>;
    Context() {
        try {
            initialize();
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
        if (device)
            janitor = std::thread([this] { trimWhenIdle(); });
    }
    ~Context() {
        {
            std::lock_guard lock(janitorMutex);
            stopping = true;
        }
        janitorWake.notify_all();
        if (janitor.joinable())
            janitor.join();
        if (device && DeviceWaitIdle)
            DeviceWaitIdle(device);
        savePipelineCache();
        freeBuffers();
        if (device && DestroyDevice) {
            if (fence)
                DestroyFence(device, fence, nullptr);
            if (commandPool)
                DestroyCommandPool(device, commandPool, nullptr);
            if (descriptorPool)
                DestroyDescriptorPool(device, descriptorPool, nullptr);
            for (VkPipeline pipeline : pipelines)
                if (pipeline)
                    DestroyPipeline(device, pipeline, nullptr);
            if (pipelineCache)
                DestroyPipelineCache(device, pipelineCache, nullptr);
            if (pipelineLayout)
                DestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptorLayout)
                DestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
            if (shader)
                DestroyShaderModule(device, shader, nullptr);
            DestroyDevice(device, nullptr);
        }
        if (instance && DestroyInstance)
            DestroyInstance(instance, nullptr);
    }
    bool render(const GpuGraph &, int output, GpuFormat outputFormat, const Consumer &, GpuExecution &);
    bool deviceLost() const { return lost; }
    GpuDeviceInfo info() {
        GpuDeviceInfo i;
        i.devices = deviceNames;
        if (!device) {
            i.error = error;
            return i;
        }
        i.available = error.isEmpty();
        i.error = error;
        i.name = QString::fromUtf8(properties.deviceName);
        i.type = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? QStringLiteral("discrete GPU")
                 : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? QStringLiteral("integrated GPU")
                                                                                   : QStringLiteral("software Vulkan");
        i.apiVersion = QStringLiteral("%1.%2.%3")
                           .arg(VK_VERSION_MAJOR(properties.apiVersion))
                           .arg(VK_VERSION_MINOR(properties.apiVersion))
                           .arg(VK_VERSION_PATCH(properties.apiVersion));
        const uint32_t d = properties.driverVersion;
        i.driver = properties.vendorID == 0x10de
                       ? QStringLiteral("%1.%2").arg(d >> 22).arg((d >> 14) & 0xff)
                       : QStringLiteral("%1.%2.%3").arg(VK_VERSION_MAJOR(d)).arg(VK_VERSION_MINOR(d)).arg(VK_VERSION_PATCH(d));
        for (uint32_t h = 0; h < memoryProperties.memoryHeapCount; ++h)
            if (memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                i.deviceLocalBytes += memoryProperties.memoryHeaps[h].size;
        for (uint32_t h = 0; h < memoryProperties.memoryHeapCount; ++h)
            for (uint32_t t = 0; t < memoryProperties.memoryTypeCount; ++t)
                if (memoryProperties.memoryTypes[t].heapIndex == h &&
                    (memoryProperties.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                    i.hostVisibleBytes += memoryProperties.memoryHeaps[h].size;
                    break;
                }
        return i;
    }
    void warmUp() {
        if (!error.isEmpty() || !lock.acquire(GpuPriority::Export, std::chrono::milliseconds(-1)))
            return;
        Held held{lock};
        try {
            for (unsigned op = 1; op < gpuOperationCount; ++op)
                pipelineFor(GpuOp(op));
            savePipelineCache();
        } catch (const std::exception &) {
        }
    }
    // Free pooled buffers unless the queue is in use.
    void trim() {
        if (!lock.acquire(GpuPriority::Export, std::chrono::milliseconds(0)))
            return;
        Held held{lock};
        freeBuffers();
    }

  private:
#define DECLARE(name) PFN_vk##name name = nullptr;
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE
    QLibrary loader{QStringLiteral("vulkan"), 1};
    PFN_vkGetInstanceProcAddr getInstanceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    uint32_t queueFamily = 0;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::array<VkPipeline, gpuOperationCount> pipelines{};
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2048> descriptorSets{};
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    QString error, cachePath;
    QStringList deviceNames;
    bool lost = false;
    PriorityLock lock;
    // Idle trimming.
    std::thread janitor;
    std::mutex janitorMutex;
    std::condition_variable janitorWake;
    bool stopping = false;
    std::chrono::steady_clock::time_point lastUse = std::chrono::steady_clock::now();

    void check(VkResult result, const char *operation) {
        if (result == VK_SUCCESS)
            return;
        const QString message = QStringLiteral("Vulkan %1 failed (%2)").arg(QLatin1String(operation)).arg(int(result));
        if (result == VK_ERROR_DEVICE_LOST) {
            error = message; // Do not retry a lost device on every preview.
            lost = true;
        }
        throw std::runtime_error(message.toStdString());
    }
    void initialize();
    VkPipeline pipelineFor(GpuOp op);
    void loadPipelineCache();
    void savePipelineCache();
    void trimWhenIdle() {
        std::unique_lock guard(janitorMutex);
        while (!stopping) {
            janitorWake.wait_for(guard, idleTrimDelay());
            if (stopping)
                break;
            if (std::chrono::steady_clock::now() - lastUse < idleTrimDelay())
                continue;
            guard.unlock();
            trim();
            guard.lock();
        }
    }
    void touch() {
        std::lock_guard guard(janitorMutex);
        lastUse = std::chrono::steady_clock::now();
    }
    void barrier(VkPipelineStageFlags from, VkAccessFlags writes, VkPipelineStageFlags to, VkAccessFlags reads) {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = writes;
        b.dstAccessMask = reads;
        CmdPipelineBarrier(command, from, to, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
    struct Buffer {
        Context &context;
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkMemoryPropertyFlags flags = 0;
        void *mapped = nullptr;
        explicit Buffer(Context &c) : context(c) {}
        Buffer(const Buffer &) = delete;
        Buffer &operator=(const Buffer &) = delete;
        ~Buffer() {
            if (mapped)
                context.UnmapMemory(context.device, memory);
            if (handle)
                context.DestroyBuffer(context.device, handle, nullptr);
            if (memory)
                context.FreeMemory(context.device, memory, nullptr);
        }
        void allocate(VkDeviceSize bytes, bool host) {
            VkBufferCreateInfo b{};
            b.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            b.size = bytes;
            b.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            b.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            context.check(context.CreateBuffer(context.device, &b, nullptr, &handle), "create buffer");
            VkMemoryRequirements requirements{};
            context.GetBufferMemoryRequirements(context.device, handle, &requirements);
            const bool discrete = context.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            int chosen = -1, score = -1000;
            for (uint32_t i = 0; i < context.memoryProperties.memoryTypeCount; ++i) {
                if (!(requirements.memoryTypeBits & (1u << i)))
                    continue;
                const auto &type = context.memoryProperties.memoryTypes[i];
                const auto required = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                if (!(type.propertyFlags & required))
                    continue;
                const auto &heap = context.memoryProperties.memoryHeaps[type.heapIndex];
                if (requirements.size > heap.size / 4)
                    continue;
                const bool local = type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                const bool cached = type.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
                const bool coherent = type.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                int rank = 0;
                if (!host)
                    rank = (type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? 0 : 1;
                else if (discrete)
                    // Read back from cached system memory; mapped VRAM (ReBAR) is uncached for the CPU.
                    rank = (cached ? 8 : 0) + (coherent ? 2 : 0) - (local ? 16 : 0);
                else
                    // Unified memory: a device-local host-visible heap avoids the staging copy.
                    rank = (local ? 4 : 0) + (coherent ? 2 : 0) + (cached ? 1 : 0);
                if (rank > score) {
                    chosen = int(i);
                    score = rank;
                }
            }
            if (chosen < 0)
                throw std::runtime_error("No suitable Vulkan memory within the working-memory limit");
            flags = context.memoryProperties.memoryTypes[chosen].propertyFlags;
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = uint32_t(chosen);
            context.check(context.AllocateMemory(context.device, &allocation, nullptr, &memory), "allocate memory");
            context.check(context.BindBufferMemory(context.device, handle, memory, 0), "bind buffer");
            if (host)
                context.check(context.MapMemory(context.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map memory");
        }
        void synchronize(bool flush) {
            if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                return;
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = memory;
            range.offset = 0;
            range.size = VK_WHOLE_SIZE;
            context.check(flush ? context.FlushMappedMemoryRanges(context.device, 1, &range)
                                : context.InvalidateMappedMemoryRanges(context.device, 1, &range),
                          "synchronize mapped memory");
        }
    };
    std::unique_ptr<Buffer> hostBuffer, localBuffer;
    VkDeviceSize capacity = 0;
    std::vector<VkDeviceSize> sourceLayout;
    std::vector<qint64> sourceKeys;
    void freeBuffers() {
        hostBuffer.reset();
        localBuffer.reset();
        capacity = 0;
        sourceKeys.clear();
        sourceLayout.clear();
    }
};

void Context::initialize() {
    if (!loader.load())
        throw std::runtime_error("Vulkan loader unavailable");
    getInstanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader.resolve("vkGetInstanceProcAddr"));
    if (!getInstanceProc)
        throw std::runtime_error("Vulkan loader has no entry point");
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(getInstanceProc(nullptr, "vkCreateInstance"));
    if (!createInstance)
        throw std::runtime_error("Vulkan loader cannot create an instance");
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "CompositorLX Renderer";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    check(createInstance(&info, nullptr, &instance), "create instance");
#define LOAD_INSTANCE(name)                                                                                            \
    name = reinterpret_cast<PFN_vk##name>(getInstanceProc(instance, "vk" #name));                                      \
    if (!name)                                                                                                         \
        throw std::runtime_error("Missing Vulkan function: " #name);
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    uint32_t count = 0;
    check(EnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
    std::vector<VkPhysicalDevice> devices(count);
    check(EnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
    // Automatic device choice prefers the integrated GPU on hybrid laptops: it does not wake
    // a discrete GPU (OD6). A saved or environment device name selects one explicitly.
    QString wanted = qEnvironmentVariable("COMPOSITOR_GPU_DEVICE");
    if (wanted.isEmpty())
        wanted = QSettings().value("rendering/device").toString();
    // Tests may run on software Vulkan (lavapipe) in CI; it is never used as acceleration otherwise.
    const bool allowSoftware = qEnvironmentVariableIsSet("COMPOSITOR_GPU_ALLOW_SOFTWARE");
    int best = -1;
    for (uint32_t d = 0; d < count; ++d) {
        VkPhysicalDeviceProperties p{};
        GetPhysicalDeviceProperties(devices[d], &p);
        const bool hardware = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                              p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (!hardware && !(allowSoftware && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU))
            continue;
        if (p.limits.maxComputeWorkGroupInvocations < 64 || p.limits.maxComputeWorkGroupSize[0] < 8 ||
            p.limits.maxComputeWorkGroupSize[1] < 8 || p.limits.maxPushConstantsSize < sizeof(Parameters) ||
            p.limits.maxPerStageDescriptorStorageBuffers < 3 || p.limits.maxDescriptorSetStorageBuffers < 3)
            continue;
        uint32_t families = 0;
        GetPhysicalDeviceQueueFamilyProperties(devices[d], &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        GetPhysicalDeviceQueueFamilyProperties(devices[d], &families, queues.data());
        for (uint32_t q = 0; q < families; ++q)
            if (queues[q].queueCount && (queues[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                const QString name = QString::fromUtf8(p.deviceName);
                deviceNames << name;
                int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                           : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU  ? 1
                                                                                   : 0;
                if (!wanted.isEmpty() && name == wanted)
                    rank = 10;
                if (rank > best) {
                    physical = devices[d];
                    properties = p;
                    queueFamily = q;
                    best = rank;
                }
                break;
            }
    }
    if (!physical)
        throw std::runtime_error("No hardware Vulkan compute device available");
    GetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    const float priority = 1;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = queueFamily;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    VkDeviceCreateInfo d{};
    d.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    d.queueCreateInfoCount = 1;
    d.pQueueCreateInfos = &q;
    check(CreateDevice(physical, &d, nullptr, &device), "create device");
#define LOAD_DEVICE(name)                                                                                              \
    name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name));                                      \
    if (!name)                                                                                                         \
        throw std::runtime_error("Missing Vulkan function: " #name);
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    GetDeviceQueue(device, queueFamily, 0, &queue);
    VkShaderModuleCreateInfo module{};
    module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module.codeSize = sizeof(renderingSpirv);
    module.pCode = renderingSpirv;
    check(CreateShaderModule(device, &module, nullptr, &shader), "create shader");
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = bindings.size();
    layout.pBindings = bindings.data();
    check(CreateDescriptorSetLayout(device, &layout, nullptr, &descriptorLayout), "create descriptor layout");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &descriptorLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    check(CreatePipelineLayout(device, &pl, nullptr, &pipelineLayout), "create pipeline layout");
    loadPipelineCache();
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * uint32_t(descriptorSets.size())};
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = descriptorSets.size();
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &poolSize;
    check(CreateDescriptorPool(device, &dp, nullptr, &descriptorPool), "create descriptor pool");
    VkDescriptorSetAllocateInfo set{};
    set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    std::array<VkDescriptorSetLayout, 2048> layouts;
    layouts.fill(descriptorLayout);
    set.descriptorPool = descriptorPool;
    set.descriptorSetCount = layouts.size();
    set.pSetLayouts = layouts.data();
    check(AllocateDescriptorSets(device, &set, descriptorSets.data()), "allocate descriptor sets");
    VkCommandPoolCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.queueFamilyIndex = queueFamily;
    check(CreateCommandPool(device, &cp, nullptr, &commandPool), "create command pool");
    VkCommandBufferAllocateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = commandPool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    check(AllocateCommandBuffers(device, &cb, &command), "allocate command buffer");
    VkFenceCreateInfo f{};
    f.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check(CreateFence(device, &f, nullptr, &fence), "create fence");
}

// The persistent cache lets later sessions skip the driver's shader compile. The header is
// checked against this device so a driver or GPU change never loads foreign data.
void Context::loadPipelineCache() {
    QByteArray data;
    if (!qEnvironmentVariableIsSet("COMPOSITOR_GPU_NO_PIPELINE_CACHE")) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (!dir.isEmpty())
            cachePath = dir + QStringLiteral("/vulkan-pipelines-%1-%2.bin")
                                  .arg(properties.vendorID, 4, 16, QLatin1Char('0'))
                                  .arg(properties.deviceID, 4, 16, QLatin1Char('0'));
        QFile file(cachePath);
        if (!cachePath.isEmpty() && file.open(QIODevice::ReadOnly))
            data = file.readAll();
        const auto word = [&](int at) {
            uint32_t v = 0;
            std::memcpy(&v, data.constData() + at, 4);
            return v;
        };
        if (data.size() < 32 || word(4) != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || word(8) != properties.vendorID ||
            word(12) != properties.deviceID ||
            std::memcmp(data.constData() + 16, properties.pipelineCacheUUID, VK_UUID_SIZE) != 0)
            data.clear();
    }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = size_t(data.size());
    info.pInitialData = data.isEmpty() ? nullptr : data.constData();
    if (CreatePipelineCache(device, &info, nullptr, &pipelineCache) != VK_SUCCESS) {
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        check(CreatePipelineCache(device, &info, nullptr, &pipelineCache), "create pipeline cache");
    }
}
void Context::savePipelineCache() {
    if (!pipelineCache || cachePath.isEmpty() || !GetPipelineCacheData)
        return;
    size_t size = 0;
    if (GetPipelineCacheData(device, pipelineCache, &size, nullptr) != VK_SUCCESS || !size)
        return;
    QByteArray data(qsizetype(size), Qt::Uninitialized);
    if (GetPipelineCacheData(device, pipelineCache, &size, data.data()) != VK_SUCCESS)
        return;
    data.resize(qsizetype(size));
    QDir().mkpath(QFileInfo(cachePath).absolutePath());
    QFile file(cachePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(data);
}
// One pipeline per operation, specialized so the driver compiles only that branch and
// sizes registers for it alone.
VkPipeline Context::pipelineFor(GpuOp op) {
    VkPipeline &pipeline = pipelines[unsigned(op)];
    if (pipeline)
        return pipeline;
    const uint32_t id = uint32_t(op);
    VkSpecializationMapEntry entry{0, 0, sizeof(id)};
    VkSpecializationInfo specialization{1, &entry, sizeof(id), &id};
    VkComputePipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipe.stage.module = shader;
    pipe.stage.pName = "main";
    pipe.stage.pSpecializationInfo = &specialization;
    pipe.layout = pipelineLayout;
    check(CreateComputePipelines(device, pipelineCache, 1, &pipe, nullptr, &pipeline), "create compute pipeline");
    return pipeline;
}

bool Context::render(const GpuGraph &graph, int output, GpuFormat outputFormat, const Consumer &consume,
                     GpuExecution &report) {
    report.device = QString::fromUtf8(properties.deviceName);
    if (!error.isEmpty()) {
        report.reason = error;
        return false;
    }
    const auto &nodes = graph.nodes();
    const int count = int(nodes.size());
    // Everything that does not touch the device happens before the queue is locked.
    std::vector<bool> used(count, false);
    std::vector<int> last(count, -1);
    std::vector<Prepared> prepared(count);
    std::vector<GpuFormat> formats(count, GpuFormat::Float32);
    std::vector<VkDeviceSize> sizes(count);
    std::vector<int> sources, passes;
    try {
        if (count > 4096)
            throw std::runtime_error("GPU graph exceeds the node limit");
        used[output] = true;
        last[output] = count;
        for (int i = output; i >= 0; --i)
            if (used[i] && nodes[i].operation != GpuOp::Source) {
                for (int input : {nodes[i].input, nodes[i].auxiliary})
                    if (input >= 0) {
                        if (input >= i)
                            throw std::runtime_error("GPU graph is not acyclic");
                        used[input] = true;
                        last[input] = std::max(last[input], i);
                    }
            }
        for (int i = 0; i < count; ++i) {
            if (!used[i])
                continue;
            const auto &n = nodes[i];
            if (n.size.isEmpty())
                throw std::runtime_error("Empty image in GPU graph");
            for (float f : n.parameters)
                if (!std::isfinite(f))
                    throw std::runtime_error("Non-finite GPU parameters");
            if (n.operation == GpuOp::Source) {
                prepared[i] = prepare(n);
                if (!n.channel && prepared[i].image.isNull())
                    throw std::runtime_error("Could not convert GPU source");
                formats[i] = prepared[i].format;
                sources.push_back(i);
            } else {
                formats[i] = i == output ? outputFormat
                                         : passFormat(n, n.input >= 0 ? formats[n.input] : GpuFormat::Float32);
                // An update writes into its input, so the input must be a pass of its kind that nothing else reads.
                if (n.inPlace && (n.input < 0 || nodes[n.input].operation == GpuOp::Source || last[n.input] != i ||
                                  nodes[n.input].size != n.size || formats[n.input] != formats[i] ||
                                  n.operation < GpuOp::LensPoint))
                    throw std::runtime_error("Invalid in-place GPU pass");
                passes.push_back(i);
            }
            sizes[i] = storageBytes(formats[i], n.size);
            if (sizes[i] > properties.limits.maxStorageBufferRange)
                throw std::runtime_error("GPU storage range exceeded");
        }
        if (passes.size() > descriptorSets.size())
            throw std::runtime_error("Too many GPU passes");
    } catch (const std::exception &e) {
        report.reason = QString::fromUtf8(e.what());
        return false;
    }
    // Interactive work on the GUI thread never queues behind a canvas render or an export.
    const bool gui = report.priority == GpuPriority::Interactive && QCoreApplication::instance() &&
                     QThread::currentThread() == QCoreApplication::instance()->thread();
    if (!lock.acquire(report.priority, std::chrono::milliseconds(gui ? 0 : -1))) {
        report.reason = QStringLiteral("GPU busy; rendered on the CPU");
        return false;
    }
    Held held{lock};
    QElapsedTimer timer;
    timer.start();
    try {
        const auto align = [&](VkDeviceSize bytes) {
            const auto a = std::max(VkDeviceSize(16), properties.limits.minStorageBufferOffsetAlignment);
            return (bytes + a - 1) / a * a;
        };
        std::vector<VkDeviceSize> offsets(count);
        VkDeviceSize total = 0;
        // Reuse allocations once their final consumer has completed, and transient sources' too (uploaded again next
        // time; the others stay resident for the next graph). The slots tile the buffer in offset order: neighbors
        // that are both free merge, a free slot larger than a pass needs is split, and a free slot at the end grows, so
        // small passes never strand a large one.
        struct Slot {
            VkDeviceSize offset, size;
            int until;
        };
        std::vector<Slot> allocations;
        for (int i : sources) {
            offsets[i] = total;
            allocations.push_back({total, align(sizes[i]), nodes[i].transient ? last[i] : count});
            total += align(sizes[i]);
        }
        for (int i : passes) {
            if (nodes[i].inPlace) {
                offsets[i] = offsets[nodes[i].input];
                for (auto &slot : allocations)
                    if (slot.offset == offsets[i])
                        slot.until = last[i];
                continue;
            }
            for (size_t slot = 0; slot + 1 < allocations.size();)
                if (allocations[slot].until < i && allocations[slot + 1].until < i) {
                    allocations[slot].size += allocations[slot + 1].size;
                    allocations.erase(allocations.begin() + std::ptrdiff_t(slot) + 1);
                } else
                    ++slot;
            const VkDeviceSize needed = align(sizes[i]);
            auto free = allocations.end();
            for (auto slot = allocations.begin(); slot != allocations.end(); ++slot)
                if (slot->until < i && slot->size >= needed && (free == allocations.end() || slot->size < free->size))
                    free = slot;
            if (free == allocations.end() && !allocations.empty() && allocations.back().until < i) {
                // The free slot at the end grows to fit.
                total += needed - allocations.back().size;
                allocations.back().size = needed;
                free = allocations.end() - 1;
            }
            if (free != allocations.end()) {
                if (free->size > needed) {
                    const Slot rest{free->offset + needed, free->size - needed, -1};
                    free->size = needed;
                    free = allocations.insert(free + 1, rest) - 1;
                }
                offsets[i] = free->offset;
                free->until = last[i];
            } else {
                offsets[i] = total;
                allocations.push_back({total, needed, last[i]});
                total += needed;
            }
        }
        std::vector<bool> overwritten(sources.size(), false);
        for (size_t root = 0; root < sources.size(); ++root)
            for (int i : passes)
                if (offsets[i] < offsets[sources[root]] + sizes[sources[root]] && offsets[sources[root]] < offsets[i] + sizes[i])
                    overwritten[root] = true;
        const VkDeviceSize budget = GpuRenderer::memoryBudgetBytes();
        const bool forceStaging = qEnvironmentVariableIsSet("COMPOSITOR_GPU_FORCE_STAGING");
        if (total > budget)
            throw std::runtime_error("GPU graph exceeds the working-memory budget");
        if (!hostBuffer || capacity < total || capacity * (localBuffer ? 2 : 1) > budget ||
            (forceStaging && !localBuffer)) {
            freeBuffers();
            auto host = std::make_unique<Buffer>(*this);
            host->allocate(total, true);
            std::unique_ptr<Buffer> local;
            if (forceStaging || !(host->flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                if (total > budget / 2)
                    throw std::runtime_error("GPU staging exceeds the working-memory budget");
                local = std::make_unique<Buffer>(*this);
                local->allocate(total, false);
            }
            hostBuffer = std::move(host);
            localBuffer = std::move(local);
            capacity = total;
        }
        auto &host = *hostBuffer;
        const bool shared = !localBuffer;
        const VkBuffer buffer = shared ? host.handle : localBuffer->handle;
        auto *mapped = static_cast<unsigned char *>(host.mapped);
        std::vector<VkDeviceSize> layout;
        for (int i : sources) {
            layout.push_back(offsets[i]);
            layout.push_back(sizes[i]);
            layout.push_back(VkDeviceSize(formats[i]));
        }
        if (layout != sourceLayout) {
            sourceLayout = layout;
            sourceKeys.assign(sources.size(), 0);
        }
        std::vector<VkBufferCopy> uploads;
        for (size_t root = 0; root < sources.size(); ++root) {
            const int i = sources[root];
            if (sourceKeys[root] == nodes[i].key)
                continue;
            unsigned char *target = mapped + offsets[i];
            if (const auto &values = prepared[i].channel)
                std::memcpy(target, values->data(), values->size() * sizeof(float));
            else {
                const QImage &image = prepared[i].image;
                const size_t row = size_t(image.width()) * bytesPerPixel[unsigned(formats[i])];
                for (int y = 0; y < image.height(); ++y)
                    std::memcpy(target + VkDeviceSize(y) * row, image.constScanLine(y), row);
            }
            uploads.push_back({offsets[i], offsets[i], sizes[i]});
            report.uploadedBytes += sizes[i];
        }
        host.synchronize(true);
        for (size_t pass = 0; pass < passes.size(); ++pass) {
            const int i = passes[pass];
            const auto &n = nodes[i];
            const int in = n.input < 0 ? i : n.input, aux = n.auxiliary < 0 ? in : n.auxiliary;
            const std::array<VkDescriptorBufferInfo, 3> buffers{{{buffer, offsets[in], sizes[in]},
                                                                 {buffer, offsets[aux], sizes[aux]},
                                                                 {buffer, offsets[i], sizes[i]}}};
            std::array<VkWriteDescriptorSet, 3> writes{};
            for (uint32_t binding = 0; binding < 3; ++binding) {
                auto &w = writes[binding];
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = descriptorSets[pass];
                w.dstBinding = binding;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w.pBufferInfo = &buffers[binding];
            }
            UpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
        }
        check(ResetCommandPool(device, commandPool, 0), "reset command pool");
        check(ResetFences(device, 1, &fence), "reset fence");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(BeginCommandBuffer(command, &begin), "begin graph");
        if (!shared && !uploads.empty()) {
            CmdCopyBuffer(command, host.handle, buffer, uploads.size(), uploads.data());
            barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
        } else
            barrier(VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT);
        VkPipeline bound = VK_NULL_HANDLE;
        for (size_t pass = 0; pass < passes.size(); ++pass) {
            const int i = passes[pass];
            const auto &n = nodes[i];
            const int in = n.input < 0 ? i : n.input, aux = n.auxiliary < 0 ? in : n.auxiliary;
            const uint32_t flags = (n.quantization & 15u) | (uint32_t(formats[in]) << 4) |
                                   (uint32_t(formats[aux]) << 8) | (uint32_t(formats[i]) << 12);
            Parameters params{uint32_t(n.size.width()), uint32_t(n.size.height()), uint32_t(n.operation), flags,
                              n.parameters};
            if (!n.region.isEmpty()) {
                params.values[gpuRegionX] = float(n.region.x());
                params.values[gpuRegionY] = float(n.region.y());
                params.values[gpuRegionWidth] = float(n.region.width());
                params.values[gpuRegionHeight] = float(n.region.height());
            }
            if (const VkPipeline pipeline = pipelineFor(n.operation); pipeline != bound) {
                CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                bound = pipeline;
            }
            CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSets[pass],
                                  0, nullptr);
            CmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
            uint32_t workWidth = params.width, workHeight = params.height;
            if (!n.region.isEmpty()) {
                workWidth = uint32_t(n.region.width());
                workHeight = uint32_t(n.region.height());
            }
            if (n.operation == GpuOp::BoxBlur) {
                const bool horizontal = n.parameters[GpuParam::BoxBlur::axis] == 0;
                const uint32_t span = uint32_t(std::max(1.f, n.parameters[GpuParam::BoxBlur::span]));
                (horizontal ? workWidth : workHeight) = ((horizontal ? workWidth : workHeight) + span - 1) / span;
            }
            if ((workWidth + 7) / 8 > properties.limits.maxComputeWorkGroupCount[0] ||
                (workHeight + 7) / 8 > properties.limits.maxComputeWorkGroupCount[1])
                throw std::runtime_error("GPU dispatch limit exceeded");
            CmdDispatch(command, (workWidth + 7) / 8, (workHeight + 7) / 8, 1);
            barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        if (!shared) {
            barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferCopy download{offsets[output], offsets[output], sizes[output]};
            CmdCopyBuffer(command, buffer, host.handle, 1, &download);
            barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                    VK_ACCESS_HOST_READ_BIT);
        } else
            barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                    VK_ACCESS_HOST_READ_BIT);
        check(EndCommandBuffer(command), "end graph");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(QueueSubmit(queue, 1, &submit, fence), "submit graph");
        check(WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait for graph");
        host.synchronize(false);
        for (size_t root = 0; root < sources.size(); ++root)
            sourceKeys[root] = overwritten[root] ? 0 : nodes[sources[root]].key;
        consume(mapped + offsets[output], formats[output]);
        report.accelerated = true;
        report.downloadedBytes = sizes[output];
        report.workingBytes = capacity * (shared ? 1 : 2);
        report.dispatches = passes.size();
        report.milliseconds = timer.nsecsElapsed() / 1e6;
        touch();
        return true;
    } catch (const std::exception &e) {
        sourceKeys.assign(sourceKeys.size(), 0);
        report.reason = QString::fromUtf8(e.what());
        report.milliseconds = timer.nsecsElapsed() / 1e6;
        touch();
        return false;
    }
}
#undef INSTANCE_FUNCTIONS
#undef DEVICE_FUNCTIONS

// The context is created on first use and can be released (CPU mode) or replaced once
// after a lost device; in-flight renders keep their own reference.
std::mutex contextMutex;
std::shared_ptr<Context> contextStorage;
int deviceRecoveries = 0;
std::shared_ptr<Context> context(bool create = true) {
    std::lock_guard lock(contextMutex);
    static const bool registered = [] {
        // Drivers/validation layers can create lazy statics during rendering. Release the
        // device before their exit handlers, after preview workers finish.
        qAddPostRoutine([] {
            QThreadPool::globalInstance()->waitForDone();
            std::shared_ptr<Context> last;
            {
                std::lock_guard lock(contextMutex);
                last = std::move(contextStorage);
            }
        });
        return true;
    }();
    Q_UNUSED(registered)
    if (!contextStorage && create)
        contextStorage = std::make_shared<Context>();
    return contextStorage;
}
void afterRender(const std::shared_ptr<Context> &used) {
    if (!used->deviceLost())
        return;
    std::lock_guard lock(contextMutex);
    if (contextStorage == used && deviceRecoveries < 1) {
        ++deviceRecoveries; // Recover once; a second loss keeps the latched error.
        contextStorage.reset();
    }
}
void copyMetadata(QImage &result, const GpuGraph &graph) {
    for (const auto &node : graph.nodes())
        if (node.operation == GpuOp::Source && !node.channel) {
            result.setColorSpace(node.image.colorSpace());
            result.setDotsPerMeterX(node.image.dotsPerMeterX());
            result.setDotsPerMeterY(node.image.dotsPerMeterY());
            for (const auto &key : node.image.textKeys())
                result.setText(key, node.image.text(key));
            return;
        }
}
} // namespace
QImage executeVulkanGraph(const GpuGraph &graph, int output, QImage::Format format, GpuExecution &report) {
    const auto ctx = context();
    const QSize size = graph.nodes()[output].size;
    QImage result;
    const bool ok = ctx->render(
        graph, output, outputFormatFor(format),
        [&](const unsigned char *data, GpuFormat storage) {
            result = QImage(size, imageFormatFor(storage));
            if (result.isNull())
                throw std::runtime_error("Could not allocate GPU readback");
            const size_t row = size_t(size.width()) * bytesPerPixel[unsigned(storage)];
            for (int y = 0; y < size.height(); ++y)
                std::memcpy(result.scanLine(y), data + VkDeviceSize(y) * row, row);
        },
        report);
    afterRender(ctx);
    if (!ok)
        return {};
    copyMetadata(result, graph);
    if (result.format() != format)
        result = result.convertToFormat(format);
    if (result.isNull()) {
        report.accelerated = false;
        report.reason = QStringLiteral("Could not convert GPU result");
    }
    return result;
}
std::vector<float> executeVulkanChannel(const GpuGraph &graph, int output, GpuExecution &report) {
    const auto ctx = context();
    const QSize size = graph.nodes()[output].size;
    std::vector<float> result;
    const bool ok = ctx->render(
        graph, output, GpuFormat::Red32,
        [&](const unsigned char *data, GpuFormat) {
            const auto *values = reinterpret_cast<const float *>(data);
            result.assign(values, values + size_t(size.width()) * size.height());
        },
        report);
    afterRender(ctx);
    return ok ? result : std::vector<float>{};
}
QString vulkanDeviceDescription() {
    const auto info = context()->info();
    return info.available ? QStringLiteral("Vulkan: %1").arg(info.name) : info.error;
}
GpuDeviceInfo vulkanDeviceInfo(bool open) {
    const auto ctx = context(open);
    if (!ctx) {
        GpuDeviceInfo info;
        info.error = QStringLiteral("CPU rendering selected; the Vulkan device is closed");
        return info;
    }
    return ctx->info();
}
void warmUpVulkan() { context()->warmUp(); }
void trimVulkan() {
    if (const auto ctx = context(false))
        ctx->trim();
}
void releaseVulkan() {
    std::shared_ptr<Context> old;
    std::lock_guard lock(contextMutex);
    old = std::move(contextStorage);
    // `old` is destroyed after the lock is released, outside any render.
}
} // namespace compositor
#else
namespace compositor {
QImage executeVulkanGraph(const GpuGraph &, int, QImage::Format, GpuExecution &report) {
    report.reason = QStringLiteral("This build has no Vulkan support");
    return {};
}
std::vector<float> executeVulkanChannel(const GpuGraph &, int, GpuExecution &report) {
    report.reason = QStringLiteral("This build has no Vulkan support");
    return {};
}
QString vulkanDeviceDescription() { return QStringLiteral("This build has no Vulkan support"); }
GpuDeviceInfo vulkanDeviceInfo(bool) {
    GpuDeviceInfo info;
    info.error = QStringLiteral("This build has no Vulkan support");
    return info;
}
void warmUpVulkan() {}
void trimVulkan() {}
void releaseVulkan() {}
} // namespace compositor
#endif
