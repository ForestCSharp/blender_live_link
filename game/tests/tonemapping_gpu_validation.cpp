#define VK_NO_PROTOTYPES
#include "volk/volk.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "tonemapping_validation_common.h"

using TonemappingValidation::Assets;
using TonemappingValidation::Float4;
using TonemappingValidation::RGB;

static void vk_check(VkResult result, const char* operation)
{
	if (result != VK_SUCCESS)
		throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
}

static bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
	for (const VkExtensionProperties& extension : extensions)
		if (std::strcmp(extension.extensionName, name) == 0) return true;
	return false;
}

struct Buffer
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* mapped = nullptr;
	VkDeviceSize size = 0;
	bool coherent = false;
};

struct Image
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
};

struct ConfigurationResult
{
	std::string name;
	float maximum_error = 0.0f;
	double mean_error = 0.0;
	size_t samples = 0;
};

class VulkanHarness
{
public:
	void initialize()
	{
		vk_check(volkInitialize(), "volkInitialize");
		uint32_t instance_extension_count = 0;
		vk_check(vkEnumerateInstanceExtensionProperties(
			nullptr, &instance_extension_count, nullptr), "enumerate instance extensions");
		std::vector<VkExtensionProperties> instance_extensions(instance_extension_count);
		vk_check(vkEnumerateInstanceExtensionProperties(
			nullptr, &instance_extension_count, instance_extensions.data()), "enumerate instance extensions");
		std::vector<const char*> enabled_instance_extensions;
		VkInstanceCreateFlags instance_flags = 0;
		if (has_extension(instance_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
		{
			enabled_instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		}
		VkApplicationInfo application_info = {
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.pApplicationName = "Game2 Tonemapping GPU Validation",
			.applicationVersion = 1,
			.pEngineName = "Game2",
			.engineVersion = 1,
			.apiVersion = VK_API_VERSION_1_2,
		};
		VkInstanceCreateInfo instance_info = {
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.flags = instance_flags,
			.pApplicationInfo = &application_info,
			.enabledExtensionCount = (uint32_t)enabled_instance_extensions.size(),
			.ppEnabledExtensionNames = enabled_instance_extensions.data(),
		};
		vk_check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance");
		volkLoadInstance(instance);

		uint32_t physical_device_count = 0;
		vk_check(vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr),
			"enumerate physical devices");
		if (physical_device_count == 0) throw std::runtime_error("no Vulkan physical device available");
		std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
		vk_check(vkEnumeratePhysicalDevices(instance, &physical_device_count, physical_devices.data()),
			"enumerate physical devices");
		for (VkPhysicalDevice candidate : physical_devices)
		{
			uint32_t family_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
			std::vector<VkQueueFamilyProperties> families(family_count);
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
			for (uint32_t family = 0; family < family_count; ++family)
			{
				if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) continue;
				VkFormatProperties format_properties = {};
				vkGetPhysicalDeviceFormatProperties(
					candidate, VK_FORMAT_R16G16B16A16_SFLOAT, &format_properties);
				if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0
					|| (format_properties.optimalTilingFeatures
						& VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) continue;
				physical_device = candidate;
				queue_family = family;
				break;
			}
			if (physical_device != VK_NULL_HANDLE) break;
		}
		if (physical_device == VK_NULL_HANDLE)
			throw std::runtime_error("no Vulkan compute device supports linear RGBA16F sampling");

		VkPhysicalDeviceProperties properties = {};
		vkGetPhysicalDeviceProperties(physical_device, &properties);
		device_name = properties.deviceName;
		if (properties.limits.maxImageArrayLayers < TONEMAP_LUT_LAYER_COUNT)
			throw std::runtime_error("device does not support the 192-layer tonemapping LUT");
		vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

		uint32_t device_extension_count = 0;
		vk_check(vkEnumerateDeviceExtensionProperties(
			physical_device, nullptr, &device_extension_count, nullptr), "enumerate device extensions");
		std::vector<VkExtensionProperties> device_extensions(device_extension_count);
		vk_check(vkEnumerateDeviceExtensionProperties(
			physical_device, nullptr, &device_extension_count, device_extensions.data()),
			"enumerate device extensions");
		std::vector<const char*> enabled_device_extensions;
		if (has_extension(device_extensions, "VK_KHR_portability_subset"))
			enabled_device_extensions.push_back("VK_KHR_portability_subset");
		const float queue_priority = 1.0f;
		VkDeviceQueueCreateInfo queue_info = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = queue_family,
			.queueCount = 1,
			.pQueuePriorities = &queue_priority,
		};
		VkDeviceCreateInfo device_info = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.queueCreateInfoCount = 1,
			.pQueueCreateInfos = &queue_info,
			.enabledExtensionCount = (uint32_t)enabled_device_extensions.size(),
			.ppEnabledExtensionNames = enabled_device_extensions.data(),
		};
		vk_check(vkCreateDevice(physical_device, &device_info, nullptr, &device), "vkCreateDevice");
		volkLoadDevice(device);
		vkGetDeviceQueue(device, queue_family, 0, &queue);

		VkCommandPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = queue_family,
		};
		vk_check(vkCreateCommandPool(device, &pool_info, nullptr, &command_pool), "create command pool");
		VkCommandBufferAllocateInfo command_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = command_pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		vk_check(vkAllocateCommandBuffers(device, &command_info, &command_buffer),
			"allocate command buffer");
		create_descriptors_and_pipelines();
	}

	void create_io(const std::vector<Float4>& corpus)
	{
		input = create_buffer(corpus.size() * sizeof(Float4),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		output = create_buffer(corpus.size() * sizeof(Float4),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		std::memcpy(input.mapped, corpus.data(), input.size);
		flush(input);
		VkDescriptorBufferInfo input_info = {input.buffer, 0, input.size};
		VkDescriptorBufferInfo output_info = {output.buffer, 0, output.size};
		VkWriteDescriptorSet writes[2] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &input_info,
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 1,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &output_info,
			},
		};
		vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
	}

	void upload_lut(const Assets& assets)
	{
		destroy_image(lut);
		const size_t layer_texel_count =
			(size_t)TONEMAP_LUT_RESOLUTION * TONEMAP_LUT_RESOLUTION * 4;
		std::vector<u16> packed(layer_texel_count * TONEMAP_LUT_LAYER_COUNT);
		assert(assets.gt7.length() == layer_texel_count * TONEMAP_LUT_RESOLUTION);
		assert(assets.aces2.pixels.length() == assets.gt7.length());
		assert(assets.agx.pixels.length() == assets.gt7.length());
		std::memcpy(packed.data(), assets.gt7.data(), assets.gt7.length() * sizeof(u16));
		std::memcpy(packed.data() + assets.gt7.length(),
			assets.aces2.pixels.data(), assets.aces2.pixels.length() * sizeof(u16));
		std::memcpy(packed.data() + assets.gt7.length() + assets.aces2.pixels.length(),
			assets.agx.pixels.data(), assets.agx.pixels.length() * sizeof(u16));

		Buffer staging = create_buffer(packed.size() * sizeof(u16),
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
		std::memcpy(staging.mapped, packed.data(), staging.size);
		flush(staging);

		VkImageCreateInfo image_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.extent = {TONEMAP_LUT_RESOLUTION, TONEMAP_LUT_RESOLUTION, 1},
			.mipLevels = 1,
			.arrayLayers = TONEMAP_LUT_LAYER_COUNT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		vk_check(vkCreateImage(device, &image_info, nullptr, &lut.image), "create LUT image");
		VkMemoryRequirements requirements = {};
		vkGetImageMemoryRequirements(device, lut.image, &requirements);
		VkMemoryAllocateInfo memory_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = requirements.size,
			.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
		};
		vk_check(vkAllocateMemory(device, &memory_info, nullptr, &lut.memory), "allocate LUT memory");
		vk_check(vkBindImageMemory(device, lut.image, lut.memory, 0), "bind LUT memory");

		begin_commands();
		VkImageMemoryBarrier to_transfer = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = lut.image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, TONEMAP_LUT_LAYER_COUNT},
		};
		vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);
		VkBufferImageCopy copy = {
			.bufferOffset = 0,
			.bufferRowLength = 0,
			.bufferImageHeight = 0,
			.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, TONEMAP_LUT_LAYER_COUNT},
			.imageOffset = {0, 0, 0},
			.imageExtent = {TONEMAP_LUT_RESOLUTION, TONEMAP_LUT_RESOLUTION, 1},
		};
		vkCmdCopyBufferToImage(command_buffer, staging.buffer, lut.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		VkImageMemoryBarrier to_shader = to_transfer;
		to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_shader);
		end_commands();
		destroy_buffer(staging);

		VkImageViewCreateInfo view_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = lut.image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, TONEMAP_LUT_LAYER_COUNT},
		};
		vk_check(vkCreateImageView(device, &view_info, nullptr, &lut.view), "create LUT view");
		VkDescriptorImageInfo descriptor_image = {
			.sampler = sampler,
			.imageView = lut.view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = descriptor_set,
			.dstBinding = 2,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &descriptor_image,
		};
		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
	}

	std::vector<Float4> dispatch(int method, float scale, size_t sample_count, bool matched_gray)
	{
		struct PushConstants { int method; int count; float scale; int padding; };
		const PushConstants constants = {method, (int)sample_count, scale, 0};
		begin_commands();
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			matched_gray ? matched_pipeline : exact_pipeline);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
		vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
			0, sizeof(constants), &constants);
		vkCmdDispatch(command_buffer, (uint32_t)((sample_count + 63) / 64), 1, 1);
		VkMemoryBarrier barrier = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		};
		vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		end_commands();
		invalidate(output);
		const Float4* values = (const Float4*)output.mapped;
		return std::vector<Float4>(values, values + sample_count);
	}

	std::vector<Float4> dispatch_display(
		int output_mode, int sdr_attachment_is_srgb, size_t sample_count)
	{
		struct PushConstants { int mode; int count; int srgb; int padding; };
		const PushConstants constants = {
			output_mode, (int)sample_count, sdr_attachment_is_srgb, 0};
		begin_commands();
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, display_pipeline);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
		vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
			0, sizeof(constants), &constants);
		vkCmdDispatch(command_buffer, (uint32_t)((sample_count + 63) / 64), 1, 1);
		VkMemoryBarrier barrier = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		};
		vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		end_commands();
		invalidate(output);
		const Float4* values = (const Float4*)output.mapped;
		return std::vector<Float4>(values, values + sample_count);
	}

	const std::string& gpu_name() const { return device_name; }

	void shutdown()
	{
		if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
		destroy_image(lut);
		destroy_buffer(output);
		destroy_buffer(input);
		if (exact_pipeline) vkDestroyPipeline(device, exact_pipeline, nullptr);
		if (matched_pipeline) vkDestroyPipeline(device, matched_pipeline, nullptr);
		if (display_pipeline) vkDestroyPipeline(device, display_pipeline, nullptr);
		if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
		if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
		if (descriptor_layout) vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
		if (sampler) vkDestroySampler(device, sampler, nullptr);
		if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
		if (device) vkDestroyDevice(device, nullptr);
		if (instance) vkDestroyInstance(instance, nullptr);
	}

