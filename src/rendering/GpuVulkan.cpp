#include "rendering/DeepImage.h"
#include "rendering/GpuRenderer.h"

#ifdef COMPOSITOR_HAVE_VULKAN
#define VK_NO_PROTOTYPES
#include "rendering/shaders/RenderingSpirv.h"
#include <QColorSpace>
#include <QCoreApplication>
#include <QLibrary>
#include <QThreadPool>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>

namespace compositor {
namespace {
#define INSTANCE_FUNCTIONS(X)                                                                                          \
    X(DestroyInstance)                                                                                                 \
    X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceMemoryProperties)                    \
        X(GetPhysicalDeviceQueueFamilyProperties) X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X)                                                                                            \
    X(DestroyDevice)                                                                                                   \
    X(DeviceWaitIdle) X(GetDeviceQueue) X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout)      \
        X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines)       \
            X(DestroyPipeline) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets)              \
                X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers)           \
                    X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBindPipeline)                   \
                        X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(CmdPipelineBarrier)              \
                            X(CmdCopyBuffer) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements)           \
                                X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory)        \
                                    X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) X(CreateFence)          \
                                        X(DestroyFence) X(ResetFences) X(QueueSubmit) X(WaitForFences)

struct Parameters {
    uint32_t width, height, operation, quantization;
    std::array<float, 28> values;
};
static_assert(sizeof(Parameters) == 128);

