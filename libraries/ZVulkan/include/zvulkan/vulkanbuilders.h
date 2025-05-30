#pragma once

#include "vulkanobjects.h"
#include <cassert>
#include <set>

class VulkanCompatibleDevice;

class VulkanInstanceBuilder
{
public:
	VulkanInstanceBuilder();

	VulkanInstanceBuilder& ApiVersionsToTry(const std::vector<uint32_t>& versions);
	VulkanInstanceBuilder& RequireExtension(const std::string& extensionName);
	VulkanInstanceBuilder& RequireExtensions(const std::vector<std::string>& extensions);
	VulkanInstanceBuilder& RequireExtensions(const std::vector<const char*>& extensions);
	VulkanInstanceBuilder& RequireExtensions(const char** extensions, size_t count);
	VulkanInstanceBuilder& OptionalExtension(const std::string& extensionName);
	VulkanInstanceBuilder& OptionalSwapchainColorspace();
	VulkanInstanceBuilder& DebugLayer(bool enable = true);

	std::shared_ptr<VulkanInstance> Create();

private:
	std::vector<uint32_t> apiVersionsToTry;
	std::set<std::string> requiredExtensions;
	std::set<std::string> optionalExtensions;
	bool debugLayer = false;
};

class VulkanDeviceBuilder
{
public:
	VulkanDeviceBuilder();

	VulkanDeviceBuilder& RequireExtension(const std::string& extensionName);
	VulkanDeviceBuilder& OptionalExtension(const std::string& extensionName);
	VulkanDeviceBuilder& OptionalRayQuery();
	VulkanDeviceBuilder& OptionalDescriptorIndexing();
	VulkanDeviceBuilder& Surface(std::shared_ptr<VulkanSurface> surface);
	VulkanDeviceBuilder& SelectDevice(int index);

	std::vector<VulkanCompatibleDevice> FindDevices(const std::shared_ptr<VulkanInstance>& instance);
	std::shared_ptr<VulkanDevice> Create(std::shared_ptr<VulkanInstance> instance);

private:
	std::set<std::string> requiredDeviceExtensions;
	std::set<std::string> optionalDeviceExtensions;
	std::shared_ptr<VulkanSurface> surface;
	int deviceIndex = 0;
};

class VulkanSwapChainBuilder
{
public:
	VulkanSwapChainBuilder();

	std::shared_ptr<VulkanSwapChain> Create(VulkanDevice* device);
};

class CommandPoolBuilder
{
public:
	CommandPoolBuilder();

	CommandPoolBuilder& QueueFamily(int index);
	CommandPoolBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanCommandPool> Create(VulkanDevice* device);

private:
	const char* debugName = nullptr;
	int queueFamilyIndex = -1;
};

class SemaphoreBuilder
{
public:
	SemaphoreBuilder();

	SemaphoreBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanSemaphore> Create(VulkanDevice* device);

private:
	const char* debugName = nullptr;
};

class FenceBuilder
{
public:
	FenceBuilder();

	FenceBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanFence> Create(VulkanDevice* device);

private:
	const char* debugName = nullptr;
};

class ImageBuilder
{
public:
	ImageBuilder();

	ImageBuilder& Type(VkImageType type);
	ImageBuilder& Flags(VkImageCreateFlags flags);
	ImageBuilder& Size(int width, int height, int miplevels = 1, int arrayLayers = 1);
	ImageBuilder& Samples(VkSampleCountFlagBits samples);
	ImageBuilder& Format(VkFormat format);
	ImageBuilder& Usage(VkImageUsageFlags imageUsage, VmaMemoryUsage memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY, VmaAllocationCreateFlags allocFlags = 0);
	ImageBuilder& MemoryType(VkMemoryPropertyFlags requiredFlags, VkMemoryPropertyFlags preferredFlags, uint32_t memoryTypeBits = 0);
	ImageBuilder& LinearTiling();
	ImageBuilder& DebugName(const char* name) { debugName = name; return *this; }

