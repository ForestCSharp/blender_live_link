#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "core/types.h"
#include "core/stretchy_buffer.h"
#include "core/timings.h"

#define VK_CHECK(f)                                                                 \
{                                                                                   \
	VkResult vk_check_result = (f);                                                 \
	if (vk_check_result != VK_SUCCESS)                                              \
	{                                                                               \
		printf("VULKAN ERROR: %s RETURNED: %i LINE: %i\n", #f, vk_check_result, __LINE__); \
		exit(1);                                                                    \
	}                                                                               \
}

static bool g_vulkan_debug_utils_enabled = false;

#include "render/gpu_image.h"

static const u32 MAX_FRAMES_IN_FLIGHT = 2;

// GPU timestamp queries: one pool per frame in flight; query 0/1 span the
// frame, 2+2i / 3+2i bracket timed scope i (one per render pass today)
static constexpr i32 MAX_GPU_TIMED_SCOPES = 48;	// main chain ~15 + GI capture passes
static constexpr i32 GPU_TIMESTAMP_QUERY_COUNT = 2 + 2 * MAX_GPU_TIMED_SCOPES;

// GPU resources can't be destroyed while a frame that references them is still
// in flight. Deletions are queued with the frame number they were requested on
// and freed once that frame's fence has been waited on.
struct PendingBufferDelete
{
	VkBuffer buffer;
	VmaAllocation allocation;
	u64 frame_number;
};

struct VulkanMetrics
{
	u64 descriptor_update_calls = 0;
	u64 descriptor_writes = 0;
	u64 descriptors_written = 0;
	u64 draw_calls = 0;
	u64 dispatch_calls = 0;
	u64 immediate_submit_count = 0;
	u64 queue_wait_idle_count = 0;
	u64 device_wait_idle_count = 0;
	u64 upload_bytes = 0;
	u64 pipeline_count = 0;
	f64 pipeline_creation_ms = 0.0;
};

struct VulkanMemoryStats
{
	u64 allocation_count = 0;
	u64 allocation_bytes = 0;
	u64 block_count = 0;
	u64 block_bytes = 0;
	u64 budget_bytes = 0;
	u64 usage_bytes = 0;
};

struct QueueSelection
{
	u32 graphics_family = UINT32_MAX;
	u32 present_family = UINT32_MAX;
	bool complete() const { return graphics_family != UINT32_MAX && present_family != UINT32_MAX; }
	bool shared_family() const { return complete() && graphics_family == present_family; }
};

struct VulkanCapabilities
{
	VkPhysicalDeviceProperties properties = {};
	VkPhysicalDeviceVulkan12Features features_1_2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
	};
	VkPhysicalDeviceVulkan13Features features_1_3 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
	};
	QueueSelection queues;
	bool swapchain_extension = false;
	bool portability_subset_extension = false;
	bool compatible = false;
	i32 score = -1;
	VkSurfaceFormatKHR surface_format = {};
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
	VkFormat scene_color_format = VK_FORMAT_UNDEFINED;
	VkFormat scene_depth_format = VK_FORMAT_UNDEFINED;
	VkFormat gbuffer_format = VK_FORMAT_UNDEFINED;
	VkFormat shadow_moments_format = VK_FORMAT_UNDEFINED;
	VkFormat ssao_format = VK_FORMAT_UNDEFINED;
	char rejection_reason[1024] = {};
};

struct VulkanContext
{
	GLFWwindow* window = nullptr;

	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	u32 graphics_queue_family_index = 0;
	u32 present_queue_family_index = 0;
	VkSurfaceFormatKHR surface_format = {};
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue graphics_queue = VK_NULL_HANDLE;
	VkQueue present_queue = VK_NULL_HANDLE;
	VmaAllocator allocator = VK_NULL_HANDLE;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties physical_device_properties = {};
	VulkanCapabilities capabilities;
	u64 shader_build_hash = 0;
	bool debug_utils_enabled = false;
	bool portability_enumeration_enabled = false;
	bool screenshot_supported = false;

	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkExtent2D swapchain_extent = {};
	u32 swapchain_min_image_count = 2;
	u32 swapchain_image_count = 0;
	std::vector<VkImage> swapchain_images;
	std::vector<VkImageView> swapchain_image_views;

	VkCommandPool command_pool = VK_NULL_HANDLE;

	// Per frame-in-flight
	VkCommandBuffer command_buffers[MAX_FRAMES_IN_FLIGHT] = {};
	VkSemaphore image_available_semaphores[MAX_FRAMES_IN_FLIGHT] = {};
	VkFence frame_fences[MAX_FRAMES_IN_FLIGHT] = {};

	// Per swapchain image (present can't wait on a per-frame semaphore safely)
	std::vector<VkSemaphore> render_finished_semaphores;

	u64 frame_number = 0;
	u32 frame_index = 0;
	u32 swapchain_image_index = 0;
	bool needs_resize = false;

	// When set, end_frame dumps the frame to this path (between submit and
	// present, while the swapchain image is still acquired) then clears it
	const char* pending_frame_dump = nullptr;

	// GPU timestamp state
	bool timestamps_supported = false;
	f32 timestamp_period_ns = 0.0f;
	VkQueryPool timestamp_pools[MAX_FRAMES_IN_FLIGHT] = {};
	struct GpuTimestampFrameState
	{
		i64 cpu_frame_index = -1;
		i32 scope_count = 0;
		char scope_names[MAX_GPU_TIMED_SCOPES][CPU_TIMINGS_MAX_NAME_LENGTH] = {};
		bool submitted = false;
	} timestamp_frames[MAX_FRAMES_IN_FLIGHT];

	StretchyBuffer<PendingBufferDelete> deletion_queue;
	VulkanMetrics metrics;
};

// Set by vulkan_context_init; used by GpuBuffer's lazy creation (the way
// sokol's global context backs sg_make_buffer in game/)
static VulkanContext* g_vulkan_context = nullptr;

inline u64 vulkan_hash_bytes(u64 in_hash, const void* in_data, size_t in_size)
{
	const u8* bytes = (const u8*)in_data;
	for (size_t byte_index = 0; byte_index < in_size; ++byte_index)
	{
		in_hash ^= bytes[byte_index];
		in_hash *= 1099511628211ull;
	}
	return in_hash;
}

u64 vulkan_shader_build_hash()
{
	u64 hash = 1469598103934665603ull;
	std::vector<std::filesystem::path> shader_paths;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator("bin/shaders", error))
	{
		if (!error && entry.is_regular_file() && entry.path().extension() == ".spv")
		{
			shader_paths.push_back(entry.path());
		}
	}
	std::sort(shader_paths.begin(), shader_paths.end());
	for (const std::filesystem::path& path : shader_paths)
	{
		const std::string path_text = path.filename().string();
		hash = vulkan_hash_bytes(hash, path_text.data(), path_text.size());
		std::ifstream file(path, std::ios::binary);
		char buffer[16384];
		while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0)
		{
			hash = vulkan_hash_bytes(hash, buffer, (size_t)file.gcount());
		}
	}
	return hash;
}

void vulkan_set_object_name(VulkanContext* ctx, VkObjectType in_type, u64 in_handle, const char* in_name)
{
	if (!ctx || !ctx->debug_utils_enabled || !vkSetDebugUtilsObjectNameEXT || in_handle == 0 || !in_name || in_name[0] == '\0')
	{
		return;
	}
	VkDebugUtilsObjectNameInfoEXT name_info = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
		.objectType = in_type,
		.objectHandle = in_handle,
		.pObjectName = in_name,
	};
	vkSetDebugUtilsObjectNameEXT(ctx->device, &name_info);
}

void vulkan_begin_debug_label(VulkanContext* ctx, const char* in_name)
{
	if (!ctx || !ctx->debug_utils_enabled || !vkCmdBeginDebugUtilsLabelEXT || !in_name) return;
	VkDebugUtilsLabelEXT label = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
		.pLabelName = in_name,
		.color = { 0.25f, 0.55f, 0.9f, 1.0f },
	};
	vkCmdBeginDebugUtilsLabelEXT(ctx->command_buffers[ctx->frame_index], &label);
}

void vulkan_end_debug_label(VulkanContext* ctx)
{
	if (ctx && ctx->debug_utils_enabled && vkCmdEndDebugUtilsLabelEXT)
	{
		vkCmdEndDebugUtilsLabelEXT(ctx->command_buffers[ctx->frame_index]);
	}
}

