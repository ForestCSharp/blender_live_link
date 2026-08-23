#pragma once

#include "core/types.h"
#include "core/dynamic_array.h"
#include "render/gpu_buffer.h"
#include "render/shader_module.h"
#include "render/vulkan_context.h"

#include <cassert>

struct FullscreenPipelineDesc
{
	const char* vertex_shader_path = nullptr;
	const char* fragment_shader_path = nullptr;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	const VkFormat* color_formats = nullptr;
	u32 color_format_count = 0;
	VkFormat depth_format = VK_FORMAT_UNDEFINED;
	bool depth_test_enable = false;
	bool depth_write_enable = false;
	VkCompareOp depth_compare_op = VK_COMPARE_OP_ALWAYS;
	bool additive_blending = false;
	u32 additive_blend_mask = 0;
};

inline VkPipeline vulkan_create_fullscreen_pipeline(
	VulkanContext* ctx,
	const FullscreenPipelineDesc& in_desc)
{
	assert(in_desc.vertex_shader_path);
	assert(in_desc.fragment_shader_path);
	assert(in_desc.pipeline_layout != VK_NULL_HANDLE);
	assert(in_desc.color_formats || in_desc.color_format_count == 0);
	assert(in_desc.color_format_count > 0);

	VkShaderModule vertex_module =
		create_shader_module_from_file(ctx->device, in_desc.vertex_shader_path);
	VkShaderModule fragment_module =
		create_shader_module_from_file(ctx->device, in_desc.fragment_shader_path);
	VkPipelineShaderStageCreateInfo shader_stages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertex_module,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragment_module,
			.pName = "main",
		},
	};

	VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};
	VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dynamic_states,
	};
	VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo viewport = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rasterization = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisampling = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = in_desc.depth_test_enable,
		.depthWriteEnable = in_desc.depth_write_enable,
		.depthCompareOp = in_desc.depth_compare_op,
	};
	DynamicArray<VkPipelineColorBlendAttachmentState> blend_attachments;
	blend_attachments.resize(
		in_desc.color_format_count,
		(VkPipelineColorBlendAttachmentState) {
			.blendEnable = in_desc.additive_blending ? VK_TRUE : VK_FALSE,
			.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
			.dstColorBlendFactor = in_desc.additive_blending
				? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO,
			.colorBlendOp = VK_BLEND_OP_ADD,
			.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
			.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
			.alphaBlendOp = VK_BLEND_OP_ADD,
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
							| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		}
	);
	for (u32 attachment_idx = 0; attachment_idx < in_desc.color_format_count; ++attachment_idx)
	{
		const bool additive = in_desc.additive_blending
			|| (in_desc.additive_blend_mask & (1u << attachment_idx)) != 0;
		blend_attachments[attachment_idx].blendEnable = additive ? VK_TRUE : VK_FALSE;
		blend_attachments[attachment_idx].dstColorBlendFactor =
			additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
	}
	VkPipelineColorBlendStateCreateInfo color_blending = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = in_desc.color_format_count,
		.pAttachments = blend_attachments.data(),
	};
	VkPipelineRenderingCreateInfo rendering = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		.colorAttachmentCount = in_desc.color_format_count,
		.pColorAttachmentFormats = in_desc.color_formats,
		.depthAttachmentFormat = in_desc.depth_format,
	};
	VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.pNext = &rendering,
		.stageCount = 2,
		.pStages = shader_stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport,
		.pRasterizationState = &rasterization,
		.pMultisampleState = &multisampling,
		.pDepthStencilState = &depth_stencil,
		.pColorBlendState = &color_blending,
		.pDynamicState = &dynamic_state,
		.layout = in_desc.pipeline_layout,
		.renderPass = VK_NULL_HANDLE,
	};

	VkPipeline pipeline = VK_NULL_HANDLE;
	VK_CHECK(vulkan_create_graphics_pipelines(ctx, 1, &pipeline_info, &pipeline));
	vkDestroyShaderModule(ctx->device, vertex_module, nullptr);
	vkDestroyShaderModule(ctx->device, fragment_module, nullptr);
	return pipeline;
}

struct FullscreenPipeline
{
	VkPipeline pipeline = VK_NULL_HANDLE;

