#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/gpu_buffer.h"

// GLSL-shared struct definitions (PerFrameData, ObjectData)
#include "shader_common.h"

static_assert(sizeof(PerFrameData) == 256, "PerFrameData must match its std140 layout (vec4/mat4 members only)");
static_assert(sizeof(ObjectData) == 144, "ObjectData must match game/'s geometry_ObjectData_t stride");
static_assert(sizeof(Material) == 64, "Material must match game/'s geometry_Material_t stride");

// Descriptor set 0 plumbing shared by scene passes, plus the sampled-input
// set used by fullscreen passes (copy-to-swapchain now, post passes later).
//
// Strategy: one descriptor set per frame in flight per layout, rewritten
// every frame right after that slot's fence wait. Buffer growth and
// resize-recreated image views are picked up automatically — no
// invalidation tracking.
struct FrameData
{
	// Layout A: per-frame UBO + ObjectData SSBO (scene passes)
	VkDescriptorSetLayout per_frame_layout = VK_NULL_HANDLE;

	// Layout B: single sampled image (fullscreen passes)
	VkDescriptorSetLayout sampled_input_layout = VK_NULL_HANDLE;

	VkDescriptorPool pool = VK_NULL_HANDLE;

	VkDescriptorSet per_frame_sets[MAX_FRAMES_IN_FLIGHT] = {};
	VkDescriptorSet copy_input_sets[MAX_FRAMES_IN_FLIGHT] = {};

	VkSampler linear_sampler = VK_NULL_HANDLE;

	GpuBuffer<PerFrameData> per_frame_ubos[MAX_FRAMES_IN_FLIGHT];
};

static FrameData frame_data;

void frame_data_init(VulkanContext* ctx)
{
	// Linear clamp sampler, created first: layout A embeds it as an immutable
	// sampler (binding 5); the copy pass shares it
	{
		VkSamplerCreateInfo sampler_create_info = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};
		VK_CHECK(vkCreateSampler(ctx->device, &sampler_create_info, nullptr, &frame_data.linear_sampler));
	}

	// Layout A (scene passes):
	//   0 = PerFrameData UBO          (VS|FS)
	//   1 = ObjectData SSBO           (VS)
	//   2 = Material SSBO             (FS)
	//   3 = skin matrix arena SSBO    (VS)
	//   4 = bindless texture array    (FS, PARTIALLY_BOUND)
	//   5 = immutable linear sampler  (FS)
	{
		VkDescriptorSetLayoutBinding bindings[] = {
			{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			},
			{
				.binding = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			},
			{
				.binding = 2,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			},
			{
				.binding = 3,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			},
			{
				.binding = 4,
				.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
				.descriptorCount = MAX_BINDLESS_IMAGES,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
			},
			{
				.binding = 5,
				.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				.pImmutableSamplers = &frame_data.linear_sampler,
			},
		};

		// Binding 4 is PARTIALLY_BOUND: only elements [0, image_count) are
		// ever written, and the shader-side image_index >= 0 guard keeps
		// unwritten elements from being accessed
		VkDescriptorBindingFlags binding_flags[] = {
			0, 0, 0, 0,
			VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
			0,
		};
		VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
			.bindingCount = sizeof(binding_flags) / sizeof(binding_flags[0]),
			.pBindingFlags = binding_flags,
		};

		VkDescriptorSetLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext = &binding_flags_create_info,
			.bindingCount = sizeof(bindings) / sizeof(bindings[0]),
			.pBindings = bindings,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &frame_data.per_frame_layout));
	}

	// Layout B: binding 0 = combined image sampler
	{
		VkDescriptorSetLayoutBinding binding = {
			.binding = 0,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		};

		VkDescriptorSetLayoutCreateInfo layout_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = 1,
			.pBindings = &binding,
		};
		VK_CHECK(vkCreateDescriptorSetLayout(ctx->device, &layout_create_info, nullptr, &frame_data.sampled_input_layout));
	}

	// Pool (sets live forever and are re-updated; headroom for future passes)
	{
		VkDescriptorPoolSize pool_sizes[] = {
			{ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 4 * MAX_FRAMES_IN_FLIGHT },
			{ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 8 * MAX_FRAMES_IN_FLIGHT },
			{ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 8 * MAX_FRAMES_IN_FLIGHT },
			{ .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = MAX_BINDLESS_IMAGES * MAX_FRAMES_IN_FLIGHT },
			{ .type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT },
		};

		VkDescriptorPoolCreateInfo pool_create_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = 16,
			.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]),
			.pPoolSizes = pool_sizes,
		};
		VK_CHECK(vkCreateDescriptorPool(ctx->device, &pool_create_info, nullptr, &frame_data.pool));
	}

	// Allocate the per-frame sets
	{
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			VkDescriptorSetAllocateInfo allocate_info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
				.descriptorPool = frame_data.pool,
				.descriptorSetCount = 1,
				.pSetLayouts = &frame_data.per_frame_layout,
			};
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &frame_data.per_frame_sets[frame_idx]));

			allocate_info.pSetLayouts = &frame_data.sampled_input_layout;
			VK_CHECK(vkAllocateDescriptorSets(ctx->device, &allocate_info, &frame_data.copy_input_sets[frame_idx]));
		}
	}

	// Per-frame UBOs
	for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
	{
		frame_data.per_frame_ubos[frame_idx] = GpuBuffer((GpuBufferDesc<PerFrameData>){
			.data = nullptr,
			.size = sizeof(PerFrameData),
			.usage = {
				.uniform_buffer = true,
				.stream_update = true,
			},
			.label = "FrameData::per_frame_ubo",
		});
	}
}