void vulkan_update_descriptor_sets(
	VulkanContext* ctx,
	u32 in_write_count,
	const VkWriteDescriptorSet* in_writes,
	u32 in_copy_count = 0,
	const VkCopyDescriptorSet* in_copies = nullptr)
{
	ctx->metrics.descriptor_update_calls += 1;
	ctx->metrics.descriptor_writes += in_write_count;
	for (u32 write_index = 0; write_index < in_write_count; ++write_index)
	{
		ctx->metrics.descriptors_written += in_writes[write_index].descriptorCount;
	}
	vkUpdateDescriptorSets(ctx->device, in_write_count, in_writes, in_copy_count, in_copies);
}

VkResult vulkan_create_graphics_pipelines(
	VulkanContext* ctx,
	u32 in_count,
	const VkGraphicsPipelineCreateInfo* in_infos,
	VkPipeline* out_pipelines)
{
	const auto start = std::chrono::steady_clock::now();
	VkResult result = vkCreateGraphicsPipelines(ctx->device, ctx->pipeline_cache, in_count, in_infos, nullptr, out_pipelines);
	const auto end = std::chrono::steady_clock::now();
	ctx->metrics.pipeline_creation_ms += std::chrono::duration<f64, std::milli>(end - start).count();
	ctx->metrics.pipeline_count += in_count;
	for (u32 index = 0; result == VK_SUCCESS && index < in_count; ++index)
	{
		char name[64];
		snprintf(name, sizeof(name), "Graphics Pipeline %llu", (unsigned long long)(ctx->metrics.pipeline_count - in_count + index));
		vulkan_set_object_name(ctx, VK_OBJECT_TYPE_PIPELINE, (u64)out_pipelines[index], name);
	}
	return result;
}

VkResult vulkan_create_compute_pipelines(
	VulkanContext* ctx,
	u32 in_count,
	const VkComputePipelineCreateInfo* in_infos,
	VkPipeline* out_pipelines)
{
	const auto start = std::chrono::steady_clock::now();
	VkResult result = vkCreateComputePipelines(ctx->device, ctx->pipeline_cache, in_count, in_infos, nullptr, out_pipelines);
	const auto end = std::chrono::steady_clock::now();
	ctx->metrics.pipeline_creation_ms += std::chrono::duration<f64, std::milli>(end - start).count();
	ctx->metrics.pipeline_count += in_count;
	for (u32 index = 0; result == VK_SUCCESS && index < in_count; ++index)
	{
		char name[64];
		snprintf(name, sizeof(name), "Compute Pipeline %llu", (unsigned long long)(ctx->metrics.pipeline_count - in_count + index));
		vulkan_set_object_name(ctx, VK_OBJECT_TYPE_PIPELINE, (u64)out_pipelines[index], name);
	}
	return result;
}

void vulkan_cmd_draw(VulkanContext* ctx, u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance)
{
	ctx->metrics.draw_calls += 1;
	vkCmdDraw(ctx->command_buffers[ctx->frame_index], vertex_count, instance_count, first_vertex, first_instance);
}

void vulkan_cmd_draw_indexed(VulkanContext* ctx, u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset, u32 first_instance)
{
	ctx->metrics.draw_calls += 1;
	vkCmdDrawIndexed(ctx->command_buffers[ctx->frame_index], index_count, instance_count, first_index, vertex_offset, first_instance);
}

void vulkan_cmd_dispatch(VulkanContext* ctx, u32 x, u32 y, u32 z)
{
	ctx->metrics.dispatch_calls += 1;
	vkCmdDispatch(ctx->command_buffers[ctx->frame_index], x, y, z);
}

VkResult vulkan_device_wait_idle(VulkanContext* ctx)
{
	ctx->metrics.device_wait_idle_count += 1;
	return vkDeviceWaitIdle(ctx->device);
}

VkResult vulkan_queue_wait_idle(VulkanContext* ctx)
{
	ctx->metrics.queue_wait_idle_count += 1;
	return vkQueueWaitIdle(ctx->graphics_queue);
}

VulkanMemoryStats vulkan_context_get_memory_stats(VulkanContext* ctx)
{
	VulkanMemoryStats result = {};
	if (!ctx || !ctx->allocator) return result;
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
	vmaGetHeapBudgets(ctx->allocator, budgets);
	VkPhysicalDeviceMemoryProperties memory_properties = {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &memory_properties);
	for (u32 heap_index = 0; heap_index < memory_properties.memoryHeapCount; ++heap_index)
	{
		result.allocation_count += budgets[heap_index].statistics.allocationCount;
		result.allocation_bytes += budgets[heap_index].statistics.allocationBytes;
		result.block_count += budgets[heap_index].statistics.blockCount;
		result.block_bytes += budgets[heap_index].statistics.blockBytes;
		if (memory_properties.memoryHeaps[heap_index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
		{
			result.budget_bytes += budgets[heap_index].budget;
			result.usage_bytes += budgets[heap_index].usage;
		}
	}
	return result;
}

bool vulkan_has_extension(const std::vector<VkExtensionProperties>& in_extensions, const char* in_name)
{
	for (const VkExtensionProperties& extension : in_extensions)
	{
		if (strcmp(extension.extensionName, in_name) == 0) return true;
	}
	return false;
}

const char* vulkan_present_mode_name(VkPresentModeKHR in_mode)
{
	switch (in_mode)
	{
		case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
		case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
		case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
		case VK_PRESENT_MODE_FIFO_KHR:
		default: return "fifo";
	}
}

bool vulkan_format_is_bgra8(VkFormat in_format)
{
	return in_format == VK_FORMAT_B8G8R8A8_SRGB || in_format == VK_FORMAT_B8G8R8A8_UNORM;
}

bool vulkan_format_is_rgba8(VkFormat in_format)
{
	return in_format == VK_FORMAT_R8G8B8A8_SRGB || in_format == VK_FORMAT_R8G8B8A8_UNORM;
}

VkPresentModeKHR vulkan_requested_present_mode()
{
	const char* requested = getenv("GAME_PRESENT_MODE");
	if (!requested) requested = getenv("GAME2_PRESENT_MODE");
	if (!requested || strcmp(requested, "fifo") == 0 || strcmp(requested, "vsync") == 0)
		return VK_PRESENT_MODE_FIFO_KHR;
	if (strcmp(requested, "mailbox") == 0)
		return VK_PRESENT_MODE_MAILBOX_KHR;
	if (strcmp(requested, "immediate") == 0)
		return VK_PRESENT_MODE_IMMEDIATE_KHR;
	if (strcmp(requested, "fifo_relaxed") == 0)
		return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
	printf("Unknown GAME_PRESENT_MODE '%s'; using fifo\n", requested);
	return VK_PRESENT_MODE_FIFO_KHR;
}

bool vulkan_format_supports(VkPhysicalDevice in_device, VkFormat in_format, VkFormatFeatureFlags2 in_features)
{
	VkFormatProperties3 properties_3 = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
	};
	VkFormatProperties2 properties_2 = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &properties_3,
	};
	vkGetPhysicalDeviceFormatProperties2(in_device, in_format, &properties_2);
	return (properties_3.optimalTilingFeatures & in_features) == in_features;
}

VkFormat vulkan_choose_format(
	VkPhysicalDevice in_device,
	const VkFormat* in_candidates,
	u32 in_candidate_count,
	VkFormatFeatureFlags2 in_required_features)
{
	for (u32 candidate_index = 0; candidate_index < in_candidate_count; ++candidate_index)
	{
		if (vulkan_format_supports(in_device, in_candidates[candidate_index], in_required_features))
			return in_candidates[candidate_index];
	}
	return VK_FORMAT_UNDEFINED;
}

void vulkan_append_rejection(char* out_reason, size_t in_size, const char* in_text)
{
	if (!in_text || !in_text[0]) return;
	const size_t used = strlen(out_reason);
	if (used >= in_size - 1) return;
	snprintf(out_reason + used, in_size - used, "%s%s", used > 0 ? "; " : "", in_text);
}