	void init(VulkanContext* ctx, const FullscreenPipelineDesc& in_desc)
	{
		assert(pipeline == VK_NULL_HANDLE);
		pipeline = vulkan_create_fullscreen_pipeline(ctx, in_desc);
	}

	void bind(VulkanContext* ctx) const
	{
		bind(vulkan_current_command_buffer(ctx));
	}

	void bind(VkCommandBuffer in_command_buffer) const
	{
		assert(pipeline != VK_NULL_HANDLE);
		assert(in_command_buffer != VK_NULL_HANDLE);
		vkCmdBindPipeline(in_command_buffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	}

	void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		pipeline = VK_NULL_HANDLE;
	}
};

struct DescriptorBindingSpec
{
	u32 binding = 0;
	VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
	u32 count = 1;
	VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT;
};

enum class EDescriptorSetAllocation
{
	PersistentPerFrame,
	Transient,
};

struct FullscreenEffectDesc
{
	const char* vertex_shader_path = nullptr;
	const char* fragment_shader_path = nullptr;
	const VkFormat* color_formats = nullptr;
	u32 color_format_count = 0;
	const DescriptorBindingSpec* bindings = nullptr;
	u32 binding_count = 0;
	u32 push_constant_size = 0;
	VkShaderStageFlags push_constant_stages = VK_SHADER_STAGE_FRAGMENT_BIT;
	bool additive_blending = false;
	u32 additive_blend_mask = 0;
};

// Fixed-capacity writer for the small descriptor sets used by graphics and
// compute effects. Binding types are checked against the C++ declaration in
// debug builds, keeping shader-facing declarations explicit without reflection.
struct DescriptorWriter
{
	static constexpr u32 MAX_WRITES = 32;

	VulkanContext* ctx = nullptr;
	VkDescriptorSet set = VK_NULL_HANDLE;
	const DescriptorBindingSpec* specs = nullptr;
	u32 spec_count = 0;
	bool allow_cache = true;
	VkDescriptorImageInfo image_infos[MAX_WRITES] = {};
	VkDescriptorBufferInfo buffer_infos[MAX_WRITES] = {};
	VkWriteDescriptorSet writes[MAX_WRITES] = {};
	u32 image_count = 0;
	u32 buffer_count = 0;
	u32 write_count = 0;

	const DescriptorBindingSpec& find(u32 in_binding, VkDescriptorType in_type) const
	{
		for (u32 index = 0; index < spec_count; ++index)
		{
			if (specs[index].binding == in_binding)
			{
				assert(specs[index].type == in_type);
				return specs[index];
			}
		}
		assert(false && "descriptor binding was not declared");
		return specs[0];
	}

