#pragma once

#include "core/types.h"
#include "core/dynamic_array.h"

// Minimal VMA-backed image. Used for the depth buffer now; offscreen render
// targets for future passes will reuse this.

struct GpuImageDesc
{
	u32 width;
	u32 height;
	VkFormat format;
	VkImageUsageFlags usage;
	VkImageAspectFlags aspect;
	u32 array_layers = 1;	// > 1 = 2D array (per-layer attachment views + array sampled view)
	u32 mip_levels = 1;		// whole-image view samples every mip; mip_views render one mip at a time
	bool cubemap = false;	// requires array_layers == 6; sampled view is CUBE
	const char* label = nullptr;
};

struct GpuImage
{
	VkImage image = VK_NULL_HANDLE;

	// Whole-image view: 2D for single-layer images, 2D_ARRAY when layered
	// (this is what gets sampled)
	VkImageView view = VK_NULL_HANDLE;

	// Per-layer 2D views for rendering into individual slices (layered only)
	DynamicArray<VkImageView> layer_views;

	// Per-mip views for rendering into an individual mip (mipped images only)
	DynamicArray<VkImageView> mip_views;

	VmaAllocation allocation = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkExtent2D extent = {};
	u32 array_layers = 1;
	u32 mip_levels = 1;
	VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
	u64 generation = 0;

	struct ImageSubresourceState
	{
		VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		VkAccessFlags2 access = 0;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};
	// Color/depth/stencil each own mip_levels * array_layers entries.
	DynamicArray<ImageSubresourceState> subresource_states;
};

struct ImageUsage
{
	GpuImage* image = nullptr;
	VkImageSubresourceRange range = {};
	VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	VkAccessFlags2 access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
	VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
	bool discard = false;
};

struct BufferUsage
{
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	VkDeviceSize size = VK_WHOLE_SIZE;
	VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	VkAccessFlags2 access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
};

struct PassResourceUsage
{
	DynamicArray<ImageUsage> images;
	DynamicArray<BufferUsage> buffers;
};

inline u64 gpu_image_next_generation()
{
	static u64 generation = 1;
	return generation++;
}

inline i32 gpu_image_aspect_slot(VkImageAspectFlagBits in_aspect)
{
	switch (in_aspect)
	{
		case VK_IMAGE_ASPECT_COLOR_BIT: return 0;
		case VK_IMAGE_ASPECT_DEPTH_BIT: return 1;
		case VK_IMAGE_ASPECT_STENCIL_BIT: return 2;
		default: return 0;
	}
}

inline size_t gpu_image_state_index(
	const GpuImage& in_image,
	VkImageAspectFlagBits in_aspect,
	u32 in_mip,
	u32 in_layer)
{
	return (size_t)gpu_image_aspect_slot(in_aspect) * in_image.mip_levels * in_image.array_layers
		+ (size_t)in_mip * in_image.array_layers + in_layer;
}