VulkanCapabilities vulkan_evaluate_device(VkPhysicalDevice in_device, VkSurfaceKHR in_surface)
{
	VulkanCapabilities result;
	result.features_1_3.pNext = &result.features_1_2;
	VkPhysicalDeviceFeatures2 features_2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &result.features_1_3,
	};
	vkGetPhysicalDeviceFeatures2(in_device, &features_2);
	vkGetPhysicalDeviceProperties(in_device, &result.properties);

	u32 extension_count = 0;
	vkEnumerateDeviceExtensionProperties(in_device, nullptr, &extension_count, nullptr);
	std::vector<VkExtensionProperties> extensions(extension_count);
	vkEnumerateDeviceExtensionProperties(in_device, nullptr, &extension_count, extensions.data());
	result.swapchain_extension = vulkan_has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	result.portability_subset_extension = vulkan_has_extension(extensions, "VK_KHR_portability_subset");

	u32 queue_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(in_device, &queue_count, nullptr);
	std::vector<VkQueueFamilyProperties> queues(queue_count);
	vkGetPhysicalDeviceQueueFamilyProperties(in_device, &queue_count, queues.data());
	for (u32 queue_index = 0; queue_index < queue_count; ++queue_index)
	{
		if (result.queues.graphics_family == UINT32_MAX && (queues[queue_index].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			result.queues.graphics_family = queue_index;
		VkBool32 present_supported = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(in_device, queue_index, in_surface, &present_supported);
		if (result.queues.present_family == UINT32_MAX && present_supported)
			result.queues.present_family = queue_index;
		if ((queues[queue_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_supported)
		{
			result.queues.graphics_family = queue_index;
			result.queues.present_family = queue_index;
			break;
		}
	}

	u32 format_count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(in_device, in_surface, &format_count, nullptr);
	std::vector<VkSurfaceFormatKHR> surface_formats(format_count);
	if (format_count > 0)
		vkGetPhysicalDeviceSurfaceFormatsKHR(in_device, in_surface, &format_count, surface_formats.data());
	if (surface_formats.size() == 1 && surface_formats[0].format == VK_FORMAT_UNDEFINED)
	{
		result.surface_format = {
			.format = VK_FORMAT_B8G8R8A8_SRGB,
			.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		};
	}
	const VkFormat surface_format_preferences[] = {
		VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
		VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
	};
	for (VkFormat preferred_format : surface_format_preferences)
	{
		for (const VkSurfaceFormatKHR& format : surface_formats)
		{
			if (format.format == preferred_format && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				result.surface_format = format;
				break;
			}
		}
		if (result.surface_format.format != VK_FORMAT_UNDEFINED) break;
	}
	if (result.surface_format.format == VK_FORMAT_UNDEFINED && !surface_formats.empty() && surface_formats[0].format != VK_FORMAT_UNDEFINED)
		result.surface_format = surface_formats[0];

	u32 present_mode_count = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(in_device, in_surface, &present_mode_count, nullptr);
	std::vector<VkPresentModeKHR> present_modes(present_mode_count);
	if (present_mode_count > 0)
		vkGetPhysicalDeviceSurfacePresentModesKHR(in_device, in_surface, &present_mode_count, present_modes.data());
	const VkPresentModeKHR requested_present_mode = vulkan_requested_present_mode();
	result.present_mode = VK_PRESENT_MODE_FIFO_KHR;
	for (VkPresentModeKHR mode : present_modes)
	{
		if (mode == requested_present_mode) result.present_mode = mode;
	}
	VkSurfaceCapabilitiesKHR surface_capabilities = {};
	const VkResult surface_capabilities_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(in_device, in_surface, &surface_capabilities);

	const VkFormat color_16_candidates[] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
	const VkFormat depth_candidates[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT };
	const VkFormat gbuffer_candidates[] = { VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT };
	const VkFormat ssao_candidates[] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT };
	const VkFormatFeatureFlags2 color_features = VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
	result.scene_color_format = vulkan_choose_format(in_device, color_16_candidates, 2, color_features | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
	result.scene_depth_format = vulkan_choose_format(in_device, depth_candidates, 3, VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT);
	result.gbuffer_format = vulkan_choose_format(in_device, gbuffer_candidates, 2, color_features | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
	result.shadow_moments_format = vulkan_choose_format(in_device, color_16_candidates, 2, color_features | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
	result.ssao_format = vulkan_choose_format(in_device, ssao_candidates, 2, color_features | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT);

	if (VK_API_VERSION_MAJOR(result.properties.apiVersion) < 1 ||
		(VK_API_VERSION_MAJOR(result.properties.apiVersion) == 1 && VK_API_VERSION_MINOR(result.properties.apiVersion) < 3))
		vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "Vulkan 1.3 required");
	if (!result.swapchain_extension) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "VK_KHR_swapchain missing");
	if (!result.queues.complete()) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "graphics/present queues missing");
	if (format_count == 0 || present_mode_count == 0) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "surface formats/present modes missing");
	if (result.surface_format.format == VK_FORMAT_UNDEFINED) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "no usable surface format");
	if (surface_capabilities_result != VK_SUCCESS || !(surface_capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
		vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "surface color attachments unsupported");
	if (surface_capabilities_result == VK_SUCCESS && surface_capabilities.maxImageCount > 0 && surface_capabilities.maxImageCount < 2)
		vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "at least two swapchain images required");
	if (!result.features_1_3.dynamicRendering) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "dynamicRendering missing");
	if (!result.features_1_3.synchronization2) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "synchronization2 missing");
	if (!result.features_1_3.shaderDemoteToHelperInvocation) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "shaderDemoteToHelperInvocation missing");
	if (!result.features_1_2.descriptorBindingPartiallyBound) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "descriptorBindingPartiallyBound missing");
	if (!result.features_1_2.shaderSampledImageArrayNonUniformIndexing) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "sampled-image non-uniform indexing missing");
	if (result.properties.limits.maxPerStageDescriptorSampledImages < 128 || result.properties.limits.maxDescriptorSetSampledImages < 128)
		vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "128 sampled-image descriptors unsupported");
	if (result.scene_color_format == VK_FORMAT_UNDEFINED) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "scene-color format unsupported");
	if (result.scene_depth_format == VK_FORMAT_UNDEFINED) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "scene-depth format unsupported");
	if (result.gbuffer_format == VK_FORMAT_UNDEFINED) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "G-buffer format unsupported");
	if (result.shadow_moments_format == VK_FORMAT_UNDEFINED) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "shadow-moments format unsupported");
	if (result.ssao_format == VK_FORMAT_UNDEFINED) vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "SSAO format unsupported");
	if (!vulkan_format_supports(in_device, VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT))
		vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "RGBA8 uploaded textures unsupported");
	if (!vulkan_format_supports(in_device, VK_FORMAT_R32G32B32A32_SFLOAT,
		VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT))
		vulkan_append_rejection(result.rejection_reason, sizeof(result.rejection_reason), "RGBA32F SSAO noise texture unsupported");

	result.compatible = result.rejection_reason[0] == '\0';
	if (result.compatible)
	{
		result.score = result.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2000
			: result.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1000 : 100;
		result.score += (i32)MIN(result.properties.limits.maxImageDimension2D / 1024, 100u);
		if (result.queues.shared_family()) result.score += 50;
	}
	result.features_1_3.pNext = nullptr;
	result.features_1_2.pNext = nullptr;
	return result;
}

VKAPI_ATTR VkBool32 VKAPI_CALL vulkan_debug_callback(
	VkDebugUtilsMessageSeverityFlagBitsEXT in_severity,
	VkDebugUtilsMessageTypeFlagsEXT in_type,
	const VkDebugUtilsMessengerCallbackDataEXT* in_callback_data,
	void* in_user_data)
{
	printf("[vulkan] %s\n", in_callback_data->pMessage);
	return VK_FALSE;
}

struct PipelineCacheFileHeader
{
	u32 magic = 0x47435043; // GCPC
	u32 version = 1;
	u32 vendor_id = 0;
	u32 device_id = 0;
	u32 driver_version = 0;
	u8 pipeline_cache_uuid[VK_UUID_SIZE] = {};
	u64 shader_build_hash = 0;
	u64 payload_size = 0;
};

const char* vulkan_pipeline_cache_path()
{
	const char* override_path = getenv("GAME_PIPELINE_CACHE");
	if (!override_path) override_path = getenv("GAME2_PIPELINE_CACHE");
	return override_path ? override_path : "bin/pipeline_cache.bin";
}

void vulkan_context_create_pipeline_cache(VulkanContext* ctx)
{
	ctx->shader_build_hash = vulkan_shader_build_hash();
	std::vector<u8> initial_data;
	std::ifstream input(vulkan_pipeline_cache_path(), std::ios::binary);
	if (input)
	{
		PipelineCacheFileHeader header = {};
		input.read((char*)&header, sizeof(header));
		const bool compatible = input.good()
			&& header.magic == PipelineCacheFileHeader{}.magic
			&& header.version == PipelineCacheFileHeader{}.version
			&& header.vendor_id == ctx->physical_device_properties.vendorID
			&& header.device_id == ctx->physical_device_properties.deviceID
			&& header.driver_version == ctx->physical_device_properties.driverVersion
			&& header.shader_build_hash == ctx->shader_build_hash
			&& memcmp(header.pipeline_cache_uuid, ctx->physical_device_properties.pipelineCacheUUID, VK_UUID_SIZE) == 0
			&& header.payload_size <= 128ull * 1024ull * 1024ull;
		if (compatible)
		{
			initial_data.resize((size_t)header.payload_size);
			input.read((char*)initial_data.data(), (std::streamsize)initial_data.size());
			if (!input) initial_data.clear();
		}
	}

	VkPipelineCacheCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
		.initialDataSize = initial_data.size(),
		.pInitialData = initial_data.empty() ? nullptr : initial_data.data(),
	};
	VkResult create_result = vkCreatePipelineCache(ctx->device, &create_info, nullptr, &ctx->pipeline_cache);
	if (create_result != VK_SUCCESS && !initial_data.empty())
	{
		printf("Pipeline cache rejected cached data; rebuilding it\n");
		create_info.initialDataSize = 0;
		create_info.pInitialData = nullptr;
		VK_CHECK(vkCreatePipelineCache(ctx->device, &create_info, nullptr, &ctx->pipeline_cache));
	}
	else
	{
		VK_CHECK(create_result);
	}
	vulkan_set_object_name(ctx, VK_OBJECT_TYPE_PIPELINE_CACHE, (u64)ctx->pipeline_cache, "Persistent Pipeline Cache");
	printf("Pipeline cache: %s (%zu cached bytes)\n", vulkan_pipeline_cache_path(), initial_data.size());
}

