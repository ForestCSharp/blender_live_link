#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"
#include "render/gpu_buffer.h"

// GLSL-shared struct definitions (PerFrameData, ObjectData)
#include "shader_common.h"

static_assert(sizeof(PerFrameData) == 320, "PerFrameData must match its std140 layout (vec4/mat4 members only)");
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

	VkDescriptorSet per_frame_sets[MAX_FRAMES_IN_FLIGHT] = {};
	VkDescriptorSet copy_input_sets[MAX_FRAMES_IN_FLIGHT] = {};

	VkSampler linear_sampler = VK_NULL_HANDLE;

	GpuBuffer<PerFrameData> per_frame_ubos[MAX_FRAMES_IN_FLIGHT];

	struct BindingCache
	{
		VkBuffer ubo = VK_NULL_HANDLE;
		VkBuffer object_data = VK_NULL_HANDLE;
		VkBuffer material = VK_NULL_HANDLE;
		VkBuffer skin_matrices = VK_NULL_HANDLE;
		i32 image_count = -1;
		VkImageView image_views[MAX_BINDLESS_IMAGES] = {};
		VkImageView copy_input = VK_NULL_HANDLE;
	} binding_cache[MAX_FRAMES_IN_FLIGHT];
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
		vulkan_set_object_name(ctx, VK_OBJECT_TYPE_SAMPLER, (u64)frame_data.linear_sampler, "Shared Linear Sampler");
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

	// Allocate the per-frame sets
	{
		for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
		{
			frame_data.per_frame_sets[frame_idx] =
				vulkan_allocate_persistent_descriptor_set(ctx, frame_data.per_frame_layout);
			char frame_set_name[64];
			snprintf(frame_set_name, sizeof(frame_set_name), "Frame %u Scene Descriptor Set", frame_idx);
			vulkan_set_object_name(ctx, VK_OBJECT_TYPE_DESCRIPTOR_SET, (u64)frame_data.per_frame_sets[frame_idx], frame_set_name);

			frame_data.copy_input_sets[frame_idx] =
				vulkan_allocate_persistent_descriptor_set(ctx, frame_data.sampled_input_layout);
			char copy_set_name[64];
			snprintf(copy_set_name, sizeof(copy_set_name), "Frame %u Copy Input Set", frame_idx);
			vulkan_set_object_name(ctx, VK_OBJECT_TYPE_DESCRIPTOR_SET, (u64)frame_data.copy_input_sets[frame_idx], copy_set_name);
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

	FrameData::BindingCache& cache = frame_data.binding_cache[frame_index];
	VkWriteDescriptorSet writes[5] = {};
	u32 write_count = 0;
	auto append_buffer_write = [&](u32 in_binding, VkDescriptorType in_type, VkDescriptorBufferInfo* in_info)
	{
		writes[write_count++] = (VkWriteDescriptorSet) {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = frame_data.per_frame_sets[frame_index],
			.dstBinding = in_binding,
			.descriptorCount = 1,
			.descriptorType = in_type,
			.pBufferInfo = in_info,
		};
	};
	if (cache.ubo != ubo_info.buffer)
	{
		append_buffer_write(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ubo_info);
		cache.ubo = ubo_info.buffer;
	}
	if (cache.object_data != object_data_info.buffer)
	{
		append_buffer_write(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &object_data_info);
		cache.object_data = object_data_info.buffer;
	}
	if (cache.material != material_info.buffer)
	{
		append_buffer_write(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &material_info);
		cache.material = material_info.buffer;
	}
	if (cache.skin_matrices != skin_matrix_info.buffer)
	{
		append_buffer_write(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &skin_matrix_info);
		cache.skin_matrices = skin_matrix_info.buffer;
	}

	// Bindless texture array: write only the registered prefix
	// (PARTIALLY_BOUND covers the rest; shader guards image_index >= 0)
	static VkDescriptorImageInfo image_infos[MAX_BINDLESS_IMAGES];
	bool images_dirty = cache.image_count != in_image_count;
	for (i32 image_idx = 0; !images_dirty && image_idx < in_image_count; ++image_idx)
	{
		images_dirty = cache.image_views[image_idx] != in_images[image_idx].view;
	}
	if (images_dirty && in_image_count > 0)
	{
		assert(in_image_count <= MAX_BINDLESS_IMAGES);
		for (i32 image_idx = 0; image_idx < in_image_count; ++image_idx)
		{
			image_infos[image_idx] = (VkDescriptorImageInfo) {
				.imageView = in_images[image_idx].view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			};
			cache.image_views[image_idx] = in_images[image_idx].view;
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
	if (images_dirty) cache.image_count = in_image_count;

	if (write_count > 0) vulkan_update_descriptor_sets(ctx, write_count, writes);
}

// Points this frame's sampled-input set at an image view (used by the
// copy-to-swapchain pass; same fence-wait safety contract as above)
void frame_data_write_copy_input(VulkanContext* ctx, VkImageView in_view)
{
	FrameData::BindingCache& cache = frame_data.binding_cache[ctx->frame_index];
	if (cache.copy_input == in_view) return;
	cache.copy_input = in_view;

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

	vulkan_update_descriptor_sets(ctx, 1, &write);
}

void frame_data_shutdown(VulkanContext* ctx)
{
	for (u32 frame_idx = 0; frame_idx < MAX_FRAMES_IN_FLIGHT; ++frame_idx)
	{
		frame_data.per_frame_ubos[frame_idx].destroy_gpu_buffer();
	}

	vkDestroySampler(ctx->device, frame_data.linear_sampler, nullptr);
	vkDestroyDescriptorSetLayout(ctx->device, frame_data.sampled_input_layout, nullptr);
	vkDestroyDescriptorSetLayout(ctx->device, frame_data.per_frame_layout, nullptr);
}