VkImageAspectFlags gpu_image_aspect_for_format(VkFormat in_format)
{
	switch (in_format)
	{
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_D32_SFLOAT:
			return VK_IMAGE_ASPECT_DEPTH_BIT;
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
			return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		default:
			return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}

// Maps a layout to the pipeline stages + accesses that use it, for both the
// src (what must finish/flush) and dst (what must wait) sides of a barrier
void gpu_image_layout_sync_info(VkImageLayout in_layout, VkPipelineStageFlags2* out_stage, VkAccessFlags2* out_access)
{
	switch (in_layout)
	{
		case VK_IMAGE_LAYOUT_UNDEFINED:
			*out_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			*out_access = 0;
			break;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			*out_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			*out_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
			*out_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
			*out_access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			*out_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			*out_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			*out_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			*out_access = VK_ACCESS_2_TRANSFER_READ_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			*out_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			*out_access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			break;
		default:
			*out_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			*out_access = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
			break;
	}
}

inline bool gpu_access_has_write(VkAccessFlags2 in_access)
{
	const VkAccessFlags2 writes =
		VK_ACCESS_2_SHADER_WRITE_BIT
		| VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
		| VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_2_TRANSFER_WRITE_BIT
		| VK_ACCESS_2_HOST_WRITE_BIT
		| VK_ACCESS_2_MEMORY_WRITE_BIT;
	return (in_access & writes) != 0;
}

void gpu_image_apply_usages(
	VkCommandBuffer in_command_buffer,
	const ImageUsage* in_usages,
	u32 in_usage_count)
{
	DynamicArray<VkImageMemoryBarrier2> barriers;
	for (u32 usage_index = 0; usage_index < in_usage_count; ++usage_index)
	{
		const ImageUsage& usage = in_usages[usage_index];
		assert(usage.image && usage.image->image != VK_NULL_HANDLE);
		GpuImage& image = *usage.image;
		const VkImageAspectFlags aspect_mask = usage.range.aspectMask
			? usage.range.aspectMask : image.aspects;
		const u32 first_mip = usage.range.baseMipLevel;
		const u32 mip_count = usage.range.levelCount == VK_REMAINING_MIP_LEVELS
			? image.mip_levels - first_mip : usage.range.levelCount;
		const u32 first_layer = usage.range.baseArrayLayer;
		const u32 layer_count = usage.range.layerCount == VK_REMAINING_ARRAY_LAYERS
			? image.array_layers - first_layer : usage.range.layerCount;

		const VkImageAspectFlagBits aspect_bits[] = {
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_ASPECT_DEPTH_BIT,
			VK_IMAGE_ASPECT_STENCIL_BIT,
		};
		for (VkImageAspectFlagBits aspect : aspect_bits)
		{
			if ((aspect_mask & aspect) == 0) continue;
			for (u32 mip = first_mip; mip < first_mip + mip_count; ++mip)
			{
				for (u32 layer = first_layer; layer < first_layer + layer_count; ++layer)
				{
					GpuImage::ImageSubresourceState& state =
						image.subresource_states[gpu_image_state_index(image, aspect, mip, layer)];
					const bool needs_barrier =
						state.layout != usage.layout
						|| gpu_access_has_write(state.access)
						|| gpu_access_has_write(usage.access)
						|| usage.discard;
					if (needs_barrier)
					{
						barriers.add((VkImageMemoryBarrier2) {
							.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
							.srcStageMask = state.stage,
							.srcAccessMask = state.access,
							.dstStageMask = usage.stage,
							.dstAccessMask = usage.access,
							.oldLayout = usage.discard ? VK_IMAGE_LAYOUT_UNDEFINED : state.layout,
							.newLayout = usage.layout,
							.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
							.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
							.image = image.image,
							.subresourceRange = {
								.aspectMask = aspect,
								.baseMipLevel = mip,
								.levelCount = 1,
								.baseArrayLayer = layer,
								.layerCount = 1,
							},
						});
						state.stage = usage.stage;
						state.access = usage.access;
						state.layout = usage.layout;
					}
					else
					{
						state.stage |= usage.stage;
						state.access |= usage.access;
					}
				}
			}
		}
	}

	if (barriers.empty()) return;
	VkDependencyInfo dependency_info = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = (u32)barriers.length(),
		.pImageMemoryBarriers = barriers.data(),
	};
	vkCmdPipelineBarrier2(in_command_buffer, &dependency_info);
}

// Compatibility wrapper for call sites that have not yet moved their usage
// declaration into a PassResourceUsage batch.
void gpu_image_transition(
	VkCommandBuffer in_command_buffer,
	GpuImage& in_image,
	VkImageLayout in_new_layout,
	bool in_discard_contents = false)
{
	VkPipelineStageFlags2 stage;
	VkAccessFlags2 access;
	gpu_image_layout_sync_info(in_new_layout, &stage, &access);
	ImageUsage usage = {
		.image = &in_image,
		.range = {
			.aspectMask = in_image.aspects,
			.baseMipLevel = 0,
			.levelCount = VK_REMAINING_MIP_LEVELS,
			.baseArrayLayer = 0,
			.layerCount = VK_REMAINING_ARRAY_LAYERS,
		},
		.stage = stage,
		.access = access,
		.layout = in_new_layout,
		.discard = in_discard_contents,
	};
	gpu_image_apply_usages(in_command_buffer, &usage, 1);
}