	DescriptorWriter& sampled(u32 in_binding, VkSampler in_sampler, VkImageView in_view)
	{
		find(in_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		assert(image_count < MAX_WRITES && write_count < MAX_WRITES);
		image_infos[image_count] = descriptor_sampled(in_sampler, in_view);
		writes[write_count++] = descriptor_write_image(
			set, in_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			&image_infos[image_count++]);
		return *this;
	}

	DescriptorWriter& storage_image(
		u32 in_binding,
		VkImageView in_view,
		VkImageLayout in_layout = VK_IMAGE_LAYOUT_GENERAL)
	{
		find(in_binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		assert(image_count < MAX_WRITES && write_count < MAX_WRITES);
		image_infos[image_count] = {
			.imageView = in_view,
			.imageLayout = in_layout,
		};
		writes[write_count++] = descriptor_write_image(
			set, in_binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			&image_infos[image_count++]);
		return *this;
	}

	DescriptorWriter& buffer(
		u32 in_binding,
		VkDescriptorType in_type,
		VkBuffer in_buffer,
		VkDeviceSize in_range = VK_WHOLE_SIZE)
	{
		find(in_binding, in_type);
		assert(buffer_count < MAX_WRITES && write_count < MAX_WRITES);
		buffer_infos[buffer_count] = descriptor_buffer(in_buffer, in_range);
		writes[write_count++] = descriptor_write_buffer(
			set, in_binding, in_type, &buffer_infos[buffer_count++]);
		return *this;
	}

	void commit()
	{
		vulkan_update_descriptor_sets(
			ctx, write_count, writes, 0, nullptr, allow_cache);
	}
};

struct DescriptorSetSchema
{
	DynamicArray<DescriptorBindingSpec> binding_specs;
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	PerFrameDescriptorSets persistent_sets;
	EDescriptorSetAllocation allocation = EDescriptorSetAllocation::Transient;

	void init(
		VulkanContext* ctx,
		const DescriptorBindingSpec* in_bindings,
		u32 in_binding_count,
		EDescriptorSetAllocation in_allocation)
	{
		assert(layout == VK_NULL_HANDLE);
		assert(in_bindings || in_binding_count == 0);
		allocation = in_allocation;
		binding_specs.resize(in_binding_count);
		DynamicArray<VkDescriptorSetLayoutBinding> vk_bindings;
		vk_bindings.resize(in_binding_count);
		for (u32 index = 0; index < in_binding_count; ++index)
		{
			binding_specs[index] = in_bindings[index];
			vk_bindings[index] = {
				.binding = in_bindings[index].binding,
				.descriptorType = in_bindings[index].type,
				.descriptorCount = in_bindings[index].count,
				.stageFlags = in_bindings[index].stages,
			};
		}

		VkDescriptorSetLayoutCreateInfo set_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = in_binding_count,
			.pBindings = vk_bindings.data(),
		};
		VK_CHECK(vkCreateDescriptorSetLayout(
			ctx->device, &set_info, nullptr, &layout));
		if (allocation == EDescriptorSetAllocation::PersistentPerFrame)
		{
			persistent_sets.init_persistent(ctx, layout);
		}
	}

	VkDescriptorSet allocate(VulkanContext* ctx)
	{
		assert(layout != VK_NULL_HANDLE);
		return allocation == EDescriptorSetAllocation::PersistentPerFrame
			? persistent_sets.current(ctx)
			: vulkan_allocate_transient_descriptor_set(ctx, layout);
	}

	DescriptorWriter writer(VulkanContext* ctx)
	{
		return writer(ctx, allocate(ctx));
	}

	DescriptorWriter writer(VulkanContext* ctx, VkDescriptorSet in_set)
	{
		return writer(ctx, in_set,
			allocation == EDescriptorSetAllocation::PersistentPerFrame);
	}

	DescriptorWriter writer(
		VulkanContext* ctx,
		VkDescriptorSet in_set,
		bool in_allow_cache)
	{
		assert(in_set != VK_NULL_HANDLE);
		return {
			.ctx = ctx,
			.set = in_set,
			.specs = binding_specs.data(),
			.spec_count = (u32)binding_specs.length(),
			.allow_cache = in_allow_cache,
		};
	}

	VkDescriptorSet current(VulkanContext* ctx)
	{
		assert(allocation == EDescriptorSetAllocation::PersistentPerFrame);
		return persistent_sets.current(ctx);
	}

	void shutdown(VulkanContext* ctx)
	{
		vkDestroyDescriptorSetLayout(ctx->device, layout, nullptr);
		layout = VK_NULL_HANDLE;
		binding_specs.reset();
	}
};

struct EffectPipelineLayout
{
	VkPipelineLayout layout = VK_NULL_HANDLE;
	u32 push_constant_size = 0;
	VkShaderStageFlags push_constant_stages = 0;

	void init(
		VulkanContext* ctx,
		const VkDescriptorSetLayout* in_set_layouts,
		u32 in_set_layout_count,
		u32 in_push_constant_size,
		VkShaderStageFlags in_push_constant_stages)
	{
		assert(layout == VK_NULL_HANDLE);
		assert(in_set_layouts || in_set_layout_count == 0);
		push_constant_size = in_push_constant_size;
		push_constant_stages = in_push_constant_stages;
		VkPushConstantRange push_range = {
			.stageFlags = push_constant_stages,
			.offset = 0,
			.size = push_constant_size,
		};
		VkPipelineLayoutCreateInfo layout_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = in_set_layout_count,
			.pSetLayouts = in_set_layouts,
			.pushConstantRangeCount = push_constant_size > 0 ? 1u : 0u,
			.pPushConstantRanges = push_constant_size > 0 ? &push_range : nullptr,
		};
		VK_CHECK(vkCreatePipelineLayout(
			ctx->device, &layout_info, nullptr, &layout));
	}