void vulkan_context_save_pipeline_cache(VulkanContext* ctx)
{
	if (!ctx->pipeline_cache) return;
	size_t payload_size = 0;
	if (vkGetPipelineCacheData(ctx->device, ctx->pipeline_cache, &payload_size, nullptr) != VK_SUCCESS || payload_size == 0)
	{
		return;
	}
	std::vector<u8> payload(payload_size);
	if (vkGetPipelineCacheData(ctx->device, ctx->pipeline_cache, &payload_size, payload.data()) != VK_SUCCESS)
	{
		return;
	}
	payload.resize(payload_size);
	PipelineCacheFileHeader header = {
		.vendor_id = ctx->physical_device_properties.vendorID,
		.device_id = ctx->physical_device_properties.deviceID,
		.driver_version = ctx->physical_device_properties.driverVersion,
		.shader_build_hash = ctx->shader_build_hash,
		.payload_size = payload.size(),
	};
	memcpy(header.pipeline_cache_uuid, ctx->physical_device_properties.pipelineCacheUUID, VK_UUID_SIZE);
	std::ofstream output(vulkan_pipeline_cache_path(), std::ios::binary | std::ios::trunc);
	if (output)
	{
		output.write((const char*)&header, sizeof(header));
		output.write((const char*)payload.data(), (std::streamsize)payload.size());
	}
}

void vulkan_context_create_swapchain(VulkanContext* ctx)
{
	i32 framebuffer_width = 0;
	i32 framebuffer_height = 0;
	glfwGetFramebufferSize(ctx->window, &framebuffer_width, &framebuffer_height);

	VkSurfaceCapabilitiesKHR surface_capabilities;
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physical_device, ctx->surface, &surface_capabilities));

	VkExtent2D extent = surface_capabilities.currentExtent;
	if (extent.width == UINT32_MAX)
	{
		extent.width = CLAMP((u32)MAX(framebuffer_width, 1), surface_capabilities.minImageExtent.width, surface_capabilities.maxImageExtent.width);
		extent.height = CLAMP((u32)MAX(framebuffer_height, 1), surface_capabilities.minImageExtent.height, surface_capabilities.maxImageExtent.height);
	}
	ctx->swapchain_extent = extent;

	u32 desired_image_count = surface_capabilities.minImageCount + 1;
	if (surface_capabilities.maxImageCount > 0 && desired_image_count > surface_capabilities.maxImageCount)
	{
		desired_image_count = surface_capabilities.maxImageCount;
	}
	ctx->swapchain_min_image_count = MAX(surface_capabilities.minImageCount, 2u);
	if (surface_capabilities.maxImageCount > 0)
		ctx->swapchain_min_image_count = MIN(ctx->swapchain_min_image_count, surface_capabilities.maxImageCount);

	VkSwapchainKHR old_swapchain = ctx->swapchain;
	if (!(surface_capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
	{
		printf("Selected surface does not support color-attachment swapchain images\n");
		exit(1);
	}
	VkImageUsageFlags image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ctx->screenshot_supported = (surface_capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0
		&& (vulkan_format_is_bgra8(ctx->surface_format.format) || vulkan_format_is_rgba8(ctx->surface_format.format));
	if (ctx->screenshot_supported) image_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	VkSurfaceTransformFlagBitsKHR pre_transform = surface_capabilities.currentTransform;
	if (surface_capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
		pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	const VkCompositeAlphaFlagBitsKHR alpha_preferences[] = {
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
		VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
		VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
	};
	VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	for (VkCompositeAlphaFlagBitsKHR candidate : alpha_preferences)
	{
		if (surface_capabilities.supportedCompositeAlpha & candidate)
		{
			composite_alpha = candidate;
			break;
		}
	}
	u32 queue_family_indices[] = { ctx->graphics_queue_family_index, ctx->present_queue_family_index };
	const bool separate_present_queue = ctx->graphics_queue_family_index != ctx->present_queue_family_index;

	VkSwapchainCreateInfoKHR swapchain_create_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = ctx->surface,
		.minImageCount = desired_image_count,
		.imageFormat = ctx->surface_format.format,
		.imageColorSpace = ctx->surface_format.colorSpace,
		.imageExtent = extent,
		.imageArrayLayers = 1,
		.imageUsage = image_usage,
		.imageSharingMode = separate_present_queue ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = separate_present_queue ? 2u : 0u,
		.pQueueFamilyIndices = separate_present_queue ? queue_family_indices : nullptr,
		.preTransform = pre_transform,
		.compositeAlpha = composite_alpha,
		.presentMode = ctx->present_mode,
		.clipped = VK_TRUE,
		.oldSwapchain = old_swapchain,
	};

	VK_CHECK(vkCreateSwapchainKHR(ctx->device, &swapchain_create_info, nullptr, &ctx->swapchain));
	printf("Swapchain: %ux%u, %u requested images, %s, screenshots %s\n",
		extent.width, extent.height, desired_image_count, vulkan_present_mode_name(ctx->present_mode),
		ctx->screenshot_supported ? "enabled" : "unsupported by surface");

	if (old_swapchain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(ctx->device, old_swapchain, nullptr);
	}

	u32 swapchain_image_count = 0;
	VK_CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &swapchain_image_count, nullptr));
	ctx->swapchain_images.resize(swapchain_image_count);
	VK_CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &swapchain_image_count, ctx->swapchain_images.data()));
	ctx->swapchain_image_count = swapchain_image_count;
	if (ctx->swapchain_image_count < ctx->swapchain_min_image_count)
	{
		printf("Swapchain returned %u images, below required minimum %u\n", ctx->swapchain_image_count, ctx->swapchain_min_image_count);
		exit(1);
	}

	ctx->swapchain_image_views.resize(swapchain_image_count);
	for (u32 image_idx = 0; image_idx < swapchain_image_count; ++image_idx)
	{
		char image_name[64];
		snprintf(image_name, sizeof(image_name), "Swapchain Image %u", image_idx);
		vulkan_set_object_name(ctx, VK_OBJECT_TYPE_IMAGE, (u64)ctx->swapchain_images[image_idx], image_name);
		VkImageViewCreateInfo image_view_create_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = ctx->swapchain_images[image_idx],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = ctx->surface_format.format,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};
		VK_CHECK(vkCreateImageView(ctx->device, &image_view_create_info, nullptr, &ctx->swapchain_image_views[image_idx]));
		char view_name[64];
		snprintf(view_name, sizeof(view_name), "Swapchain Image %u View", image_idx);
		vulkan_set_object_name(ctx, VK_OBJECT_TYPE_IMAGE_VIEW, (u64)ctx->swapchain_image_views[image_idx], view_name);
	}

	// Render-finished semaphores are indexed by swapchain image
	for (VkSemaphore semaphore : ctx->render_finished_semaphores)
	{
		vkDestroySemaphore(ctx->device, semaphore, nullptr);
	}
	ctx->render_finished_semaphores.resize(swapchain_image_count);
	for (u32 image_idx = 0; image_idx < swapchain_image_count; ++image_idx)
	{
		const VkSemaphoreCreateInfo semaphore_create_info = {
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};
		VK_CHECK(vkCreateSemaphore(ctx->device, &semaphore_create_info, nullptr, &ctx->render_finished_semaphores[image_idx]));
	}

}

void vulkan_context_destroy_swapchain_resources(VulkanContext* ctx)
{
	for (VkImageView image_view : ctx->swapchain_image_views)
	{
		vkDestroyImageView(ctx->device, image_view, nullptr);
	}
	ctx->swapchain_image_views.clear();
	ctx->swapchain_images.clear();
	ctx->swapchain_image_count = 0;
}