// Call once per frame, after vulkan_context_begin_frame's fence wait for
// this slot. Uploads the UBO and rewrites this frame's descriptor sets —
// buffer growth, image registration, and resize-recreated views are picked
// up automatically because the bindings are refreshed every frame.
void frame_data_update(
	VulkanContext* ctx,
	const PerFrameData& in_per_frame_data,
	VkBuffer in_object_data_buffer,
	VkBuffer in_material_buffer,
	VkBuffer in_skin_matrix_buffer,
	const GpuImage* in_images,
	i32 in_image_count
)
{
	const u32 frame_index = ctx->frame_index;

	frame_data.per_frame_ubos[frame_index].update_gpu_buffer(&in_per_frame_data, sizeof(PerFrameData));

	VkDescriptorBufferInfo ubo_info = {
		.buffer = frame_data.per_frame_ubos[frame_index].get_gpu_buffer(),
		.offset = 0,
		.range = sizeof(PerFrameData),
	};

	VkDescriptorBufferInfo object_data_info = {
		.buffer = in_object_data_buffer,
		.offset = 0,
		.range = VK_WHOLE_SIZE,
	};

	VkDescriptorBufferInfo material_info = {
		.buffer = in_material_buffer,
		.offset = 0,
		.range = VK_WHOLE_SIZE,
	};

	VkDescriptorBufferInfo skin_matrix_info = {
		.buffer = in_skin_matrix_buffer,
		.offset = 0,
		.range = VK_WHOLE_SIZE,
	};

	VkWriteDescriptorSet writes[5] = {
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = frame_data.per_frame_sets[frame_index],
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &ubo_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = frame_data.per_frame_sets[frame_index],
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &object_data_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = frame_data.per_frame_sets[frame_index],
			.dstBinding = 2,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &material_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = frame_data.per_frame_sets[frame_index],
			.dstBinding = 3,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &skin_matrix_info,
		},
	};
	u32 write_count = 4;

	// Bindless texture array: write only the registered prefix
	// (PARTIALLY_BOUND covers the rest; shader guards image_index >= 0)
	static VkDescriptorImageInfo image_infos[MAX_BINDLESS_IMAGES];
	if (in_image_count > 0)
	{
		assert(in_image_count <= MAX_BINDLESS_IMAGES);
		for (i32 image_idx = 0; image_idx < in_image_count; ++image_idx)
		{
			image_infos[image_idx] = (VkDescriptorImageInfo) {
				.imageView = in_images[image_idx].view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
		}

		writes[write_count++] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = frame_data.per_frame_sets[frame_index],
			.dstBinding = 4,
			.dstArrayElement = 0,
			.descriptorCount = (u32) in_image_count,
			.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.pImageInfo = image_infos,
		};
	}

	vkUpdateDescriptorSets(ctx->device, write_count, writes, 0, nullptr);
}

// Points this frame's sampled-input set at an image view (used by the
// copy-to-swapchain pass; same fence-wait safety contract as above)
void frame_data_write_copy_input(VulkanContext* ctx, VkImageView in_view)
{
	VkDescriptorImageInfo image_info = {
		.sampler = frame_data.linear_sampler,
		.imageView = in_view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};

	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = frame_data.copy_input_sets[ctx->frame_index],
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image_info,
	};

	vkUpdateDescriptorSets(ctx->device, 1, &write, 0, nullptr);
}

void frame_data_shutdown(VulkanContext* ctx)
{
	for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
	{
		frame_data.per_frame_ubos[frame_idx].destroy_gpu_buffer();
	}

	vkDestroySampler(ctx->device, frame_data.linear_sampler, nullptr);
	vkDestroyDescriptorPool(ctx->device, frame_data.pool, nullptr);
	vkDestroyDescriptorSetLayout(ctx->device, frame_data.sampled_input_layout, nullptr);
	vkDestroyDescriptorSetLayout(ctx->device, frame_data.per_frame_layout, nullptr);
}