private:
	uint32_t find_memory_type(
		uint32_t type_bits,
		VkMemoryPropertyFlags required,
		VkMemoryPropertyFlags preferred) const
	{
		for (uint32_t pass = 0; pass < 2; ++pass)
			for (uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index)
			{
				if ((type_bits & (1u << index)) == 0) continue;
				const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[index].propertyFlags;
				if ((flags & required) != required) continue;
				if (pass == 0 && (flags & preferred) != preferred) continue;
				return index;
			}
		throw std::runtime_error("no compatible Vulkan memory type");
	}

	Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible)
	{
		Buffer result;
		result.size = size;
		VkBufferCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = size,
			.usage = usage,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		vk_check(vkCreateBuffer(device, &info, nullptr, &result.buffer), "create buffer");
		VkMemoryRequirements requirements = {};
		vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
		const VkMemoryPropertyFlags required = host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0;
		const VkMemoryPropertyFlags preferred = host_visible
			? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		const uint32_t memory_type = find_memory_type(requirements.memoryTypeBits, required, preferred);
		const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[memory_type].propertyFlags;
		result.coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
		VkMemoryAllocateInfo allocation = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = requirements.size,
			.memoryTypeIndex = memory_type,
		};
		vk_check(vkAllocateMemory(device, &allocation, nullptr, &result.memory), "allocate buffer memory");
		vk_check(vkBindBufferMemory(device, result.buffer, result.memory, 0), "bind buffer memory");
		if (host_visible)
			vk_check(vkMapMemory(device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped), "map buffer");
		return result;
	}

	void flush(const Buffer& buffer)
	{
		if (buffer.coherent) return;
		VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr,
			buffer.memory, 0, VK_WHOLE_SIZE};
		vk_check(vkFlushMappedMemoryRanges(device, 1, &range), "flush buffer");
	}

	void invalidate(const Buffer& buffer)
	{
		if (buffer.coherent) return;
		VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr,
			buffer.memory, 0, VK_WHOLE_SIZE};
		vk_check(vkInvalidateMappedMemoryRanges(device, 1, &range), "invalidate buffer");
	}

	void destroy_buffer(Buffer& buffer)
	{
		if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
		if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
		if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
		buffer = {};
	}

	void destroy_image(Image& image)
	{
		if (image.view) vkDestroyImageView(device, image.view, nullptr);
		if (image.image) vkDestroyImage(device, image.image, nullptr);
		if (image.memory) vkFreeMemory(device, image.memory, nullptr);
		image = {};
	}

	void begin_commands()
	{
		vk_check(vkResetCommandBuffer(command_buffer, 0), "reset command buffer");
		VkCommandBufferBeginInfo begin = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		vk_check(vkBeginCommandBuffer(command_buffer, &begin), "begin command buffer");
	}

	void end_commands()
	{
		vk_check(vkEndCommandBuffer(command_buffer), "end command buffer");
		VkSubmitInfo submit = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.commandBufferCount = 1,
			.pCommandBuffers = &command_buffer,
		};
		vk_check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "queue submit");
		vk_check(vkQueueWaitIdle(queue), "queue wait idle");
	}

	std::vector<uint32_t> read_spirv(const char* path)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file.good()) throw std::runtime_error(std::string("could not open ") + path);
		const std::streamsize bytes = file.tellg();
		if (bytes <= 0 || bytes % 4 != 0) throw std::runtime_error("invalid SPIR-V byte size");
		std::vector<uint32_t> words((size_t)bytes / 4);
		file.seekg(0);
		file.read((char*)words.data(), bytes);
		return words;
	}

	VkPipeline create_pipeline(const char* path)
	{
		const std::vector<uint32_t> words = read_spirv(path);
		VkShaderModuleCreateInfo module_info = {
			.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			.codeSize = words.size() * sizeof(uint32_t),
			.pCode = words.data(),
		};
		VkShaderModule module = VK_NULL_HANDLE;
		vk_check(vkCreateShaderModule(device, &module_info, nullptr, &module), "create shader module");
		VkPipelineShaderStageCreateInfo stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module,
			.pName = "main",
		};
		VkComputePipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = stage,
			.layout = pipeline_layout,
		};
		VkPipeline pipeline = VK_NULL_HANDLE;
		vk_check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
			&pipeline_info, nullptr, &pipeline), "create compute pipeline");
		vkDestroyShaderModule(device, module, nullptr);
		return pipeline;
	}

	void create_descriptors_and_pipelines()
	{
		VkSamplerCreateInfo sampler_info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.maxLod = 0.0f,
		};
		vk_check(vkCreateSampler(device, &sampler_info, nullptr, &sampler), "create sampler");
		VkDescriptorSetLayoutBinding bindings[3] = {
			{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
			{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		};
		VkDescriptorSetLayoutCreateInfo layout_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 3,
			.pBindings = bindings,
		};
		vk_check(vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &descriptor_layout),
			"create descriptor layout");
		VkDescriptorPoolSize pool_sizes[2] = {
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
		};
		VkDescriptorPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 1,
			.poolSizeCount = 2,
			.pPoolSizes = pool_sizes,
		};
		vk_check(vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool),
			"create descriptor pool");
		VkDescriptorSetAllocateInfo set_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = descriptor_pool,
			.descriptorSetCount = 1,
			.pSetLayouts = &descriptor_layout,
		};
		vk_check(vkAllocateDescriptorSets(device, &set_info, &descriptor_set),
			"allocate descriptor set");
		VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
		VkPipelineLayoutCreateInfo pipeline_layout_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &descriptor_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_range,
		};
		vk_check(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout),
			"create pipeline layout");
		exact_pipeline = create_pipeline("bin/shaders/tonemapping_validation.comp.spv");
		matched_pipeline = create_pipeline("bin/shaders/tonemapping_validation_match_gray.comp.spv");
		display_pipeline = create_pipeline("bin/shaders/display_encoding_validation.comp.spv");
	}

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memory_properties = {};
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
	VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline exact_pipeline = VK_NULL_HANDLE;
	VkPipeline matched_pipeline = VK_NULL_HANDLE;
	VkPipeline display_pipeline = VK_NULL_HANDLE;
	Buffer input;
	Buffer output;
	Image lut;
	std::string device_name;
};