GpuImage gpu_image_create(VmaAllocator in_allocator, VkDevice in_device, const GpuImageDesc& in_desc)
{
	const u32 array_layers = MAX(in_desc.array_layers, 1u);
	const u32 mip_levels = MAX(in_desc.mip_levels, 1u);

	GpuImage result = {
		.format = in_desc.format,
		.extent = { in_desc.width, in_desc.height },
		.array_layers = array_layers,
		.mip_levels = mip_levels,
		.aspects = in_desc.aspect,
		.generation = gpu_image_next_generation(),
	};
	result.subresource_states.resize(3 * result.mip_levels * result.array_layers);

	assert(!in_desc.cubemap || array_layers == 6);

	VkImageCreateInfo image_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.flags = in_desc.cubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : (VkImageCreateFlags) 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = in_desc.format,
		.extent = {
			.width = in_desc.width,
			.height = in_desc.height,
			.depth = 1,
		},
		.mipLevels = mip_levels,
		.arrayLayers = array_layers,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = in_desc.usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VmaAllocationCreateInfo allocation_create_info = {
		.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
	};

	VK_CHECK(vmaCreateImage(
		in_allocator,
		&image_create_info,
		&allocation_create_info,
		&result.image,
		&result.allocation,
		nullptr
	));
	if (in_desc.label)
	{
		vmaSetAllocationName(in_allocator, result.allocation, in_desc.label);
		if (g_vulkan_debug_utils_enabled && vkSetDebugUtilsObjectNameEXT)
		{
			VkDebugUtilsObjectNameInfoEXT name_info = {
				.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
				.objectType = VK_OBJECT_TYPE_IMAGE,
				.objectHandle = (u64)result.image,
				.pObjectName = in_desc.label,
			};
			vkSetDebugUtilsObjectNameEXT(in_device, &name_info);
		}
	}

	VkImageViewCreateInfo image_view_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = result.image,
		.viewType = in_desc.cubemap
			? VK_IMAGE_VIEW_TYPE_CUBE
			: array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
		.format = in_desc.format,
		.subresourceRange = {
			.aspectMask = in_desc.aspect,
			.baseMipLevel = 0,
			.levelCount = mip_levels,
			.baseArrayLayer = 0,
			.layerCount = array_layers,
		},
	};

	VK_CHECK(vkCreateImageView(in_device, &image_view_create_info, nullptr, &result.view));
	if (in_desc.label && g_vulkan_debug_utils_enabled && vkSetDebugUtilsObjectNameEXT)
	{
		char view_label[192];
		snprintf(view_label, sizeof(view_label), "%s View", in_desc.label);
		VkDebugUtilsObjectNameInfoEXT name_info = {
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.objectType = VK_OBJECT_TYPE_IMAGE_VIEW,
			.objectHandle = (u64)result.view,
			.pObjectName = view_label,
		};
		vkSetDebugUtilsObjectNameEXT(in_device, &name_info);
	}

	// Per-layer 2D views for slice rendering
	if (array_layers > 1)
	{
		for (u32 layer_idx = 0; layer_idx < array_layers; ++layer_idx)
		{
			VkImageViewCreateInfo layer_view_create_info = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = result.image,
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = in_desc.format,
				.subresourceRange = {
					.aspectMask = in_desc.aspect,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = layer_idx,
					.layerCount = 1,
				},
			};
			VkImageView layer_view = VK_NULL_HANDLE;
			VK_CHECK(vkCreateImageView(in_device, &layer_view_create_info, nullptr, &layer_view));
			if (in_desc.label && g_vulkan_debug_utils_enabled && vkSetDebugUtilsObjectNameEXT)
			{
				char layer_label[192];
				snprintf(layer_label, sizeof(layer_label), "%s Layer %u View", in_desc.label, layer_idx);
				VkDebugUtilsObjectNameInfoEXT name_info = {
					.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
					.objectType = VK_OBJECT_TYPE_IMAGE_VIEW,
					.objectHandle = (u64)layer_view,
					.pObjectName = layer_label,
				};
				vkSetDebugUtilsObjectNameEXT(in_device, &name_info);
			}
			result.layer_views.add(layer_view);
		}
	}

	if (mip_levels > 1)
	{
		for (u32 mip_idx = 0; mip_idx < mip_levels; ++mip_idx)
		{
			VkImageViewCreateInfo mip_view_create_info = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = result.image,
				.viewType = array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
				.format = in_desc.format,
				.subresourceRange = {
					.aspectMask = in_desc.aspect,
					.baseMipLevel = mip_idx,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = array_layers,
				},
			};
			VkImageView mip_view = VK_NULL_HANDLE;
			VK_CHECK(vkCreateImageView(in_device, &mip_view_create_info, nullptr, &mip_view));
			if (in_desc.label && g_vulkan_debug_utils_enabled && vkSetDebugUtilsObjectNameEXT)
			{
				char mip_label[192];
				snprintf(mip_label, sizeof(mip_label), "%s Mip %u View", in_desc.label, mip_idx);
				VkDebugUtilsObjectNameInfoEXT name_info = {
					.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
					.objectType = VK_OBJECT_TYPE_IMAGE_VIEW,
					.objectHandle = (u64)mip_view,
					.pObjectName = mip_label,
				};
				vkSetDebugUtilsObjectNameEXT(in_device, &name_info);
			}
			result.mip_views.add(mip_view);
		}
	}

	return result;
}

void gpu_image_destroy(VmaAllocator in_allocator, VkDevice in_device, GpuImage& in_image)
{
	for (VkImageView mip_view : in_image.mip_views)
	{
		vkDestroyImageView(in_device, mip_view, nullptr);
	}
	in_image.mip_views.reset();

	for (VkImageView layer_view : in_image.layer_views)
	{
		vkDestroyImageView(in_device, layer_view, nullptr);
	}
	in_image.layer_views.reset();

	if (in_image.view != VK_NULL_HANDLE)
	{
		vkDestroyImageView(in_device, in_image.view, nullptr);
		in_image.view = VK_NULL_HANDLE;
	}

	if (in_image.image != VK_NULL_HANDLE)
	{
		vmaDestroyImage(in_allocator, in_image.image, in_image.allocation);
		in_image.image = VK_NULL_HANDLE;
		in_image.allocation = VK_NULL_HANDLE;
	}
	in_image.subresource_states.clear();
	in_image.generation = gpu_image_next_generation();
}