void vulkan_context_recreate_swapchain(VulkanContext* ctx)
{
	// Wait out minimization (0x0 framebuffer)
	i32 framebuffer_width = 0;
	i32 framebuffer_height = 0;
	glfwGetFramebufferSize(ctx->window, &framebuffer_width, &framebuffer_height);
	while (framebuffer_width == 0 || framebuffer_height == 0)
	{
		glfwWaitEvents();
		glfwGetFramebufferSize(ctx->window, &framebuffer_width, &framebuffer_height);
	}

	VK_CHECK(vulkan_device_wait_idle(ctx));

	vulkan_context_destroy_swapchain_resources(ctx);
	vulkan_context_create_swapchain(ctx);

	ctx->needs_resize = false;
}

void vulkan_context_init(VulkanContext* ctx, GLFWwindow* in_window)
{
	ctx->window = in_window;
	g_vulkan_context = ctx;

	VK_CHECK(volkInitialize());

	// Must run after volkInitialize: GLFW only finds the Vulkan loader once
	// volk has dlopened it into the process
	if (!glfwVulkanSupported())
	{
		printf("GLFW reports Vulkan is not supported\n");
		exit(1);
	}

	// Instance
	{
		VkInstanceCreateFlags instance_create_flags = 0;

		VkApplicationInfo app_info = {
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.pApplicationName = "Blender Game",
			.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
			.pEngineName = "Blender Game",
			.engineVersion = VK_MAKE_VERSION(1, 0, 0),
			.apiVersion = VK_API_VERSION_1_3,
		};

		// Develop/Debug request validation by default; Release opts in through
		// GAME_ENABLE_VALIDATION=1.
		const char* validation_layers[] = { "VK_LAYER_KHRONOS_validation" };
		u32 enabled_layer_count = 0;
		#if GAME_ENABLE_VALIDATION
		{
			u32 available_layer_count = 0;
			vkEnumerateInstanceLayerProperties(&available_layer_count, nullptr);
			std::vector<VkLayerProperties> available_layers(available_layer_count);
			vkEnumerateInstanceLayerProperties(&available_layer_count, available_layers.data());
			for (const VkLayerProperties& layer : available_layers)
			{
				if (strcmp(layer.layerName, validation_layers[0]) == 0)
				{
					enabled_layer_count = 1;
					break;
				}
			}
			if (enabled_layer_count == 0)
			{
				printf("VK_LAYER_KHRONOS_validation not found; running without validation\n");
			}
		}
		#endif

		u32 available_extension_count = 0;
		VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &available_extension_count, nullptr));
		std::vector<VkExtensionProperties> available_extensions(available_extension_count);
		VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &available_extension_count, available_extensions.data()));

		// GLFW knows the required platform surface extensions.
		u32 glfw_extension_count = 0;
		const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);
		if (!glfw_extensions || glfw_extension_count == 0)
		{
			printf("GLFW did not provide the Vulkan surface extensions required by this platform\n");
			exit(1);
		}

		std::vector<const char*> instance_extensions(glfw_extensions, glfw_extensions + glfw_extension_count);
		for (const char* required_extension : instance_extensions)
		{
			if (!vulkan_has_extension(available_extensions, required_extension))
			{
				printf("Required Vulkan instance extension is unavailable: %s\n", required_extension);
				exit(1);
			}
		}
		ctx->portability_enumeration_enabled = vulkan_has_extension(available_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		if (ctx->portability_enumeration_enabled)
		{
			instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			instance_create_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		}
		ctx->debug_utils_enabled = vulkan_has_extension(available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		g_vulkan_debug_utils_enabled = ctx->debug_utils_enabled;
		if (ctx->debug_utils_enabled) instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

		VkInstanceCreateInfo instance_create_info = {
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.flags = instance_create_flags,
			.pApplicationInfo = &app_info,
			.enabledLayerCount = enabled_layer_count,
			.ppEnabledLayerNames = validation_layers,
			.enabledExtensionCount = (u32) instance_extensions.size(),
			.ppEnabledExtensionNames = instance_extensions.data(),
		};

		VK_CHECK(vkCreateInstance(&instance_create_info, nullptr, &ctx->instance));
		volkLoadInstance(ctx->instance);
	}

	// Debug messenger (routes validation messages through our printf)
	if (ctx->debug_utils_enabled)
	{
		VkDebugUtilsMessengerCreateInfoEXT debug_messenger_create_info = {
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
							 | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
						 | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
						 | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
			.pfnUserCallback = vulkan_debug_callback,
		};
		VK_CHECK(vkCreateDebugUtilsMessengerEXT(ctx->instance, &debug_messenger_create_info, nullptr, &ctx->debug_messenger));
	}

	VK_CHECK(glfwCreateWindowSurface(ctx->instance, ctx->window, nullptr, &ctx->surface));

	// Score every compatible device instead of relying on enumeration order.
	{
		u32 physical_device_count = 0;
		VK_CHECK(vkEnumeratePhysicalDevices(ctx->instance, &physical_device_count, nullptr));
		if (physical_device_count == 0)
		{
			printf("No Vulkan physical devices were found\n");
			exit(1);
		}
		std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
		VK_CHECK(vkEnumeratePhysicalDevices(ctx->instance, &physical_device_count, physical_devices.data()));

		for (VkPhysicalDevice candidate : physical_devices)
		{
			VulkanCapabilities evaluated = vulkan_evaluate_device(candidate, ctx->surface);
			if (!evaluated.compatible)
			{
				printf("Skipping GPU '%s': %s\n", evaluated.properties.deviceName, evaluated.rejection_reason);
				continue;
			}
			printf("Compatible GPU '%s' score %i\n", evaluated.properties.deviceName, evaluated.score);
			if (ctx->physical_device == VK_NULL_HANDLE || evaluated.score > ctx->capabilities.score)
			{
				ctx->physical_device = candidate;
				ctx->capabilities = evaluated;
			}
		}
		if (ctx->physical_device == VK_NULL_HANDLE)
		{
			printf("No compatible Vulkan 1.3 device was found\n");
			exit(1);
		}
		ctx->physical_device_properties = ctx->capabilities.properties;
		ctx->surface_format = ctx->capabilities.surface_format;
		ctx->present_mode = ctx->capabilities.present_mode;
		ctx->graphics_queue_family_index = ctx->capabilities.queues.graphics_family;
		ctx->present_queue_family_index = ctx->capabilities.queues.present_family;
		const VkPresentModeKHR requested_present_mode = vulkan_requested_present_mode();
		if (ctx->present_mode != requested_present_mode)
		{
			printf("Requested present mode '%s' is unavailable; falling back to '%s'\n",
				vulkan_present_mode_name(requested_present_mode), vulkan_present_mode_name(ctx->present_mode));
		}
		printf("GPU: %s | graphics queue %u | present queue %u | present mode %s\n",
			ctx->physical_device_properties.deviceName, ctx->graphics_queue_family_index,
			ctx->present_queue_family_index, vulkan_present_mode_name(ctx->present_mode));
	}

	// Logical device + queue
	{
		f32 queue_priority = 1.0f;
		VkDeviceQueueCreateInfo queue_create_infos[2] = {{
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = ctx->graphics_queue_family_index,
			.queueCount = 1,
			.pQueuePriorities = &queue_priority,
		}};
		u32 queue_create_info_count = 1;
		if (ctx->present_queue_family_index != ctx->graphics_queue_family_index)
		{
			queue_create_infos[1] = (VkDeviceQueueCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = ctx->present_queue_family_index,
				.queueCount = 1,
				.pQueuePriorities = &queue_priority,
			};
			queue_create_info_count = 2;
		}

		std::vector<const char*> device_extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
		if (ctx->capabilities.portability_subset_extension)
			device_extensions.push_back("VK_KHR_portability_subset");

		VkPhysicalDeviceVulkan12Features enabled_features_1_2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			.shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
			.descriptorBindingPartiallyBound = VK_TRUE,
		};

		VkPhysicalDeviceVulkan13Features enabled_features_1_3 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
			.pNext = &enabled_features_1_2,
			// glslc's vulkan1.3 target compiles `discard` to demote-to-helper
			.shaderDemoteToHelperInvocation = VK_TRUE,
			.synchronization2 = VK_TRUE,
			.dynamicRendering = VK_TRUE,
		};

		VkDeviceCreateInfo device_create_info = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext = &enabled_features_1_3,
			.queueCreateInfoCount = queue_create_info_count,
			.pQueueCreateInfos = queue_create_infos,
			.enabledExtensionCount = (u32)device_extensions.size(),
			.ppEnabledExtensionNames = device_extensions.data(),
		};

		VK_CHECK(vkCreateDevice(ctx->physical_device, &device_create_info, nullptr, &ctx->device));
		volkLoadDevice(ctx->device);
		vkGetDeviceQueue(ctx->device, ctx->graphics_queue_family_index, 0, &ctx->graphics_queue);
		vkGetDeviceQueue(ctx->device, ctx->present_queue_family_index, 0, &ctx->present_queue);
		vulkan_set_object_name(ctx, VK_OBJECT_TYPE_QUEUE, (u64)ctx->graphics_queue, "Graphics Queue");
		if (ctx->present_queue != ctx->graphics_queue)
			vulkan_set_object_name(ctx, VK_OBJECT_TYPE_QUEUE, (u64)ctx->present_queue, "Present Queue");
	}

	vulkan_context_create_pipeline_cache(ctx);

	// VMA allocator (volk-compatible: it fetches everything from these two entry points)
	{
		VmaVulkanFunctions vma_vulkan_functions = {
			.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
			.vkGetDeviceProcAddr = vkGetDeviceProcAddr,
		};

		VmaAllocatorCreateInfo allocator_create_info = {
			.physicalDevice = ctx->physical_device,
			.device = ctx->device,
			.pVulkanFunctions = &vma_vulkan_functions,
			.instance = ctx->instance,
			.vulkanApiVersion = VK_API_VERSION_1_3,
		};

		VK_CHECK(vmaCreateAllocator(&allocator_create_info, &ctx->allocator));
	}

	vulkan_context_create_swapchain(ctx);

	// Command pool + per-frame command buffers and sync objects
	{
		VkCommandPoolCreateInfo command_pool_create_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = ctx->graphics_queue_family_index,
		};
		VK_CHECK(vkCreateCommandPool(ctx->device, &command_pool_create_info, nullptr, &ctx->command_pool));

		VkCommandBufferAllocateInfo command_buffer_allocate_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = ctx->command_pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = MAX_FRAMES_IN_FLIGHT,
		};
		VK_CHECK(vkAllocateCommandBuffers(ctx->device, &command_buffer_allocate_info, ctx->command_buffers));

		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			char command_name[64];
			snprintf(command_name, sizeof(command_name), "Frame %u Primary Command Buffer", frame_idx);
			vulkan_set_object_name(ctx, VK_OBJECT_TYPE_COMMAND_BUFFER, (u64)ctx->command_buffers[frame_idx], command_name);
			const VkSemaphoreCreateInfo semaphore_create_info = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			};
			VK_CHECK(vkCreateSemaphore(ctx->device, &semaphore_create_info, nullptr, &ctx->image_available_semaphores[frame_idx]));

			VkFenceCreateInfo fence_create_info = {
				.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
				.flags = VK_FENCE_CREATE_SIGNALED_BIT,
			};
			VK_CHECK(vkCreateFence(ctx->device, &fence_create_info, nullptr, &ctx->frame_fences[frame_idx]));
			char fence_name[64];
			snprintf(fence_name, sizeof(fence_name), "Frame %u Fence", frame_idx);
			vulkan_set_object_name(ctx, VK_OBJECT_TYPE_FENCE, (u64)ctx->frame_fences[frame_idx], fence_name);
			char semaphore_name[64];
			snprintf(semaphore_name, sizeof(semaphore_name), "Frame %u Image Available", frame_idx);
			vulkan_set_object_name(ctx, VK_OBJECT_TYPE_SEMAPHORE, (u64)ctx->image_available_semaphores[frame_idx], semaphore_name);
		}
	}

	// GPU timestamp queries (feeds the GpuTimings system in core/timings.h)
	{
		VkPhysicalDeviceProperties properties = {};
		vkGetPhysicalDeviceProperties(ctx->physical_device, &properties);
		ctx->timestamp_period_ns = properties.limits.timestampPeriod;

		u32 queue_family_count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &queue_family_count, nullptr);
		std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
		vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &queue_family_count, queue_families.data());

		const u32 timestamp_valid_bits = queue_families[ctx->graphics_queue_family_index].timestampValidBits;
		ctx->timestamps_supported = timestamp_valid_bits != 0 && ctx->timestamp_period_ns > 0.0f;

		if (ctx->timestamps_supported)
		{
			for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
			{
				VkQueryPoolCreateInfo query_pool_create_info = {
					.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
					.queryType = VK_QUERY_TYPE_TIMESTAMP,
					.queryCount = GPU_TIMESTAMP_QUERY_COUNT,
				};
				VK_CHECK(vkCreateQueryPool(ctx->device, &query_pool_create_info, nullptr, &ctx->timestamp_pools[frame_idx]));
				char query_name[64];
				snprintf(query_name, sizeof(query_name), "Frame %u GPU Timestamp Pool", frame_idx);
				vulkan_set_object_name(ctx, VK_OBJECT_TYPE_QUERY_POOL, (u64)ctx->timestamp_pools[frame_idx], query_name);
			}
			gpu_timings_set_available(true);
		}
		else
		{
			gpu_timings_set_available(false, "timestamp queries unsupported on the graphics queue");
		}
	}
}

