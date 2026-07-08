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
};

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
