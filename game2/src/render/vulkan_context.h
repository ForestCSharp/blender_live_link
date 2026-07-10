#pragma once

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
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

#include "render/gpu_image.h"

static const u32 MAX_FRAMES_IN_FLIGHT = 2;

// GPU timestamp queries: one pool per frame in flight; query 0/1 span the
// frame, 2+2i / 3+2i bracket timed scope i (one per render pass today)
static constexpr i32 MAX_GPU_TIMED_SCOPES = 16;
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

struct VulkanContext
{
	GLFWwindow* window = nullptr;

	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	u32 graphics_queue_family_index = 0;
	VkSurfaceFormatKHR surface_format = {};
	VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue graphics_queue = VK_NULL_HANDLE;
	VmaAllocator allocator = VK_NULL_HANDLE;

	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkExtent2D swapchain_extent = {};
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
};

// Set by vulkan_context_init; used by GpuBuffer's lazy creation (the way
// sokol's global context backs sg_make_buffer in game/)
static VulkanContext* g_vulkan_context = nullptr;

VKAPI_ATTR VkBool32 VKAPI_CALL vulkan_debug_callback(
	VkDebugUtilsMessageSeverityFlagBitsEXT in_severity,
	VkDebugUtilsMessageTypeFlagsEXT in_type,
	const VkDebugUtilsMessengerCallbackDataEXT* in_callback_data,
	void* in_user_data)
{
	printf("[vulkan] %s\n", in_callback_data->pMessage);
	return VK_FALSE;
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
		extent.width = (u32) framebuffer_width;
		extent.height = (u32) framebuffer_height;
	}
	ctx->swapchain_extent = extent;

	u32 desired_image_count = surface_capabilities.minImageCount + 1;
	if (surface_capabilities.maxImageCount > 0 && desired_image_count > surface_capabilities.maxImageCount)
	{
		desired_image_count = surface_capabilities.maxImageCount;
	}

	VkSwapchainKHR old_swapchain = ctx->swapchain;

	VkSwapchainCreateInfoKHR swapchain_create_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = ctx->surface,
		.minImageCount = desired_image_count,
		.imageFormat = ctx->surface_format.format,
		.imageColorSpace = ctx->surface_format.colorSpace,
		.imageExtent = extent,
		.imageArrayLayers = 1,
		// TRANSFER_SRC enables the debug frame dump (GAME2_SCREENSHOT)
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = surface_capabilities.currentTransform,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
		.presentMode = ctx->present_mode,
		.clipped = VK_TRUE,
		.oldSwapchain = old_swapchain,
	};

	VK_CHECK(vkCreateSwapchainKHR(ctx->device, &swapchain_create_info, nullptr, &ctx->swapchain));

	if (old_swapchain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(ctx->device, old_swapchain, nullptr);
	}

	u32 swapchain_image_count = 0;
	VK_CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &swapchain_image_count, nullptr));
	ctx->swapchain_images.resize(swapchain_image_count);
	VK_CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &swapchain_image_count, ctx->swapchain_images.data()));

	ctx->swapchain_image_views.resize(swapchain_image_count);
	for (u32 image_idx = 0; image_idx < swapchain_image_count; ++image_idx)
	{
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

	vkDeviceWaitIdle(ctx->device);

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
		#if defined(__APPLE__)
		instance_create_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		#endif

		VkApplicationInfo app_info = {
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.pApplicationName = "Blender Game 2",
			.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
			.pEngineName = "Blender Game 2",
			.engineVersion = VK_MAKE_VERSION(1, 0, 0),
			.apiVersion = VK_API_VERSION_1_3,
		};

		// Enable validation only if the layer is actually installed
		const char* validation_layers[] = { "VK_LAYER_KHRONOS_validation" };
		u32 enabled_layer_count = 0;
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

		// GLFW knows the platform surface extensions (VK_KHR_surface + VK_EXT_metal_surface on Mac)
		u32 glfw_extension_count = 0;
		const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);
		assert(glfw_extensions != nullptr);

		std::vector<const char*> instance_extensions(glfw_extensions, glfw_extensions + glfw_extension_count);
		#if defined(__APPLE__)
		instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		#endif
		instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

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

	// Physical device: first one with a usable surface format, present mode, and graphics queue
	{
		u32 physical_device_count = 0;
		vkEnumeratePhysicalDevices(ctx->instance, &physical_device_count, nullptr);
		assert(physical_device_count > 0);
		std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
		vkEnumeratePhysicalDevices(ctx->instance, &physical_device_count, physical_devices.data());

		for (VkPhysicalDevice candidate : physical_devices)
		{
			VkPhysicalDeviceProperties properties = {};
			vkGetPhysicalDeviceProperties(candidate, &properties);

			// Surface format: prefer BGRA8 sRGB
			VkSurfaceFormatKHR chosen_format = {};
			bool found_format = false;
			{
				u32 format_count = 0;
				VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, ctx->surface, &format_count, nullptr));
				if (format_count == 0) { continue; }

				std::vector<VkSurfaceFormatKHR> formats(format_count);
				VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, ctx->surface, &format_count, formats.data()));
				for (const VkSurfaceFormatKHR& format : formats)
				{
					if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					{
						chosen_format = format;
						found_format = true;
						break;
					}
				}
				if (!found_format)
				{
					chosen_format = formats[0];
					found_format = true;
				}
			}

			// Present mode: FIFO is vsync and always available
			VkPresentModeKHR chosen_present_mode = VK_PRESENT_MODE_FIFO_KHR;

			// Graphics queue family
			bool found_graphics_queue = false;
			u32 graphics_queue_family_index = 0;
			{
				u32 queue_family_count = 0;
				vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_family_count, nullptr);
				std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
				vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_family_count, queue_families.data());
				for (u32 queue_family_idx = 0; queue_family_idx < queue_family_count; ++queue_family_idx)
				{
					if (queue_families[queue_family_idx].queueFlags & VK_QUEUE_GRAPHICS_BIT)
					{
						VkBool32 present_supported = VK_FALSE;
						vkGetPhysicalDeviceSurfaceSupportKHR(candidate, queue_family_idx, ctx->surface, &present_supported);
						if (present_supported)
						{
							graphics_queue_family_index = queue_family_idx;
							found_graphics_queue = true;
							break;
						}
					}
				}
			}

			if (found_format && found_graphics_queue)
			{
				ctx->physical_device = candidate;
				ctx->surface_format = chosen_format;
				ctx->present_mode = chosen_present_mode;
				ctx->graphics_queue_family_index = graphics_queue_family_index;
				printf("GPU: %s\n", properties.deviceName);
				break;
			}
		}
		assert(ctx->physical_device != VK_NULL_HANDLE);
	}

	// Logical device + queue
	{
		f32 queue_priority = 1.0f;
		VkDeviceQueueCreateInfo queue_create_info = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueFamilyIndex = ctx->graphics_queue_family_index,
			.queueCount = 1,
			.pQueuePriorities = &queue_priority,
		};

		const char* device_extensions[] = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME,
			#if defined(__APPLE__)
			// Required when the device advertises it (MoltenVK always does)
			"VK_KHR_portability_subset",
			#endif
		};

		VkPhysicalDeviceVulkan12Features enabled_features_1_2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			.descriptorIndexing = VK_TRUE,
			// PARTIALLY_BOUND on the bindless texture array (not implied by
			// descriptorIndexing — must be enabled explicitly)
			.descriptorBindingPartiallyBound = VK_TRUE,
			.descriptorBindingVariableDescriptorCount = VK_TRUE,
			.runtimeDescriptorArray = VK_TRUE,
			.bufferDeviceAddress = VK_TRUE,
		};

		VkPhysicalDeviceVulkan13Features enabled_features_1_3 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
			.pNext = &enabled_features_1_2,
			.synchronization2 = VK_TRUE,
			.dynamicRendering = VK_TRUE,
		};

		VkDeviceCreateInfo device_create_info = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.pNext = &enabled_features_1_3,
			.queueCreateInfoCount = 1,
			.pQueueCreateInfos = &queue_create_info,
			.enabledExtensionCount = sizeof(device_extensions) / sizeof(device_extensions[0]),
			.ppEnabledExtensionNames = device_extensions,
		};

		VK_CHECK(vkCreateDevice(ctx->physical_device, &device_create_info, nullptr, &ctx->device));
		volkLoadDevice(ctx->device);
		vkGetDeviceQueue(ctx->device, ctx->graphics_queue_family_index, 0, &ctx->graphics_queue);
	}

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
			const VkSemaphoreCreateInfo semaphore_create_info = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
			};
			VK_CHECK(vkCreateSemaphore(ctx->device, &semaphore_create_info, nullptr, &ctx->image_available_semaphores[frame_idx]));

			VkFenceCreateInfo fence_create_info = {
				.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
				.flags = VK_FENCE_CREATE_SIGNALED_BIT,
			};
			VK_CHECK(vkCreateFence(ctx->device, &fence_create_info, nullptr, &ctx->frame_fences[frame_idx]));
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
	frame_event.type = GpuTimingEventType::Frame;
	frame_event.timestamp_source = GpuTimingTimestampSource::CommandBuffer;
	frame_event.timestamp_confidence = GpuTimingTimestampConfidence::Authoritative;
	frame_event.start_offset_ms = 0.0;
	frame_event.elapsed_ms = (f64)(frame_end - frame_start) * ticks_to_ms;
	frame_event.valid = true;

	for (i32 scope_idx = 0; scope_idx < frame_state.scope_count; ++scope_idx)
	{
		const u64 scope_start = results[2 + scope_idx * 2];
		const u64 scope_end = results[3 + scope_idx * 2];

		GpuTimingEvent& scope_event = events[1 + scope_idx];
		gpu_timing_event_set_name(scope_event, frame_state.scope_names[scope_idx]);
		scope_event.depth = 1;
		scope_event.parent_index = 0;
		scope_event.type = GpuTimingEventType::RenderPass;
		scope_event.timestamp_source = GpuTimingTimestampSource::CommandBuffer;
		// TOP/ALL_COMMANDS pairs can overlap neighboring GPU work — honest label
		scope_event.timestamp_confidence = GpuTimingTimestampConfidence::Sampled;
		scope_event.start_offset_ms = (f64)(scope_start - frame_start) * ticks_to_ms;
		scope_event.elapsed_ms = (f64)(scope_end - scope_start) * ticks_to_ms;
		scope_event.valid = true;
	}

	gpu_timings_record_completed_frame_events(frame_state.cpu_frame_index, events, 1 + frame_state.scope_count);

	// Periodic debug print (Phase 4's profiler UI is the real consumer)
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
}