// Brackets a GPU-timed scope (used by RenderPass::execute). Returns the slot
// or -1 when unsupported/full — pass the result to gpu_timestamps_end_scope.
i32 gpu_timestamps_begin_scope(VulkanContext* ctx, const char* in_name)
{
	if (!ctx->timestamps_supported)
	{
		return -1;
	}

	VulkanContext::GpuTimestampFrameState& frame_state = ctx->timestamp_frames[ctx->frame_index];
	if (frame_state.scope_count >= MAX_GPU_TIMED_SCOPES)
	{
		return -1;
	}

	const i32 slot = frame_state.scope_count++;
	snprintf(frame_state.scope_names[slot], CPU_TIMINGS_MAX_NAME_LENGTH, "%s", in_name ? in_name : "(unnamed)");

	vkCmdWriteTimestamp2(
		ctx->command_buffers[ctx->frame_index],
		VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
		ctx->timestamp_pools[ctx->frame_index],
		2 + slot * 2
	);
	return slot;
}

void gpu_timestamps_end_scope(VulkanContext* ctx, i32 in_slot)
{
	if (in_slot < 0)
	{
		return;
	}

	vkCmdWriteTimestamp2(
		ctx->command_buffers[ctx->frame_index],
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		ctx->timestamp_pools[ctx->frame_index],
		3 + in_slot * 2
	);
}

// Reads this slot's queries from its last submission (safe: the slot's fence
// was just waited) and forwards frame + per-pass events to GpuTimings
void gpu_timestamps_harvest(VulkanContext* ctx)
{
	VulkanContext::GpuTimestampFrameState& frame_state = ctx->timestamp_frames[ctx->frame_index];
	#if !defined(WITH_DEBUG_UI) || !WITH_DEBUG_UI
	frame_state.submitted = false;
	return;
	#else
	if (!ctx->timestamps_supported || !frame_state.submitted)
	{
		return;
	}
	frame_state.submitted = false;

	const u32 query_count = 2 + (u32) frame_state.scope_count * 2;
	u64 results[GPU_TIMESTAMP_QUERY_COUNT] = {};
	VK_CHECK(vkGetQueryPoolResults(
		ctx->device,
		ctx->timestamp_pools[ctx->frame_index],
		0, query_count,
		sizeof(results), results, sizeof(u64),
		VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT
	));

	const f64 ticks_to_ms = (f64) ctx->timestamp_period_ns / 1000000.0;
	const u64 frame_start = results[0];
	const u64 frame_end = results[1];

	GpuTimingEvent events[1 + MAX_GPU_TIMED_SCOPES] = {};

	GpuTimingEvent& frame_event = events[0];
	gpu_timing_event_set_name(frame_event, "GPU Frame");
	frame_event.depth = 0;
	frame_event.parent_index = -1;
	frame_event.lane_index = 0;
	frame_event.type = GpuTimingEventType::Frame;
	frame_event.timestamp_source = GpuTimingTimestampSource::CommandBuffer;
	frame_event.timestamp_confidence = GpuTimingTimestampConfidence::Authoritative;
	frame_event.start_offset_ms = 0.0;
	frame_event.elapsed_ms = (f64)(frame_end - frame_start) * ticks_to_ms;
	frame_event.submission_id = frame_state.cpu_frame_index;
	frame_event.command_buffer_id = ctx->frame_index;
	frame_event.command_order_index = 0;
	snprintf(frame_event.writes, sizeof(frame_event.writes), "Swapchain image %u", ctx->swapchain_image_index);
	frame_event.valid = true;

	for (i32 scope_idx = 0; scope_idx < frame_state.scope_count; ++scope_idx)
	{
		const u64 scope_start = results[2 + scope_idx * 2];
		const u64 scope_end = results[3 + scope_idx * 2];

		GpuTimingEvent& scope_event = events[1 + scope_idx];
		gpu_timing_event_set_name(scope_event, frame_state.scope_names[scope_idx]);
		scope_event.depth = 1;
		scope_event.parent_index = 0;
		scope_event.lane_index = 0;
		scope_event.type = GpuTimingEventType::RenderPass;
		scope_event.timestamp_source = GpuTimingTimestampSource::CommandBuffer;
		// TOP/ALL_COMMANDS pairs can overlap neighboring GPU work — honest label
		scope_event.timestamp_confidence = GpuTimingTimestampConfidence::Sampled;
		scope_event.start_offset_ms = (f64)(scope_start - frame_start) * ticks_to_ms;
		scope_event.elapsed_ms = (f64)(scope_end - scope_start) * ticks_to_ms;
		scope_event.submission_id = frame_state.cpu_frame_index;
		scope_event.command_buffer_id = ctx->frame_index;
		scope_event.command_order_index = scope_idx + 1;
		snprintf(scope_event.writes, sizeof(scope_event.writes), "%s attachments", frame_state.scope_names[scope_idx]);
		scope_event.valid = true;
	}

	gpu_timings_record_completed_frame_events(frame_state.cpu_frame_index, events, 1 + frame_state.scope_count);

	// Periodic debug print; the ImGui profiler is the interactive consumer.
	static const bool print_timings = getenv("GAME2_PRINT_GPU_TIMINGS") != nullptr;
	if (print_timings && (ctx->frame_number % 120) == 0)
	{
		printf("GPU Frame %.3fms", frame_event.elapsed_ms);
		for (i32 scope_idx = 0; scope_idx < frame_state.scope_count; ++scope_idx)
		{
			printf(" | %s %.3fms", frame_state.scope_names[scope_idx], events[1 + scope_idx].elapsed_ms);
		}
		printf("\n");
	}
	#endif
}