static const char* method_name(int method)
{
	const char* names[] = {"gt7", "agx", "aces2", "khronos_pbr_neutral"};
	return names[method];
}

static const char* output_name(int output_mode)
{
	return output_mode == 0 ? "sdr" : output_mode == 1 ? "edr" : "hdr10";
}

static ConfigurationResult compare(
	const std::vector<Float4>& corpus,
	const std::vector<Float4>& gpu,
	const Assets& assets,
	int method,
	int output_mode,
	bool matched_gray)
{
	ConfigurationResult result;
	result.name = std::string(method_name(method)) + "/" + output_name(output_mode);
	if (method == TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL)
		result.name += matched_gray ? "/matched-gray" : "/exact";
	result.samples = corpus.size();
	double error_sum = 0.0;
	for (size_t index = 0; index < corpus.size(); ++index)
	{
		const Float4& input = corpus[index];
		const RGB expected = TonemappingValidation::sample_cpu(
			assets, method, output_mode, {input.r, input.g, input.b}, matched_gray);
		const float errors[] = {
			std::abs(gpu[index].r - expected.r),
			std::abs(gpu[index].g - expected.g),
			std::abs(gpu[index].b - expected.b),
		};
		assert(std::isfinite(gpu[index].r) && std::isfinite(gpu[index].g)
			&& std::isfinite(gpu[index].b));
		for (float error : errors)
		{
			result.maximum_error = std::max(result.maximum_error, error);
			error_sum += error;
		}
	}
	result.mean_error = error_sum / (double)(corpus.size() * 3);
	const float threshold = method == TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL ? 2e-5f : 0.003f;
	if (result.maximum_error > threshold)
		throw std::runtime_error(result.name + " GPU error " + std::to_string(result.maximum_error)
			+ " exceeds " + std::to_string(threshold));
	return result;
}