// Records and submits a one-shot command buffer, waiting for completion.
// Main-thread only; used for staging uploads.
void vulkan_context_immediate_submit(VulkanContext* ctx, const std::function<void(VkCommandBuffer)>& in_record)
{
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
	VK_CHECK(vkQueueWaitIdle(ctx->graphics_queue));

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
		.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
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

	VkResult present_result = vkQueuePresentKHR(ctx->graphics_queue, &present_info);
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
	vkDeviceWaitIdle(ctx->device);

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
	vkQueueWaitIdle(ctx->graphics_queue);
	vkFreeCommandBuffers(ctx->device, ctx->command_pool, 1, &command_buffer);

	// Write PPM (swapchain is BGRA)
	FILE* file = fopen(in_path, "wb");
	if (file)
	{
		fprintf(file, "P6\n%u %u\n255\n", width, height);
		const u8* pixels = (const u8*) readback_allocation_info.pMappedData;
		for (u64 pixel_idx = 0; pixel_idx < (u64) width * height; ++pixel_idx)
		{
			const u8* bgra = &pixels[pixel_idx * 4];
			u8 rgb[3] = { bgra[2], bgra[1], bgra[0] };
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
	vkDeviceWaitIdle(ctx->device);

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

	vmaDestroyAllocator(ctx->allocator);
	vkDestroyDevice(ctx->device, nullptr);
	vkDestroySurfaceKHR(ctx->instance, ctx->surface, nullptr);
	vkDestroyDebugUtilsMessengerEXT(ctx->instance, ctx->debug_messenger, nullptr);
	vkDestroyInstance(ctx->instance, nullptr);
}