	bool IsFormatSupported(VulkanDevice *device, VkFormatFeatureFlags bufferFeatures = 0);

	std::unique_ptr<VulkanImage> Create(VulkanDevice *device, VkDeviceSize* allocatedBytes = nullptr);
	std::unique_ptr<VulkanImage> TryCreate(VulkanDevice *device);

private:
	VkImageCreateInfo imageInfo = {};
	VmaAllocationCreateInfo allocInfo = {};
	const char* debugName = nullptr;
};

class ImageViewBuilder
{
public:
	ImageViewBuilder();

	ImageViewBuilder& Type(VkImageViewType type);
	ImageViewBuilder& Image(VulkanImage *image, VkFormat format, VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, int mipLevel = 0, int arrayLayer = 0, int levelCount = 0, int layerCount = 0);
	ImageViewBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanImageView> Create(VulkanDevice *device);

private:
	VkImageViewCreateInfo viewInfo = {};
	const char* debugName = nullptr;
};

class SamplerBuilder
{
public:
	SamplerBuilder();

	SamplerBuilder& AddressMode(VkSamplerAddressMode addressMode);
	SamplerBuilder& AddressMode(VkSamplerAddressMode u, VkSamplerAddressMode v, VkSamplerAddressMode w);
	SamplerBuilder& MinFilter(VkFilter minFilter);
	SamplerBuilder& MagFilter(VkFilter magFilter);
	SamplerBuilder& MipmapMode(VkSamplerMipmapMode mode);
	SamplerBuilder& Anisotropy(float maxAnisotropy);
	SamplerBuilder& MipLodBias(float bias);
	SamplerBuilder& MaxLod(float value);
	SamplerBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanSampler> Create(VulkanDevice *device);

private:
	VkSamplerCreateInfo samplerInfo = {};
	const char* debugName = nullptr;
};

class BufferBuilder
{
public:
	BufferBuilder();

	BufferBuilder& Size(size_t size);
	BufferBuilder& Usage(VkBufferUsageFlags bufferUsage, VmaMemoryUsage memoryUsage = VMA_MEMORY_USAGE_GPU_ONLY, VmaAllocationCreateFlags allocFlags = 0);
	BufferBuilder& MemoryType(VkMemoryPropertyFlags requiredFlags, VkMemoryPropertyFlags preferredFlags, uint32_t memoryTypeBits = 0);
	BufferBuilder& MinAlignment(VkDeviceSize memoryAlignment);
	BufferBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanBuffer> Create(VulkanDevice *device);

private:
	VkBufferCreateInfo bufferInfo = {};
	VmaAllocationCreateInfo allocInfo = {};
	const char* debugName = nullptr;
	VkDeviceSize minAlignment = 0;
};

enum class ShaderType
{
	Vertex,
	TessControl,
	TessEvaluation,
	Geometry,
	Fragment,
	Compute
};

class ShaderIncludeResult
{
public:
	ShaderIncludeResult(std::string name, std::string text) : name(std::move(name)), text(std::move(text)) {}
	ShaderIncludeResult(std::string error) : text(std::move(error)) {}

	std::string name; // fully resolved name of the included header file
	std::string text; // the file contents - or include error message if name is empty
};

class GLSLCompiler
{
public:
	static void Init();
	static void Deinit();

	GLSLCompiler& Type(ShaderType type);
	GLSLCompiler& AddSource(const std::string& name, const std::string& code);

	GLSLCompiler& OnIncludeSystem(std::function<ShaderIncludeResult(std::string headerName, std::string includerName, size_t inclusionDepth)> onIncludeSystem);
	GLSLCompiler& OnIncludeLocal(std::function<ShaderIncludeResult(std::string headerName, std::string includerName, size_t inclusionDepth)> onIncludeLocal);