	void push(VulkanContext* ctx, const void* in_push_constants) const
	{
		push(ctx, in_push_constants, push_constant_size);
	}

	void push(
		VulkanContext* ctx,
		const void* in_push_constants,
		u32 in_push_constant_size) const
	{
		if (in_push_constant_size == 0) return;
		assert(in_push_constants);
		assert(in_push_constant_size <= push_constant_size);
		vkCmdPushConstants(vulkan_current_command_buffer(ctx), layout,
			push_constant_stages, 0, in_push_constant_size, in_push_constants);
	}

	template<typename PushT>
	void push(VulkanContext* ctx, const PushT& in_push_constants) const
	{
		static_assert(sizeof(PushT) > 0);
		assert(push_constant_size == sizeof(PushT));
		push(ctx, static_cast<const void*>(&in_push_constants));
	}

	void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipelineLayout(ctx->device, layout, nullptr);
		layout = VK_NULL_HANDLE;
		push_constant_size = 0;
		push_constant_stages = 0;
	}
};

struct FullscreenEffect
{
	DescriptorSetSchema descriptors;
	EffectPipelineLayout pipeline_layout;
	FullscreenPipeline pipeline;
	bool has_descriptors = false;

	void init(VulkanContext* ctx, const FullscreenEffectDesc& in_desc)
	{
		assert(pipeline.pipeline == VK_NULL_HANDLE);
		has_descriptors = in_desc.binding_count > 0;
		if (has_descriptors)
		{
			descriptors.init(ctx, in_desc.bindings, in_desc.binding_count,
				EDescriptorSetAllocation::PersistentPerFrame);
		}
		const VkDescriptorSetLayout* set_layouts =
			has_descriptors ? &descriptors.layout : nullptr;
		pipeline_layout.init(ctx, set_layouts, has_descriptors ? 1u : 0u,
			in_desc.push_constant_size, in_desc.push_constant_stages);
		pipeline.init(ctx, {
			.vertex_shader_path = in_desc.vertex_shader_path,
			.fragment_shader_path = in_desc.fragment_shader_path,
			.pipeline_layout = pipeline_layout.layout,
			.color_formats = in_desc.color_formats,
			.color_format_count = in_desc.color_format_count,
			.additive_blending = in_desc.additive_blending,
			.additive_blend_mask = in_desc.additive_blend_mask,
		});
	}

	DescriptorWriter writer(VulkanContext* ctx)
	{
		assert(has_descriptors);
		return descriptors.writer(ctx);
	}

	void draw(VulkanContext* ctx, const void* in_push_constants = nullptr)
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		pipeline.bind(ctx);
		if (has_descriptors)
		{
			VkDescriptorSet set = descriptors.current(ctx);
			vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
				pipeline_layout.layout, 0, 1, &set, 0, nullptr);
		}
		pipeline_layout.push(ctx, in_push_constants);
		vulkan_cmd_draw(ctx, 3, 1, 0, 0);
	}

	void shutdown(VulkanContext* ctx)
	{
		pipeline.shutdown(ctx);
		pipeline_layout.shutdown(ctx);
		if (has_descriptors) descriptors.shutdown(ctx);
		has_descriptors = false;
	}
};

struct ComputeEffectDesc
{
	const char* shader_path = nullptr;
	const DescriptorBindingSpec* bindings = nullptr;
	u32 binding_count = 0;
	u32 push_constant_size = 0;
};

struct ComputeEffect
{
	DescriptorSetSchema descriptors;
	EffectPipelineLayout pipeline_layout;
	VkPipeline pipeline = VK_NULL_HANDLE;