// Records and submits a one-shot command buffer, waiting for completion.
// Main-thread only; used for staging uploads.
void vulkan_context_immediate_submit(VulkanContext* ctx, const std::function<void(VkCommandBuffer)>& in_record)
{
	ctx->metrics.immediate_submit_count += 1;
	VkCommandBufferAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = ctx->command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer command_buffer;
	VK_CHECK(vkAllocateCommandBuffers(ctx->device, &allocate_info, &command_buffer));

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));
	in_record(command_buffer);
	VK_CHECK(vkEndCommandBuffer(command_buffer));

	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &command_buffer,
	};
	VK_CHECK(vkQueueSubmit(ctx->graphics_queue, 1, &submit_info, VK_NULL_HANDLE));
	VK_CHECK(vulkan_queue_wait_idle(ctx));

	vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &command_buffer);
}

// Creates a sampled image and uploads pixel data through a staging buffer
// (main thread only — submits on the graphics queue and waits idle).
// Ends in SHADER_READ_ONLY_OPTIMAL. Lives here (not gpu_image.h) because it
// needs VulkanContext + immediate_submit.
GpuImage gpu_image_create_from_data(
	VulkanContext* ctx,
	u32 in_width,
	u32 in_height,
	VkFormat in_format,
	const void* in_pixels,
	u64 in_byte_count
)
{
	GpuImage image = gpu_image_create(ctx->allocator, ctx->device, (GpuImageDesc) {
		.width = in_width,
		.height = in_height,
		.format = in_format,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
		.label = "Uploaded Scene Image",
	});

	VkBufferCreateInfo staging_create_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = in_byte_count,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VmaAllocationCreateInfo staging_allocation_create_info = {
		.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
			   | VMA_ALLOCATION_CREATE_MAPPED_BIT,
		.usage = VMA_MEMORY_USAGE_AUTO,
	};

	VkBuffer staging_buffer = VK_NULL_HANDLE;
	VmaAllocation staging_allocation = VK_NULL_HANDLE;
	VmaAllocationInfo staging_allocation_info = {};
	VK_CHECK(vmaCreateBuffer(
		ctx->allocator,
		&staging_create_info,
		&staging_allocation_create_info,
		&staging_buffer,
		&staging_allocation,
		&staging_allocation_info
	));
	memcpy(staging_allocation_info.pMappedData, in_pixels, in_byte_count);
	VK_CHECK(vmaFlushAllocation(ctx->allocator, staging_allocation, 0, in_byte_count));
	ctx->metrics.upload_bytes += in_byte_count;
	vmaSetAllocationName(ctx->allocator, staging_allocation, "Image Upload Staging");
	vulkan_set_object_name(ctx, VK_OBJECT_TYPE_BUFFER, (u64)staging_buffer, "Image Upload Staging");

	vulkan_context_immediate_submit(ctx, [&](VkCommandBuffer in_command_buffer)
	{
		gpu_image_transition(in_command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, /*in_discard_contents*/ true);

		VkBufferImageCopy copy_region = {
			.bufferOffset = 0,
			.imageSubresource = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
			.imageOffset = { 0, 0, 0 },
			.imageExtent = { in_width, in_height, 1 },
		};
		vkCmdCopyBufferToImage(in_command_buffer, staging_buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

		gpu_image_transition(in_command_buffer, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	// immediate_submit waited for the queue — staging can go immediately
	vmaDestroyBuffer(ctx->allocator, staging_buffer, staging_allocation);

	return image;
}

// Queue a buffer for destruction once no in-flight frame can reference it
void vulkan_context_deferred_destroy_buffer(VulkanContext* ctx, VkBuffer in_buffer, VmaAllocation in_allocation)
{
	ctx->deletion_queue.add((PendingBufferDelete) {
		.buffer = in_buffer,
		.allocation = in_allocation,
		.frame_number = ctx->frame_number,
	});
}

void vulkan_context_flush_deletion_queue(VulkanContext* ctx, bool in_force_all)
{
	for (i64 delete_idx = (i64) ctx->deletion_queue.length() - 1; delete_idx >= 0; --delete_idx)
	{
		PendingBufferDelete pending = ctx->deletion_queue[(i32) delete_idx];
		if (in_force_all || pending.frame_number + MAX_FRAMES_IN_FLIGHT <= ctx->frame_number)
		{
			vmaDestroyBuffer(ctx->allocator, pending.buffer, pending.allocation);

			// Swap-remove: order doesn't matter for pending deletes
			ctx->deletion_queue[(i32) delete_idx] = ctx->deletion_queue.last();
			ctx->deletion_queue.pop();
		}
	}
}

// Waits for the frame slot, acquires a swapchain image, and begins the command
// buffer with the swapchain image transitioned to COLOR_ATTACHMENT_OPTIMAL.
// Returns false if the frame should be skipped (swapchain was just recreated).
bool vulkan_context_begin_frame(VulkanContext* ctx)
{
	if (ctx->needs_resize)
	{
		vulkan_context_recreate_swapchain(ctx);
	}

	VK_CHECK(vkWaitForFences(ctx->device, 1, &ctx->frame_fences[ctx->frame_index], VK_TRUE, UINT64_MAX));

	gpu_timestamps_harvest(ctx);

	vulkan_context_flush_deletion_queue(ctx, false);

	VkResult acquire_result = vkAcquireNextImageKHR(
		ctx->device,
		ctx->swapchain,
		UINT64_MAX,
		ctx->image_available_semaphores[ctx->frame_index],
		VK_NULL_HANDLE,
		&ctx->swapchain_image_index
	);

	if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		vulkan_context_recreate_swapchain(ctx);
		return false;
	}
	assert(acquire_result == VK_SUCCESS || acquire_result == VK_SUBOPTIMAL_KHR);

	// Only reset the fence once we're definitely submitting this frame
	VK_CHECK(vkResetFences(ctx->device, 1, &ctx->frame_fences[ctx->frame_index]));

	VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];
	VK_CHECK(vkResetCommandBuffer(command_buffer, 0));

	VkCommandBufferBeginInfo command_buffer_begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK(vkBeginCommandBuffer(command_buffer, &command_buffer_begin_info));

	// Start this frame's GPU timing (pool was harvested above; reset must be
	// recorded, hostQueryReset is deliberately not enabled)
	if (ctx->timestamps_supported)
	{
		vkCmdResetQueryPool(command_buffer, ctx->timestamp_pools[ctx->frame_index], 0, GPU_TIMESTAMP_QUERY_COUNT);
		vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, ctx->timestamp_pools[ctx->frame_index], 0);

		VulkanContext::GpuTimestampFrameState& frame_state = ctx->timestamp_frames[ctx->frame_index];
		frame_state.scope_count = 0;
		frame_state.cpu_frame_index = cpu_timings_get_current_frame_index();
		frame_state.submitted = false;
	}

	// Swapchain image: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (contents are cleared each frame)
	VkImageMemoryBarrier2 to_color_attachment = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = 0,
		.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = ctx->swapchain_images[ctx->swapchain_image_index],
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};

	VkDependencyInfo begin_dependency_info = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &to_color_attachment,
	};
	vkCmdPipelineBarrier2(command_buffer, &begin_dependency_info);

	return true;
}