	std::vector<uint32_t> Compile(uint32_t apiVersion/* = VK_API_VERSION_1_0*/);
	std::vector<uint32_t> Compile(VulkanDevice* device);

private:
	std::vector<std::pair<std::string, std::string>> sources;
	std::function<ShaderIncludeResult(std::string headerName, std::string includerName, size_t inclusionDepth)> onIncludeSystem;
	std::function<ShaderIncludeResult(std::string headerName, std::string includerName, size_t inclusionDepth)> onIncludeLocal;
	int stage = 0;
	friend class GLSLCompilerIncluderImpl;
};

class AccelerationStructureBuilder
{
public:
	AccelerationStructureBuilder();

	AccelerationStructureBuilder& Type(VkAccelerationStructureTypeKHR type);
	AccelerationStructureBuilder& Buffer(VulkanBuffer* buffer, VkDeviceSize size);
	AccelerationStructureBuilder& Buffer(VulkanBuffer* buffer, VkDeviceSize offset, VkDeviceSize size);
	AccelerationStructureBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanAccelerationStructure> Create(VulkanDevice* device);

private:
	VkAccelerationStructureCreateInfoKHR createInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
	const char* debugName = nullptr;
};

class ComputePipelineBuilder
{
public:
	ComputePipelineBuilder();

	ComputePipelineBuilder& Cache(VulkanPipelineCache* cache);
	ComputePipelineBuilder& Layout(VulkanPipelineLayout *layout);
	ComputePipelineBuilder& ComputeShader(std::vector<uint32_t> spirv) { computeShader = std::move(spirv); return *this; }
	ComputePipelineBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanPipeline> Create(VulkanDevice *device);

private:
	VkComputePipelineCreateInfo pipelineInfo = {};
	VkPipelineShaderStageCreateInfo stageInfo = {};
	std::vector<uint32_t> computeShader;
	VulkanPipelineCache* cache = nullptr;
	const char* debugName = nullptr;
};

class DescriptorSetLayoutBuilder
{
public:
	DescriptorSetLayoutBuilder();

	DescriptorSetLayoutBuilder& Flags(VkDescriptorSetLayoutCreateFlags flags);
	DescriptorSetLayoutBuilder& AddBinding(int binding, VkDescriptorType type, int arrayCount, VkShaderStageFlags stageFlags, VkDescriptorBindingFlags flags = 0);
	DescriptorSetLayoutBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanDescriptorSetLayout> Create(VulkanDevice *device);

private:
	VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	VkDescriptorSetLayoutBindingFlagsCreateInfoEXT bindingFlagsInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT };
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	std::vector<VkDescriptorBindingFlags> bindingFlags;
	const char* debugName = nullptr;
};

class DescriptorPoolBuilder
{
public:
	DescriptorPoolBuilder();

	DescriptorPoolBuilder& Flags(VkDescriptorPoolCreateFlags flags);
	DescriptorPoolBuilder& MaxSets(int value);
	DescriptorPoolBuilder& AddPoolSize(VkDescriptorType type, int count);
	DescriptorPoolBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanDescriptorPool> Create(VulkanDevice *device);

private:
	std::vector<VkDescriptorPoolSize> poolSizes;
	VkDescriptorPoolCreateInfo poolInfo = {};
	const char* debugName = nullptr;
};

class QueryPoolBuilder
{
public:
	QueryPoolBuilder();

	QueryPoolBuilder& QueryType(VkQueryType type, int count, VkQueryPipelineStatisticFlags pipelineStatistics = 0);
	QueryPoolBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanQueryPool> Create(VulkanDevice *device);

private:
	VkQueryPoolCreateInfo poolInfo = {};
	const char* debugName = nullptr;
};

class FramebufferBuilder
{
public:
	FramebufferBuilder();

	FramebufferBuilder& RenderPass(VulkanRenderPass *renderPass);
	FramebufferBuilder& AddAttachment(VulkanImageView *view);
	FramebufferBuilder& AddAttachment(VkImageView view);
	FramebufferBuilder& Size(int width, int height, int layers = 1);
	FramebufferBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanFramebuffer> Create(VulkanDevice *device);

private:
	VkFramebufferCreateInfo framebufferInfo = {};
	std::vector<VkImageView> attachments;
	const char* debugName = nullptr;
};

