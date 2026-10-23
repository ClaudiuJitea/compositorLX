#include "rendering/RgbSplitVulkan.h"

#ifdef COMPOSITOR_HAVE_VULKAN
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "rendering/shaders/RgbSplitSpirv.h"
#include <QLibrary>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace compositor {
namespace {
#define INSTANCE_FUNCTIONS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) X(CreateShaderModule) X(DestroyShaderModule) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(CmdPipelineBarrier) X(CmdCopyBuffer) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
    X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(QueueSubmit) X(WaitForFences)

struct Parameters {
    uint32_t width, height, spectral, smear;
    float distance, blur, directionX, directionY;
    float centerX, centerY, reach;
    uint32_t radial, edges;
};
static_assert(sizeof(Parameters) == 52);

// One compute queue/pipeline, serialized across previews, canvas workers and exports. Vulkan
// objects never depend on the Qt window system, so the same path works in headless tests.
class Context {
public:
    Context() { try { initialize(); } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); } }
    ~Context() {
        if (device && DestroyDevice) {
            if (DeviceWaitIdle) DeviceWaitIdle(device);
            if (fence) DestroyFence(device, fence, nullptr);
            if (commandPool) DestroyCommandPool(device, commandPool, nullptr);
            if (descriptorPool) DestroyDescriptorPool(device, descriptorPool, nullptr);
            if (pipeline) DestroyPipeline(device, pipeline, nullptr);
            if (pipelineLayout) DestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (descriptorLayout) DestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
            if (shader) DestroyShaderModule(device, shader, nullptr);
            DestroyDevice(device, nullptr);
        }
        if (instance && DestroyInstance) DestroyInstance(instance, nullptr);
    }
    QImage render(const QImage &image, const RgbSplitSettings &settings, unsigned edges, RgbSplitExecution &report);

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
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    QString error;
    std::mutex mutex;

    void check(VkResult result, const char *operation) {
        if (result == VK_SUCCESS) return;
        const QString message = QStringLiteral("Vulkan %1 failed (%2)").arg(QLatin1String(operation)).arg(int(result));
        if (result == VK_ERROR_DEVICE_LOST) error = message; // Do not retry a lost device on every preview.
        throw std::runtime_error(message.toStdString());
    }
    void initialize();
    void barrier(VkPipelineStageFlags from, VkAccessFlags writes, VkPipelineStageFlags to, VkAccessFlags reads) {
        VkMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = writes; b.dstAccessMask = reads;
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
            if (mapped) context.UnmapMemory(context.device, memory);
            if (handle) context.DestroyBuffer(context.device, handle, nullptr);
            if (memory) context.FreeMemory(context.device, memory, nullptr);
        }
        void allocate(VkDeviceSize bytes, bool host) {
            VkBufferCreateInfo b{}; b.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            b.size = bytes; b.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            b.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            context.check(context.CreateBuffer(context.device, &b, nullptr, &handle), "create buffer");
            VkMemoryRequirements requirements{}; context.GetBufferMemoryRequirements(context.device, handle, &requirements);
            int chosen = -1, score = -1;
            for (uint32_t i = 0; i < context.memoryProperties.memoryTypeCount; ++i) {
                if (!(requirements.memoryTypeBits & (1u << i))) continue;
                const auto &type = context.memoryProperties.memoryTypes[i];
                const auto required = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                if (!(type.propertyFlags & required)) continue;
                const auto &heap = context.memoryProperties.memoryHeaps[type.heapIndex];
                if (requirements.size > heap.size / 4) continue;
                const int rank = ((type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 4 : 0)
                    + ((type.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 2 : 0)
                    + ((type.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0);
                if (rank > score) { chosen = int(i); score = rank; }
            }
            if (chosen < 0) throw std::runtime_error("No suitable Vulkan memory within the working-memory limit");
            flags = context.memoryProperties.memoryTypes[chosen].propertyFlags;
            VkMemoryAllocateInfo allocation{}; allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = uint32_t(chosen);
            context.check(context.AllocateMemory(context.device, &allocation, nullptr, &memory), "allocate memory");
            context.check(context.BindBufferMemory(context.device, handle, memory, 0), "bind buffer");
            if (host) context.check(context.MapMemory(context.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map memory");
        }
        void synchronize(bool flush) {
            if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return;
            VkMappedMemoryRange range{}; range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = memory; range.offset = 0; range.size = VK_WHOLE_SIZE;
            context.check(flush ? context.FlushMappedMemoryRanges(context.device, 1, &range)
                                : context.InvalidateMappedMemoryRanges(context.device, 1, &range), "synchronize mapped memory");
        }
    };
};

void Context::initialize()
{
    if (!loader.load()) throw std::runtime_error("Vulkan loader unavailable");
    getInstanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(loader.resolve("vkGetInstanceProcAddr"));
    if (!getInstanceProc) throw std::runtime_error("Vulkan loader has no entry point");
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(getInstanceProc(nullptr, "vkCreateInstance"));
    if (!createInstance) throw std::runtime_error("Vulkan loader cannot create an instance");
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "CompositorLX RGB Split"; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; info.pApplicationInfo = &app;
    check(createInstance(&info, nullptr, &instance), "create instance");
#define LOAD_INSTANCE(name) name = reinterpret_cast<PFN_vk##name>(getInstanceProc(instance, "vk" #name)); if (!name) throw std::runtime_error("Missing Vulkan function: " #name);
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    uint32_t count = 0; check(EnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
    std::vector<VkPhysicalDevice> devices(count);
    check(EnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
    int best = -1;
    for (uint32_t d = 0; d < count; ++d) {
        VkPhysicalDeviceProperties p{}; GetPhysicalDeviceProperties(devices[d], &p);
        // Software Vulkan is a CPU renderer, not hardware acceleration.
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
        if (p.limits.maxComputeWorkGroupInvocations < 64 || p.limits.maxComputeWorkGroupSize[0] < 8
            || p.limits.maxComputeWorkGroupSize[1] < 8 || p.limits.maxPushConstantsSize < sizeof(Parameters)
            || p.limits.maxPerStageDescriptorStorageBuffers < 3 || p.limits.maxDescriptorSetStorageBuffers < 3) continue;
        uint32_t families = 0; GetPhysicalDeviceQueueFamilyProperties(devices[d], &families, nullptr);
        std::vector<VkQueueFamilyProperties> queues(families);
        GetPhysicalDeviceQueueFamilyProperties(devices[d], &families, queues.data());
        for (uint32_t q = 0; q < families; ++q) if (queues[q].queueCount && (queues[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            const int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (rank > best) { physical = devices[d]; properties = p; queueFamily = q; best = rank; }
            break;
        }
    }
    if (!physical) throw std::runtime_error("No hardware Vulkan compute device available");
    GetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
    const float priority = 1;
    VkDeviceQueueCreateInfo q{}; q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = queueFamily; q.queueCount = 1; q.pQueuePriorities = &priority;
    VkDeviceCreateInfo d{}; d.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; d.queueCreateInfoCount = 1; d.pQueueCreateInfos = &q;
    check(CreateDevice(physical, &d, nullptr, &device), "create device");
#define LOAD_DEVICE(name) name = reinterpret_cast<PFN_vk##name>(GetDeviceProcAddr(device, "vk" #name)); if (!name) throw std::runtime_error("Missing Vulkan function: " #name);
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    GetDeviceQueue(device, queueFamily, 0, &queue);
    VkShaderModuleCreateInfo module{}; module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module.codeSize = sizeof(rgbSplitSpirv); module.pCode = rgbSplitSpirv;
    check(CreateShaderModule(device, &module, nullptr, &shader), "create shader");
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) { bindings[i].binding = i; bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo layout{}; layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = bindings.size(); layout.pBindings = bindings.data();
    check(CreateDescriptorSetLayout(device, &layout, nullptr, &descriptorLayout), "create descriptor layout");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Parameters)};
    VkPipelineLayoutCreateInfo pl{}; pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1; pl.pSetLayouts = &descriptorLayout; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &push;
    check(CreatePipelineLayout(device, &pl, nullptr, &pipelineLayout), "create pipeline layout");
    VkComputePipelineCreateInfo pipe{}; pipe.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipe.stage.module = shader; pipe.stage.pName = "main"; pipe.layout = pipelineLayout;
    check(CreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline), "create compute pipeline");
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo dp{}; dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 1; dp.poolSizeCount = 1; dp.pPoolSizes = &poolSize;
    check(CreateDescriptorPool(device, &dp, nullptr, &descriptorPool), "create descriptor pool");
    VkDescriptorSetAllocateInfo set{}; set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set.descriptorPool = descriptorPool; set.descriptorSetCount = 1; set.pSetLayouts = &descriptorLayout;
    check(AllocateDescriptorSets(device, &set, &descriptorSet), "allocate descriptor set");
    VkCommandPoolCreateInfo cp{}; cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cp.queueFamilyIndex = queueFamily;
    check(CreateCommandPool(device, &cp, nullptr, &commandPool), "create command pool");
    VkCommandBufferAllocateInfo cb{}; cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = commandPool; cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cb.commandBufferCount = 1;
    check(AllocateCommandBuffers(device, &cb, &command), "allocate command buffer");
    VkFenceCreateInfo f{}; f.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check(CreateFence(device, &f, nullptr, &fence), "create fence");
}

QImage Context::render(const QImage &image, const RgbSplitSettings &s, unsigned edges, RgbSplitExecution &report)
{
    std::lock_guard lock(mutex);
    report.deviceName = QString::fromUtf8(properties.deviceName);
    if (!error.isEmpty()) { report.fallbackReason = error; return {}; }
    try {
        for (double value : {s.distance, s.blur, s.angle, s.center.x(), s.center.y(), s.reach})
            if (!std::isfinite(value) || std::abs(value) > 10000000) throw std::runtime_error("Parameters exceed Vulkan precision limits");
        const VkDeviceSize pixels = VkDeviceSize(image.width()) * image.height(), bytes = pixels * 16;
        const auto align = [&](VkDeviceSize n) { const auto a = std::max(VkDeviceSize(1), properties.limits.minStorageBufferOffsetAlignment); return (n + a - 1) / a * a; };
        const VkDeviceSize noiseOffset = align(bytes), outputOffset = align(noiseOffset + pixels * sizeof(float)), total = outputOffset + bytes;
        if (bytes > properties.limits.maxStorageBufferRange || total > 1024ULL * 1024 * 1024
            || uint32_t((image.width() + 7) / 8) > properties.limits.maxComputeWorkGroupCount[0]
            || uint32_t((image.height() + 7) / 8) > properties.limits.maxComputeWorkGroupCount[1])
            throw std::runtime_error("Image exceeds Vulkan dispatch or working-memory limits");
        QImage input = image.convertToFormat(QImage::Format_RGBA32FPx4_Premultiplied);
        QImage output(image.size(), QImage::Format_RGBA32FPx4_Premultiplied);
        if (input.isNull() || output.isNull()) throw std::runtime_error("Could not allocate Vulkan image conversion");
        Buffer host(*this), local(*this); host.allocate(total, true);
        const bool shared = host.flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (!shared) local.allocate(total, false);
        const VkBuffer buffer = shared ? host.handle : local.handle;
        auto *mapped = static_cast<unsigned char *>(host.mapped);
        for (int y = 0; y < image.height(); ++y) std::memcpy(mapped + VkDeviceSize(y) * image.width() * 16, input.constScanLine(y), size_t(image.width()) * 16);
        auto *noise = reinterpret_cast<float *>(mapped + noiseOffset);
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            const double hash = std::sin(x * 12.9898 + y * 78.233) * 43758.5453;
            noise[VkDeviceSize(y) * image.width() + x] = float(hash - std::floor(hash));
        }
        host.synchronize(true);
        const std::array<VkDescriptorBufferInfo, 3> buffers{{{buffer, 0, bytes}, {buffer, noiseOffset, pixels * sizeof(float)}, {buffer, outputOffset, bytes}}};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = descriptorSet; writes[i].dstBinding = i;
            writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buffers[i];
        }
        UpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
        check(ResetCommandPool(device, commandPool, 0), "reset command pool");
        check(ResetFences(device, 1, &fence), "reset fence");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(BeginCommandBuffer(command, &begin), "begin command buffer");
        if (!shared) {
            VkBufferCopy copy{0, 0, outputOffset}; CmdCopyBuffer(command, host.handle, local.handle, 1, &copy);
            barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        } else barrier(VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        const double radians = s.angle * M_PI / 180;
        const Parameters p{uint32_t(image.width()), uint32_t(image.height()), uint32_t(std::clamp(std::ceil(s.distance * 2) + 1, 1., 48.)),
            s.blur > 0 ? uint32_t(std::clamp(std::ceil(s.blur / 3), 2., 24.)) : 1u,
            float(s.distance), float(s.blur), float(std::cos(radians)), float(std::sin(radians)),
            float(s.center.x()), float(s.center.y()), float(s.reach), s.radial ? 1u : 0u, edges & 15u};
        CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        CmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        CmdDispatch(command, (p.width + 7) / 8, (p.height + 7) / 8, 1);
        if (!shared) {
            barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferCopy copy{outputOffset, outputOffset, bytes}; CmdCopyBuffer(command, local.handle, host.handle, 1, &copy);
            barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        } else barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        check(EndCommandBuffer(command), "end command buffer");
        VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        check(QueueSubmit(queue, 1, &submit, fence), "submit compute");
        // Resources must stay alive until this submission finishes (including on slow devices).
        check(WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait for compute");
        host.synchronize(false);
        for (int y = 0; y < output.height(); ++y) std::memcpy(output.scanLine(y), mapped + outputOffset + VkDeviceSize(y) * output.width() * 16, size_t(output.width()) * 16);
        QImage result = output.convertToFormat(image.format());
        if (result.isNull()) throw std::runtime_error("Could not convert Vulkan output");
        report.backend = RgbSplitBackend::Vulkan;
        return result;
    } catch (const std::exception &e) { report.fallbackReason = QString::fromUtf8(e.what()); return {}; }
}
#undef INSTANCE_FUNCTIONS
#undef DEVICE_FUNCTIONS
}
QImage tryRgbSplitVulkan(const QImage &image, const RgbSplitSettings &settings, unsigned edges, RgbSplitExecution &execution)
{
    static Context context;
    return context.render(image, settings, edges, execution);
}
}
#else
namespace compositor {
QImage tryRgbSplitVulkan(const QImage &, const RgbSplitSettings &, unsigned, RgbSplitExecution &execution)
{
    execution.fallbackReason = QStringLiteral("This build has no Vulkan support");
    return {};
}
}
#endif