void vulkan_context_dump_frame(VulkanContext* ctx, const char* in_path);

void vulkan_context_end_frame(VulkanContext* ctx)
{
	VkCommandBuffer command_buffer = ctx->command_buffers[ctx->frame_index];

	// Swapchain image: COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC
	VkImageMemoryBarrier2 to_present = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
		.dstStageMask = 0,
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = ctx->swapchain_images[ctx->swapchain_image_index],
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};

	VkDependencyInfo end_dependency_info = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &to_present,
	};
	vkCmdPipelineBarrier2(command_buffer, &end_dependency_info);

	// Close this frame's GPU timing span
	if (ctx->timestamps_supported)
	{
		vkCmdWriteTimestamp2(command_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ctx->timestamp_pools[ctx->frame_index], 1);
	}

	VK_CHECK(vkEndCommandBuffer(command_buffer));

	VkSemaphoreSubmitInfo wait_semaphore_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = ctx->image_available_semaphores[ctx->frame_index],
		.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
	};

	VkSemaphoreSubmitInfo signal_semaphore_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.semaphore = ctx->render_finished_semaphores[ctx->swapchain_image_index],
		// Presentation must wait for the final COLOR_ATTACHMENT -> PRESENT
		// transition as well as the rendering itself.
		.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
	};

	VkCommandBufferSubmitInfo command_buffer_submit_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.commandBuffer = command_buffer,
	};

	VkSubmitInfo2 submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.waitSemaphoreInfoCount = 1,
		.pWaitSemaphoreInfos = &wait_semaphore_info,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &command_buffer_submit_info,
		.signalSemaphoreInfoCount = 1,
		.pSignalSemaphoreInfos = &signal_semaphore_info,
	};

	VK_CHECK(vkQueueSubmit2(ctx->graphics_queue, 1, &submit_info, ctx->frame_fences[ctx->frame_index]));

	if (ctx->timestamps_supported)
	{
		ctx->timestamp_frames[ctx->frame_index].submitted = true;
	}

	if (ctx->pending_frame_dump != nullptr)
	{
		vulkan_context_dump_frame(ctx, ctx->pending_frame_dump);
		ctx->pending_frame_dump = nullptr;
	}

	VkPresentInfoKHR present_info = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &ctx->render_finished_semaphores[ctx->swapchain_image_index],
		.swapchainCount = 1,
		.pSwapchains = &ctx->swapchain,
		.pImageIndices = &ctx->swapchain_image_index,
	};

	VkResult present_result = vkQueuePresentKHR(ctx->present_queue, &present_info);
	if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR)
	{
		ctx->needs_resize = true;
	}
	else
	{
		assert(present_result == VK_SUCCESS);
	}

	ctx->frame_number++;
	ctx->frame_index = (ctx->frame_index + 1) % MAX_FRAMES_IN_FLIGHT;
}

// Debug helper: copies the last-presented swapchain image to a PPM file.
// Used for automated visual verification (set GAME2_SCREENSHOT=<path>).
void vulkan_context_dump_frame(VulkanContext* ctx, const char* in_path)
{
	if (!ctx->screenshot_supported)
	{
		printf("Frame dump skipped: this surface does not support transfer-source swapchain images\n");
		return;
	}
	VK_CHECK(vulkan_device_wait_idle(ctx));

	VkImage source_image = ctx->swapchain_images[ctx->swapchain_image_index];
	const u32 width = ctx->swapchain_extent.width;
	const u32 height = ctx->swapchain_extent.height;
	const u64 buffer_size = (u64) width * height * 4;

	VkBufferCreateInfo buffer_create_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = buffer_size,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VmaAllocationCreateInfo allocation_create_info = {
		.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
		.usage = VMA_MEMORY_USAGE_AUTO,
	};
	VkBuffer readback_buffer = VK_NULL_HANDLE;
	VmaAllocation readback_allocation = VK_NULL_HANDLE;
	VmaAllocationInfo readback_allocation_info = {};
	VK_CHECK(vmaCreateBuffer(ctx->allocator, &buffer_create_info, &allocation_create_info, &readback_buffer, &readback_allocation, &readback_allocation_info));

	VkCommandBufferAllocateInfo command_buffer_allocate_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = ctx->command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkCommandBuffer command_buffer;
	VK_CHECK(vkAllocateCommandBuffers(ctx->device, &command_buffer_allocate_info, &command_buffer));

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

	VkImageMemoryBarrier2 to_transfer_src = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		.srcAccessMask = 0,
		.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
		.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = source_image,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};
	VkDependencyInfo dependency_info = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &to_transfer_src,
	};
	vkCmdPipelineBarrier2(command_buffer, &dependency_info);

	VkBufferImageCopy copy_region = {
		.bufferOffset = 0,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
		.imageOffset = { 0, 0, 0 },
		.imageExtent = { width, height, 1 },
	};
	vkCmdCopyImageToBuffer(command_buffer, source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1, &copy_region);

	VkImageMemoryBarrier2 back_to_present = to_transfer_src;
	back_to_present.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	back_to_present.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
	back_to_present.dstStageMask = 0;
	back_to_present.dstAccessMask = 0;
	back_to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	back_to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkDependencyInfo back_dependency_info = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &back_to_present,
	};
	vkCmdPipelineBarrier2(command_buffer, &back_dependency_info);

	VK_CHECK(vkEndCommandBuffer(command_buffer));

	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &command_buffer,
	};
	VK_CHECK(vkQueueSubmit(ctx->graphics_queue, 1, &submit_info, VK_NULL_HANDLE));
	VK_CHECK(vulkan_queue_wait_idle(ctx));
	vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &command_buffer);

	// Write PPM from the negotiated common 8-bit swapchain format.
	FILE* file = fopen(in_path, "wb");
	if (file)
	{
		fprintf(file, "P6\n%u %u\n255\n", width, height);
		const u8* pixels = (const u8*) readback_allocation_info.pMappedData;
		VK_CHECK(vmaInvalidateAllocation(ctx->allocator, readback_allocation, 0, VK_WHOLE_SIZE));
		for (u64 pixel_idx = 0; pixel_idx < (u64) width * height; ++pixel_idx)
		{
			const u8* pixel = &pixels[pixel_idx * 4];
			u8 rgb[3] = {};
			if (vulkan_format_is_bgra8(ctx->surface_format.format))
			{
				rgb[0] = pixel[2]; rgb[1] = pixel[1]; rgb[2] = pixel[0];
			}
			else
			{
				rgb[0] = pixel[0]; rgb[1] = pixel[1]; rgb[2] = pixel[2];
			}
			fwrite(rgb, 1, 3, file);
		}
		fclose(file);
		printf("Wrote frame dump to %s\n", in_path);
	}
	else
	{
		printf("Failed to open frame dump path %s\n", in_path);
	}

	vmaDestroyBuffer(ctx->allocator, readback_buffer, readback_allocation);
}

void vulkan_context_shutdown(VulkanContext* ctx)
{
	vulkan_device_wait_idle(ctx);
	vulkan_context_save_pipeline_cache(ctx);

	vulkan_context_flush_deletion_queue(ctx, true);

	for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
	{
		vkDestroySemaphore(ctx->device, ctx->image_available_semaphores[frame_idx], nullptr);
		vkDestroyFence(ctx->device, ctx->frame_fences[frame_idx], nullptr);
	}
	for (VkSemaphore semaphore : ctx->render_finished_semaphores)
	{
		vkDestroySemaphore(ctx->device, semaphore, nullptr);
	}
	ctx->render_finished_semaphores.clear();

	if (ctx->timestamps_supported)
	{
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			vkDestroyQueryPool(ctx->device, ctx->timestamp_pools[frame_idx], nullptr);
		}
	}

	vkFreeCommandBuffers(ctx->device, ctx->command_pool, MAX_FRAMES_IN_FLIGHT, ctx->command_buffers);
	vkDestroyCommandPool(ctx->device, ctx->command_pool, nullptr);

	vulkan_context_destroy_swapchain_resources(ctx);
	vkDestroySwapchainKHR(ctx->device, ctx->swapchain, nullptr);
	vkDestroyPipelineCache(ctx->device, ctx->pipeline_cache, nullptr);

	vmaDestroyAllocator(ctx->allocator);
	vkDestroyDevice(ctx->device, nullptr);
	vkDestroySurfaceKHR(ctx->instance, ctx->surface, nullptr);
	if (ctx->debug_messenger != VK_NULL_HANDLE)
		vkDestroyDebugUtilsMessengerEXT(ctx->instance, ctx->debug_messenger, nullptr);
	vkDestroyInstance(ctx->instance, nullptr);
}