class ColorBlendAttachmentBuilder
{
public:
	ColorBlendAttachmentBuilder();

	ColorBlendAttachmentBuilder& ColorWriteMask(VkColorComponentFlags mask);
	ColorBlendAttachmentBuilder& AdditiveBlendMode();
	ColorBlendAttachmentBuilder& AlphaBlendMode();
	ColorBlendAttachmentBuilder& BlendMode(VkBlendOp op, VkBlendFactor src, VkBlendFactor dst);

	VkPipelineColorBlendAttachmentState Create() { return colorBlendAttachment; }

private:
	VkPipelineColorBlendAttachmentState colorBlendAttachment = { };
};

class GraphicsPipelineBuilder
{
public:
	GraphicsPipelineBuilder();

	GraphicsPipelineBuilder& Cache(VulkanPipelineCache* cache);
	GraphicsPipelineBuilder& Subpass(int subpass);
	GraphicsPipelineBuilder& Layout(VulkanPipelineLayout *layout);
	GraphicsPipelineBuilder& RenderPass(VulkanRenderPass *renderPass);
	GraphicsPipelineBuilder& Topology(VkPrimitiveTopology topology);
	GraphicsPipelineBuilder& Viewport(float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f);
	GraphicsPipelineBuilder& Scissor(int x, int y, int width, int height);
	GraphicsPipelineBuilder& RasterizationSamples(VkSampleCountFlagBits samples);

	GraphicsPipelineBuilder& Cull(VkCullModeFlags cullMode, VkFrontFace frontFace);
	GraphicsPipelineBuilder& DepthStencilEnable(bool test, bool write, bool stencil);
	GraphicsPipelineBuilder& DepthFunc(VkCompareOp func);
	GraphicsPipelineBuilder& DepthClampEnable(bool value);
	GraphicsPipelineBuilder& DepthBias(bool enable, float biasConstantFactor, float biasClamp, float biasSlopeFactor);
	GraphicsPipelineBuilder& Stencil(VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp, VkCompareOp compareOp, uint32_t compareMask, uint32_t writeMask, uint32_t reference);

	GraphicsPipelineBuilder& AddColorBlendAttachment(VkPipelineColorBlendAttachmentState state);

	GraphicsPipelineBuilder& AddVertexShader(std::vector<uint32_t> spirv);
	GraphicsPipelineBuilder& AddFragmentShader(std::vector<uint32_t> spirv);

	GraphicsPipelineBuilder& AddConstant(uint32_t constantID, const void* data, size_t size);
	GraphicsPipelineBuilder& AddConstant(uint32_t constantID, uint32_t value);
	GraphicsPipelineBuilder& AddConstant(uint32_t constantID, int32_t value);
	GraphicsPipelineBuilder& AddConstant(uint32_t constantID, float value);

	GraphicsPipelineBuilder& AddVertexBufferBinding(int index, size_t stride);
	GraphicsPipelineBuilder& AddVertexAttribute(int location, int binding, VkFormat format, size_t offset);
	
	GraphicsPipelineBuilder& AddDynamicState(VkDynamicState state);

	GraphicsPipelineBuilder& PolygonMode(VkPolygonMode mode) {rasterizer.polygonMode = mode; return *this;};

	GraphicsPipelineBuilder& Flags(VkPipelineCreateFlags flags);
	GraphicsPipelineBuilder& LibraryFlags(VkGraphicsPipelineLibraryFlagsEXT flags);
	GraphicsPipelineBuilder& AddLibrary(VulkanPipeline* pipeline);

	GraphicsPipelineBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanPipeline> Create(VulkanDevice *device);

private:
	VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	VkPipelineVertexInputStateCreateInfo vertexInputInfo = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	VkViewport viewport = { };
	VkRect2D scissor = { };
	VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo rasterizer = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo multisampling = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	VkPipelineColorBlendStateCreateInfo colorBlending = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };

	VkPipelineLibraryCreateInfoKHR libraryCreate = { VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR };
	VkGraphicsPipelineLibraryCreateInfoEXT pipelineLibrary = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT };

	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
	std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
	std::vector<VkVertexInputBindingDescription> vertexInputBindings;
	std::vector<VkVertexInputAttributeDescription> vertexInputAttributes;
	std::vector<VkDynamicState> dynamicStates;
	std::vector<VkPipeline> libraries;
	std::vector<std::vector<uint32_t>> shaderCode;

	struct ShaderSpecialization
	{
		VkSpecializationInfo info = {};
		std::vector<VkSpecializationMapEntry> entries;
		std::vector<uint8_t> data;
	};

	std::vector<std::unique_ptr<ShaderSpecialization>> specializations;

	VulkanPipelineCache* cache = nullptr;
	const char* debugName = nullptr;
};

class PipelineLayoutBuilder
{
public:
	PipelineLayoutBuilder();

	PipelineLayoutBuilder& AddSetLayout(VulkanDescriptorSetLayout *setLayout);
	PipelineLayoutBuilder& AddPushConstantRange(VkShaderStageFlags stageFlags, size_t offset, size_t size);

	PipelineLayoutBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanPipelineLayout> Create(VulkanDevice *device);

private:
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
	std::vector<VkDescriptorSetLayout> setLayouts;
	std::vector<VkPushConstantRange> pushConstantRanges;
	const char* debugName = nullptr;
};

class PipelineCacheBuilder
{
public:
	PipelineCacheBuilder();

	PipelineCacheBuilder& InitialData(const void* data, size_t size);
	PipelineCacheBuilder& Flags(VkPipelineCacheCreateFlags flags);

	PipelineCacheBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanPipelineCache> Create(VulkanDevice* device);

private:
	VkPipelineCacheCreateInfo pipelineCacheInfo = {};
	std::vector<uint8_t> initData;
	const char* debugName = nullptr;
};

class RenderPassBuilder
{
public:
	RenderPassBuilder();

	RenderPassBuilder& AddAttachment(VkFormat format, VkSampleCountFlagBits samples, VkAttachmentLoadOp load, VkAttachmentStoreOp store, VkImageLayout initialLayout, VkImageLayout finalLayout);
	RenderPassBuilder& AddDepthStencilAttachment(VkFormat format, VkSampleCountFlagBits samples, VkAttachmentLoadOp load, VkAttachmentStoreOp store, VkAttachmentLoadOp stencilLoad, VkAttachmentStoreOp stencilStore, VkImageLayout initialLayout, VkImageLayout finalLayout);

	RenderPassBuilder& AddExternalSubpassDependency(VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask);

	RenderPassBuilder& AddSubpass();
	RenderPassBuilder& AddSubpassColorAttachmentRef(uint32_t index, VkImageLayout layout);
	RenderPassBuilder& AddSubpassDepthStencilAttachmentRef(uint32_t index, VkImageLayout layout);

	RenderPassBuilder& DebugName(const char* name) { debugName = name; return *this; }

	std::unique_ptr<VulkanRenderPass> Create(VulkanDevice *device);

private:
	VkRenderPassCreateInfo renderPassInfo = { };

	std::vector<VkAttachmentDescription> attachments;
	std::vector<VkSubpassDependency> dependencies;
	std::vector<VkSubpassDescription> subpasses;

	struct SubpassData
	{
		std::vector<VkAttachmentReference> colorRefs;
		VkAttachmentReference depthRef = { };
	};

	std::vector<std::unique_ptr<SubpassData>> subpassData;

	const char* debugName = nullptr;
};