	void init(VulkanContext* ctx, const ComputeEffectDesc& in_desc)
	{
		assert(pipeline == VK_NULL_HANDLE);
		assert(in_desc.shader_path);
		descriptors.init(ctx, in_desc.bindings, in_desc.binding_count,
			EDescriptorSetAllocation::Transient);
		pipeline_layout.init(ctx, &descriptors.layout, 1,
			in_desc.push_constant_size, VK_SHADER_STAGE_COMPUTE_BIT);

		VkShaderModule module =
			create_shader_module_from_file(ctx->device, in_desc.shader_path);
		VkComputePipelineCreateInfo pipeline_info = {
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_COMPUTE_BIT,
				.module = module,
				.pName = "main",
			},
			.layout = pipeline_layout.layout,
		};
		VK_CHECK(vulkan_create_compute_pipelines(
			ctx, 1, &pipeline_info, &pipeline));
		vkDestroyShaderModule(ctx->device, module, nullptr);
	}

	DescriptorWriter writer(VulkanContext* ctx)
	{
		return descriptors.writer(ctx);
	}

	void bind(VulkanContext* ctx, VkDescriptorSet in_set) const
	{
		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			pipeline_layout.layout, 0, 1, &in_set, 0, nullptr);
	}

	void dispatch(VulkanContext* ctx, u32 in_x, u32 in_y, u32 in_z,
		const void* in_push_constants = nullptr) const
	{
		pipeline_layout.push(ctx, in_push_constants);
		vulkan_cmd_dispatch(ctx, in_x, in_y, in_z);
	}

	void bind_and_dispatch(
		VulkanContext* ctx,
		VkDescriptorSet in_set,
		u32 in_x,
		u32 in_y,
		u32 in_z,
		const void* in_push_constants = nullptr) const
	{
		bind(ctx, in_set);
		dispatch(ctx, in_x, in_y, in_z, in_push_constants);
	}

	void shutdown(VulkanContext* ctx)
	{
		vkDestroyPipeline(ctx->device, pipeline, nullptr);
		pipeline = VK_NULL_HANDLE;
		pipeline_layout.shutdown(ctx);
		descriptors.shutdown(ctx);
	}
};

template<typename PushT>
struct TypedComputeEffect
{
	static constexpr u32 PUSH_CONSTANT_SIZE = sizeof(PushT);
	ComputeEffect effect;

	void init(VulkanContext* ctx, ComputeEffectDesc in_desc)
	{
		assert(in_desc.push_constant_size == 0
			|| in_desc.push_constant_size == PUSH_CONSTANT_SIZE);
		in_desc.push_constant_size = PUSH_CONSTANT_SIZE;
		effect.init(ctx, in_desc);
	}

	DescriptorWriter writer(VulkanContext* ctx)
	{
		return effect.writer(ctx);
	}

	void bind(VulkanContext* ctx, VkDescriptorSet in_set) const
	{
		effect.bind(ctx, in_set);
	}

	void dispatch(VulkanContext* ctx, const PushT& in_push_constants,
		u32 in_x, u32 in_y, u32 in_z) const
	{
		assert(effect.pipeline_layout.push_constant_size == PUSH_CONSTANT_SIZE);
		effect.dispatch(ctx, in_x, in_y, in_z, &in_push_constants);
	}

	void bind_and_dispatch(VulkanContext* ctx, VkDescriptorSet in_set,
		const PushT& in_push_constants, u32 in_x, u32 in_y, u32 in_z) const
	{
		effect.bind(ctx, in_set);
		dispatch(ctx, in_push_constants, in_x, in_y, in_z);
	}

	void shutdown(VulkanContext* ctx)
	{
		effect.shutdown(ctx);
	}
};

template<typename UniformT>
struct TypedFullscreenEffect
{
	FullscreenEffect effect;
	PerFrameUniform<UniformT> uniform;
	u32 uniform_binding = 0;

	void init(
		VulkanContext* ctx,
		const FullscreenEffectDesc& in_desc,
		u32 in_uniform_binding,
		const char* in_uniform_label)
	{
		uniform_binding = in_uniform_binding;
		effect.init(ctx, in_desc);
		uniform.init(in_uniform_label);
	}

	DescriptorWriter writer(VulkanContext* ctx, const UniformT& in_uniform)
	{
		DescriptorWriter result = effect.writer(ctx);
		result.buffer(uniform_binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			uniform.update(ctx, in_uniform), sizeof(UniformT));
		return result;
	}

	void draw(VulkanContext* ctx, const void* in_push_constants = nullptr)
	{
		effect.draw(ctx, in_push_constants);
	}

	void shutdown(VulkanContext* ctx)
	{
		uniform.shutdown();
		effect.shutdown(ctx);
	}
};