static float fractf(float value)
{
	return value - std::floor(value);
}

static float linear_to_srgb(float value)
{
	value = std::clamp(value, 0.0f, 1.0f);
	return value < 0.0031308f
		? value * 12.92f
		: 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

static float srgb_to_linear(float value)
{
	value = std::clamp(value, 0.0f, 1.0f);
	return value < 0.04045f
		? value / 12.92f
		: std::pow((value + 0.055f) / 1.055f, 2.4f);
}

static float st2084(float nits)
{
	constexpr float m1 = 0.1593017578125f;
	constexpr float m2 = 78.84375f;
	constexpr float c1 = 0.8359375f;
	constexpr float c2 = 18.8515625f;
	constexpr float c3 = 18.6875f;
	const float l = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
	const float lm = std::pow(l, m1);
	return std::pow((c1 + c2 * lm) / (1.0f + c3 * lm), m2);
}

static RGB display_reference(RGB scene, int output_mode, int srgb_attachment, size_t index)
{
	if (output_mode == 0)
	{
		const float x = (float)(index % 1024);
		const float y = (float)(index / 1024);
		const float noise = fractf(52.9829189f * fractf(x * 0.06711056f + y * 0.00583715f));
		const float dither = (noise - 0.5f) / 255.0f;
		RGB encoded = {
			std::clamp(linear_to_srgb(scene.r) + dither, 0.0f, 1.0f),
			std::clamp(linear_to_srgb(scene.g) + dither, 0.0f, 1.0f),
			std::clamp(linear_to_srgb(scene.b) + dither, 0.0f, 1.0f),
		};
		if (srgb_attachment)
			encoded = {srgb_to_linear(encoded.r), srgb_to_linear(encoded.g), srgb_to_linear(encoded.b)};
		return encoded;
	}
	if (output_mode == 1)
		return {
			std::max(scene.r, 0.0f) * (1000.0f / 203.0f),
			std::max(scene.g, 0.0f) * (1000.0f / 203.0f),
			std::max(scene.b, 0.0f) * (1000.0f / 203.0f),
		};
	const RGB rec2020 = {
		0.6274039f * scene.r + 0.3292830f * scene.g + 0.0433131f * scene.b,
		0.0690973f * scene.r + 0.9195404f * scene.g + 0.0113623f * scene.b,
		0.0163914f * scene.r + 0.0880133f * scene.g + 0.8955953f * scene.b,
	};
	return {
		st2084(std::max(rec2020.r, 0.0f) * 1000.0f),
		st2084(std::max(rec2020.g, 0.0f) * 1000.0f),
		st2084(std::max(rec2020.b, 0.0f) * 1000.0f),
	};
}

static ConfigurationResult compare_display(
	const std::vector<Float4>& corpus,
	const std::vector<Float4>& gpu,
	int output_mode,
	int srgb_attachment)
{
	ConfigurationResult result;
	result.name = std::string("display/") + output_name(output_mode);
	if (output_mode == 0) result.name += srgb_attachment ? "/srgb-attachment" : "/unorm-attachment";
	result.samples = corpus.size();
	double error_sum = 0.0;
	for (size_t index = 0; index < corpus.size(); ++index)
	{
		const Float4& input = corpus[index];
		const RGB expected = display_reference(
			{input.r, input.g, input.b}, output_mode, srgb_attachment, index);
		const bool compare_srgb_code = output_mode == 0 && srgb_attachment != 0;
		const float errors[] = {
			std::abs((compare_srgb_code ? linear_to_srgb(gpu[index].r) : gpu[index].r)
				- (compare_srgb_code ? linear_to_srgb(expected.r) : expected.r)),
			std::abs((compare_srgb_code ? linear_to_srgb(gpu[index].g) : gpu[index].g)
				- (compare_srgb_code ? linear_to_srgb(expected.g) : expected.g)),
			std::abs((compare_srgb_code ? linear_to_srgb(gpu[index].b) : gpu[index].b)
				- (compare_srgb_code ? linear_to_srgb(expected.b) : expected.b)),
		};
		for (float error : errors)
		{
			result.maximum_error = std::max(result.maximum_error, error);
			error_sum += error;
		}
	}
	result.mean_error = error_sum / (double)(corpus.size() * 3);
	// Gradient noise is deliberately sensitive to small floating-point changes;
	// CPU and GPU may choose opposite ends of the one-code dither interval.
	// EDR/HDR10 remain strict equation comparisons because they contain no noise.
	const float threshold = output_mode == 0 ? (1.0f / 255.0f + 2e-5f) : 2e-4f;
	if (result.maximum_error > threshold)
		throw std::runtime_error(result.name + " GPU error " + std::to_string(result.maximum_error)
			+ " exceeds " + std::to_string(threshold));
	return result;
}

static void write_json(
	const char* path,
	const std::string& device,
	const std::vector<ConfigurationResult>& results,
	size_t corpus_size)
{
	if (!path) return;
	const std::filesystem::path output_path(path);
	if (output_path.has_parent_path()) std::filesystem::create_directories(output_path.parent_path());
	std::ofstream file(output_path);
	if (!file.good()) throw std::runtime_error("could not write GPU validation JSON");
	file << "{\n  \"suite\": \"tonemapping-gpu-conformance-v1\",\n";
	file << "  \"device\": \"" << device << "\",\n";
	file << "  \"corpus_samples\": " << corpus_size << ",\n  \"passed\": true,\n";
	file << "  \"configurations\": [\n";
	for (size_t index = 0; index < results.size(); ++index)
	{
		const ConfigurationResult& result = results[index];
		file << "    {\"name\": \"" << result.name << "\", \"samples\": " << result.samples
			<< ", \"mean_absolute_rgb_error\": " << result.mean_error
			<< ", \"maximum_absolute_rgb_error\": " << result.maximum_error << "}";
		file << (index + 1 == results.size() ? "\n" : ",\n");
	}
	file << "  ]\n}\n";
}

int main(int argc, char** argv)
{
	const char* json_path = argc == 3 && std::string(argv[1]) == "--json" ? argv[2] : nullptr;
	VulkanHarness harness;
	try
	{
		const std::vector<Float4> corpus = TonemappingValidation::make_corpus();
		harness.initialize();
		harness.create_io(corpus);
		std::vector<ConfigurationResult> results;
		for (int output_mode = 0; output_mode < 3; ++output_mode)
		{
			Assets assets;
			std::string error;
			if (!TonemappingValidation::load_assets(output_mode, &assets, &error))
				throw std::runtime_error(error);
			harness.upload_lut(assets);
			for (int method = 0; method < TONEMAP_METHOD_COUNT; ++method)
			{
				const float scale = TonemappingValidation::integration_scale(method, output_mode);
				const std::vector<Float4> exact = harness.dispatch(method, scale, corpus.size(), false);
				results.push_back(compare(corpus, exact, assets, method, output_mode, false));
				if (method == TONEMAP_METHOD_KHRONOS_PBR_NEUTRAL)
				{
					const std::vector<Float4> matched = harness.dispatch(method, scale, corpus.size(), true);
					results.push_back(compare(corpus, matched, assets, method, output_mode, true));
				}
			}
		}
		for (int output_mode = 0; output_mode < 3; ++output_mode)
		{
			const int attachment_variants = output_mode == 0 ? 2 : 1;
			for (int srgb_attachment = 0; srgb_attachment < attachment_variants; ++srgb_attachment)
			{
				const std::vector<Float4> encoded = harness.dispatch_display(
					output_mode, srgb_attachment, corpus.size());
				results.push_back(compare_display(
					corpus, encoded, output_mode, srgb_attachment));
			}
		}
		write_json(json_path, harness.gpu_name(), results, corpus.size());
		std::printf("Tonemapping GPU conformance passed on %s: %zu samples, %zu configurations\n",
			harness.gpu_name().c_str(), corpus.size(), results.size());
		harness.shutdown();
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "Tonemapping GPU conformance failed: %s\n", exception.what());
		harness.shutdown();
		return 1;
	}
}