class PipelineBarrier
{
public:
	PipelineBarrier& AddMemory(VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask);
	PipelineBarrier& AddBuffer(VulkanBuffer *buffer, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask);
	PipelineBarrier& AddBuffer(VulkanBuffer *buffer, VkDeviceSize offset, VkDeviceSize size, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask);
	PipelineBarrier& AddImage(VulkanImage *image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask, VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, int baseMipLevel = 0, int levelCount = 1, int baseArrayLayer = 0, int layerCount = 1);
	PipelineBarrier& AddImage(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask, VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, int baseMipLevel = 0, int levelCount = 1, int baseArrayLayer = 0, int layerCount = 1);
	PipelineBarrier& AddQueueTransfer(int srcFamily, int dstFamily, VulkanBuffer *buffer, VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask);
	PipelineBarrier& AddQueueTransfer(int srcFamily, int dstFamily, VulkanImage *image, VkImageLayout layout, VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, int baseMipLevel = 0, int levelCount = 1);

	void Execute(VulkanCommandBuffer *commandBuffer, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags = 0);

private:
	std::vector<VkMemoryBarrier> memoryBarriers;
	std::vector<VkBufferMemoryBarrier> bufferMemoryBarriers;
	std::vector<VkImageMemoryBarrier> imageMemoryBarriers;
};

class QueueSubmit
{
public:
	QueueSubmit();

	QueueSubmit& AddCommandBuffer(VulkanCommandBuffer *buffer);
	QueueSubmit& AddWait(VkPipelineStageFlags waitStageMask, VulkanSemaphore *semaphore);
	QueueSubmit& AddSignal(VulkanSemaphore *semaphore);
	void Execute(VulkanDevice *device, VkQueue queue, VulkanFence *fence = nullptr);

private:
	VkSubmitInfo submitInfo = {};
	std::vector<VkSemaphore> waitSemaphores;
	std::vector<VkPipelineStageFlags> waitStages;
	std::vector<VkSemaphore> signalSemaphores;
	std::vector<VkCommandBuffer> commandBuffers;
};

class WriteDescriptors
{
public:
	WriteDescriptors& AddBuffer(VulkanDescriptorSet *descriptorSet, int binding, VkDescriptorType type, VulkanBuffer *buffer);
	WriteDescriptors& AddBuffer(VulkanDescriptorSet *descriptorSet, int binding, VkDescriptorType type, VulkanBuffer *buffer, size_t offset, size_t range);
	WriteDescriptors& AddStorageImage(VulkanDescriptorSet *descriptorSet, int binding, VulkanImageView *view, VkImageLayout imageLayout);
	WriteDescriptors& AddCombinedImageSampler(VulkanDescriptorSet *descriptorSet, int binding, VulkanImageView *view, VulkanSampler *sampler, VkImageLayout imageLayout);
	WriteDescriptors& AddCombinedImageSampler(VulkanDescriptorSet* descriptorSet, int binding, int arrayIndex, VulkanImageView* view, VulkanSampler* sampler, VkImageLayout imageLayout);
	WriteDescriptors& AddAccelerationStructure(VulkanDescriptorSet* descriptorSet, int binding, VulkanAccelerationStructure* accelStruct);
	void Execute(VulkanDevice *device);

private:
	struct WriteExtra
	{
		VkDescriptorImageInfo imageInfo;
		VkDescriptorBufferInfo bufferInfo;
		VkBufferView bufferView;
		VkWriteDescriptorSetAccelerationStructureKHR accelStruct;
	};

	std::vector<VkWriteDescriptorSet> writes;
	std::vector<std::unique_ptr<WriteExtra>> writeExtras;
};

class BufferTransfer
{
public:
	BufferTransfer& AddBuffer(VulkanBuffer* buffer, size_t offset, const void* data, size_t size);
	BufferTransfer& AddBuffer(VulkanBuffer* buffer, const void* data, size_t size);
	BufferTransfer& AddBuffer(VulkanBuffer* buffer, const void* data0, size_t size0, const void* data1, size_t size1);
	std::unique_ptr<VulkanBuffer> Execute(VulkanDevice* device, VulkanCommandBuffer* cmdbuffer);

private:
	struct BufferCopy
	{
		VulkanBuffer* buffer;
		size_t offset;
		const void* data0;
		size_t size0;
		const void* data1;
		size_t size1;
	};
	std::vector<BufferCopy> bufferCopies;
};
