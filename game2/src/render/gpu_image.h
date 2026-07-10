#pragma once

#include "core/types.h"

// Minimal VMA-backed image. Used for the depth buffer now; offscreen render
// targets for future passes will reuse this.

struct GpuImageDesc
{
	u32 width;
	u32 height;
	VkFormat format;
	VkImageUsageFlags usage;
	VkImageAspectFlags aspect;
};

struct GpuImage
{
	VkImage image = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VmaAllocation allocation = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkExtent2D extent = {};

	// Tracked so gpu_image_transition can derive correct src barriers
	VkImageLayout current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

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

// Transitions an image and updates its tracked layout. Must be recorded
// OUTSIDE vkCmdBeginRendering/vkCmdEndRendering. in_discard_contents uses
// oldLayout = UNDEFINED (contents dropped), which also guarantees a fresh
// execution dependency each frame for clear-every-frame attachments.
void gpu_image_transition(
	VkCommandBuffer in_command_buffer,
	GpuImage& in_image,
	VkImageLayout in_new_layout,
	bool in_discard_contents = false
)
{
	if (!in_discard_contents && in_image.current_layout == in_new_layout)
	{
		return;
	}

	const VkImageLayout old_layout = in_discard_contents ? VK_IMAGE_LAYOUT_UNDEFINED : in_image.current_layout;

	VkPipelineStageFlags2 src_stage;
	VkAccessFlags2 src_access;
	gpu_image_layout_sync_info(old_layout, &src_stage, &src_access);

	VkPipelineStageFlags2 dst_stage;
	VkAccessFlags2 dst_access;
	gpu_image_layout_sync_info(in_new_layout, &dst_stage, &dst_access);

	VkImageMemoryBarrier2 barrier = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.srcStageMask = src_stage,
		.srcAccessMask = src_access,
		.dstStageMask = dst_stage,
		.dstAccessMask = dst_access,
		.oldLayout = old_layout,
		.newLayout = in_new_layout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = in_image.image,
		.subresourceRange = {
			.aspectMask = gpu_image_aspect_for_format(in_image.format),
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};

	VkDependencyInfo dependency_info = {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.imageMemoryBarrierCount = 1,
		.pImageMemoryBarriers = &barrier,
	};
	vkCmdPipelineBarrier2(in_command_buffer, &dependency_info);

	in_image.current_layout = in_new_layout;
}

GpuImage gpu_image_create(VmaAllocator in_allocator, VkDevice in_device, const GpuImageDesc& in_desc)
{
	GpuImage result = {
		.format = in_desc.format,
		.extent = { in_desc.width, in_desc.height },
	};

	VkImageCreateInfo image_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = in_desc.format,
		.extent = {
			.width = in_desc.width,
			.height = in_desc.height,
			.depth = 1,
		},
		.mipLevels = 1,
		.arrayLayers = 1,
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

	VkImageViewCreateInfo image_view_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = result.image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = in_desc.format,
		.subresourceRange = {
			.aspectMask = in_desc.aspect,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};

	VK_CHECK(vkCreateImageView(in_device, &image_view_create_info, nullptr, &result.view));

	return result;
}

void gpu_image_destroy(VmaAllocator in_allocator, VkDevice in_device, GpuImage& in_image)
{
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
}