// One compute queue/pipeline, serialized across previews, canvas workers and exports. Vulkan
// objects never depend on the Qt window system, so the same path works in headless tests.
class Context {
  public:
    Context() {
        try {
            initialize();
        } catch (const std::exception &e) {
            error = QString::fromUtf8(e.what());
        }
    }
    ~Context() {
        if (device && DeviceWaitIdle)
            DeviceWaitIdle(device);
        hostBuffer.reset();
        localBuffer.reset();
        if (device && DestroyDevice) {
            if (fence)
                DestroyFence(device, fence, nullptr);
            if (commandPool)
                DestroyCommandPool(device, commandPool, nullptr);
            if (descriptorPool)
                DestroyDescriptorPool(device, descriptorPool, nullptr);
            if (pipeline)
                DestroyPipeline(device, pipeline, nullptr);
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
    QImage render(const GpuGraph &, int output, QImage::Format, GpuExecution &);
    QString description() {
        std::lock_guard lock(mutex);
        return error.isEmpty() ? QStringLiteral("Vulkan: %1").arg(QString::fromUtf8(properties.deviceName)) : error;
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
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 512> descriptorSets{};
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    QString error;
    std::mutex mutex;

    void check(VkResult result, const char *operation) {
        if (result == VK_SUCCESS)
            return;
        const QString message = QStringLiteral("Vulkan %1 failed (%2)").arg(QLatin1String(operation)).arg(int(result));
        if (result == VK_ERROR_DEVICE_LOST)
            error = message; // Do not retry a lost device on every preview.
        throw std::runtime_error(message.toStdString());
    }
    void initialize();
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
            int chosen = -1, score = -1;
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
                const int rank = ((type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 4 : 0) +
                                 ((type.propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 2 : 0) +
                                 ((type.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0);
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
    int best = -1;
    for (uint32_t d = 0; d < count; ++d) {
        VkPhysicalDeviceProperties p{};
        GetPhysicalDeviceProperties(devices[d], &p);
        // Software Vulkan is a CPU renderer, not hardware acceleration.
        if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
            p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
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
                const int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
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
    VkComputePipelineCreateInfo pipe{};
    pipe.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipe.stage.module = shader;
    pipe.stage.pName = "main";
    pipe.layout = pipelineLayout;
    check(CreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline), "create compute pipeline");
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * uint32_t(descriptorSets.size())};
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = descriptorSets.size();
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &poolSize;
    check(CreateDescriptorPool(device, &dp, nullptr, &descriptorPool), "create descriptor pool");
    VkDescriptorSetAllocateInfo set{};
    set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    std::array<VkDescriptorSetLayout, 512> layouts;
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

QImage Context::render(const GpuGraph &graph, int output, QImage::Format format, GpuExecution &report) {
    std::lock_guard lock(mutex);
    report.device = QString::fromUtf8(properties.deviceName);
    if (!error.isEmpty()) {
        report.reason = error;
        return {};
    }
    try {
        const auto &nodes = graph.nodes();
        const int count = int(nodes.size());
        if (count > 2048)
            throw std::runtime_error("GPU graph exceeds the node limit");
        std::vector<bool> used(count, false);
        used[output] = true;
        std::vector<int> last(count, -1);
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
        const auto align = [&](VkDeviceSize bytes) {
            const auto a = std::max(VkDeviceSize(16), properties.limits.minStorageBufferOffsetAlignment);
            return (bytes + a - 1) / a * a;
        };
        std::vector<VkDeviceSize> offsets(count), sizes(count);
        std::vector<int> sources, passes;
        VkDeviceSize total = 0;
        for (int i = 0; i < count; ++i)
            if (used[i]) {
                const auto &n = nodes[i];
                if (n.size.isEmpty())
                    throw std::runtime_error("Empty image in GPU graph");
                sizes[i] = VkDeviceSize(n.size.width()) * n.size.height() * 16;
                if (sizes[i] > properties.limits.maxStorageBufferRange)
                    throw std::runtime_error("GPU storage range exceeded");
                for (float f : n.parameters)
                    if (!std::isfinite(f))
                        throw std::runtime_error("Non-finite GPU parameters");
                if (n.operation == GpuOp::Source) {
                    offsets[i] = total;
                    total += align(sizes[i]);
                    sources.push_back(i);
                } else {
                    if (uint32_t((n.size.width() + 7) / 8) > properties.limits.maxComputeWorkGroupCount[0] ||
                        uint32_t((n.size.height() + 7) / 8) > properties.limits.maxComputeWorkGroupCount[1])
                        throw std::runtime_error("GPU dispatch limit exceeded");
                    passes.push_back(i);
                }
            }
        if (passes.size() > descriptorSets.size())
            throw std::runtime_error("Too many GPU passes");
        // Reuse intermediate allocations once their final consumer has completed.
        struct Slot {
            VkDeviceSize offset, size;
            int until;
        };
        std::vector<Slot> allocations;
        for (int i : passes) {
            auto free = allocations.end();
            for (auto slot = allocations.begin(); slot != allocations.end(); ++slot)
                if (slot->until < i && slot->size >= sizes[i] && (free == allocations.end() || slot->size < free->size))
                    free = slot;
            if (free != allocations.end()) {
                offsets[i] = free->offset;
                free->until = last[i];
            } else {
                offsets[i] = total;
                allocations.push_back({total, align(sizes[i]), last[i]});
                total += align(sizes[i]);
            }
        }
        const VkDeviceSize budget = VkDeviceSize(GpuRenderer::memoryBudgetMiB()) * 1024 * 1024;
        const bool forceStaging = qEnvironmentVariableIsSet("COMPOSITOR_GPU_FORCE_STAGING");
        if (total > budget)
            throw std::runtime_error("GPU graph exceeds the working-memory budget");
        if (!hostBuffer || capacity < total || capacity * (localBuffer ? 2 : 1) > budget ||
            (forceStaging && !localBuffer)) {
            hostBuffer.reset();
            localBuffer.reset();
            capacity = 0;
            sourceKeys.clear();
            sourceLayout.clear();
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
        }
        if (layout != sourceLayout) {
            sourceLayout = layout;
            sourceKeys.assign(sources.size(), 0);
        }
        std::vector<VkBufferCopy> uploads;
        for (size_t root = 0; root < sources.size(); ++root) {
            const int i = sources[root];
            const auto &image = nodes[i].image;
            if (sourceKeys[root] == image.cacheKey())
                continue;
            const QImage converted = DeepImage::floating(image);
            if (converted.isNull())
                throw std::runtime_error("Could not convert GPU source");
            for (int y = 0; y < image.height(); ++y)
                std::memcpy(mapped + offsets[i] + VkDeviceSize(y) * image.width() * 16, converted.constScanLine(y),
                            size_t(image.width()) * 16);
            uploads.push_back({offsets[i], offsets[i], sizes[i]});
            report.uploadedBytes += sizes[i];
        }
        host.synchronize(true);
        for (size_t pass = 0; pass < passes.size(); ++pass) {
            const int i = passes[pass];
            const auto &n = nodes[i];
            const int aux = n.auxiliary < 0 ? n.input : n.auxiliary;
            const std::array<VkDescriptorBufferInfo, 3> buffers{{{buffer, offsets[n.input], sizes[n.input]},
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
        CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        for (size_t pass = 0; pass < passes.size(); ++pass) {
            const auto &n = nodes[passes[pass]];
            const Parameters params{uint32_t(n.size.width()), uint32_t(n.size.height()), uint32_t(n.operation),
                                    n.quantization, n.parameters};
            CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSets[pass],
                                  0, nullptr);
            CmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
            uint32_t workWidth = params.width, workHeight = params.height;
            if (n.operation == GpuOp::BoxBlur) {
                if (n.parameters[1] == 0)
                    workWidth = (workWidth + 127) / 128;
                else
                    workHeight = (workHeight + 127) / 128;
            }
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
            sourceKeys[root] = nodes[sources[root]].image.cacheKey();
        QImage result(nodes[output].size, QImage::Format_RGBA32FPx4_Premultiplied);
        if (result.isNull())
            throw std::runtime_error("Could not allocate GPU readback");
        for (int y = 0; y < result.height(); ++y)
            std::memcpy(result.scanLine(y), mapped + offsets[output] + VkDeviceSize(y) * result.width() * 16,
                        size_t(result.width()) * 16);
        if (!sources.empty()) {
            const auto &original = nodes[sources.front()].image;
            result.setColorSpace(original.colorSpace());
            result.setDotsPerMeterX(original.dotsPerMeterX());
            result.setDotsPerMeterY(original.dotsPerMeterY());
            for (const auto &key : original.textKeys())
                result.setText(key, original.text(key));
        }
        result = result.convertToFormat(format);
        if (result.isNull())
            throw std::runtime_error("Could not convert GPU result");
        report.accelerated = true;
        report.downloadedBytes = sizes[output];
        report.workingBytes = capacity * (shared ? 1 : 2);
        report.dispatches = passes.size();
        return result;
    } catch (const std::exception &e) {
        sourceKeys.assign(sourceKeys.size(), 0);
        report.reason = QString::fromUtf8(e.what());
        return {};
    }
}
#undef INSTANCE_FUNCTIONS
#undef DEVICE_FUNCTIONS
std::unique_ptr<Context> &contextStorage() {
    static std::unique_ptr<Context> instance;
    return instance;
}
Context &context() {
    static const bool initialized = [] {
        contextStorage() = std::make_unique<Context>();
        // Drivers/validation layers can create lazy statics during rendering.
        // Release the device before their exit handlers, after preview workers finish.
        qAddPostRoutine([] {
            QThreadPool::globalInstance()->waitForDone();
            contextStorage().reset();
        });
        return true;
    }();
    Q_UNUSED(initialized)
    return *contextStorage();
}
} // namespace
QImage executeVulkanGraph(const GpuGraph &graph, int output, QImage::Format format, GpuExecution &report) {
    return context().render(graph, output, format, report);
}
QString vulkanDeviceDescription() { return context().description(); }
} // namespace compositor
#else
namespace compositor {
QImage executeVulkanGraph(const GpuGraph &, int, QImage::Format, GpuExecution &report) {
    report.reason = QStringLiteral("This build has no Vulkan support");
    return {};
}
QString vulkanDeviceDescription() { return QStringLiteral("This build has no Vulkan support"); }
} // namespace compositor
#endif
