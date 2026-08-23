#pragma once

#include <functional>

#include "render/render_pass.h"

// Lightweight ordered frame graph. It does not schedule or own resources:
// declarations are applied immediately before the next node in recording order.
struct FrameGraphImage
{
	GpuImage* image = nullptr;
	VkImageView image_view = VK_NULL_HANDLE;
	VkExtent2D extent = {};
	VkImageSubresourceRange range = {};

	VkImageView view() const
	{
		assert(image);
		return image_view != VK_NULL_HANDLE ? image_view : image->view;
	}

	explicit operator bool() const { return image != nullptr; }
};

struct FrameGraphBuffer
{
	VkBuffer buffer = VK_NULL_HANDLE;

	explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};

inline FrameGraphImage frame_graph_image(GpuImage& in_image)
{
	return {
		.image = &in_image,
		.image_view = in_image.view,
		.extent = in_image.extent,
		.range = {
			.aspectMask = in_image.aspects,
			.baseMipLevel = 0,
			.levelCount = in_image.mip_levels,
			.baseArrayLayer = 0,
			.layerCount = in_image.array_layers,
		},
	};
}

inline FrameGraphImage frame_graph_mip(GpuImage& in_image, u32 in_mip)
{
	assert(in_mip < in_image.mip_levels);
	return {
		.image = &in_image,
		.image_view = in_image.mip_levels > 1
			? in_image.mip_views[in_mip] : in_image.view,
		.extent = {
			MAX(1u, in_image.extent.width >> in_mip),
			MAX(1u, in_image.extent.height >> in_mip),
		},
		.range = {
			.aspectMask = in_image.aspects,
			.baseMipLevel = in_mip,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = in_image.array_layers,
		},
	};
}

inline FrameGraphImage frame_graph_layer(GpuImage& in_image, u32 in_layer)
{
	assert(in_layer < in_image.array_layers);
	return {
		.image = &in_image,
		.image_view = in_image.array_layers > 1
			? in_image.layer_views[in_layer] : in_image.view,
		.extent = in_image.extent,
		.range = {
			.aspectMask = in_image.aspects,
			.baseMipLevel = 0,
			.levelCount = in_image.mip_levels,
			.baseArrayLayer = in_layer,
			.layerCount = 1,
		},
	};
}

inline FrameGraphImage frame_graph_color(
	RenderPass& in_target,
	i32 in_output_index = 0,
	i32 in_image_index = 0)
{
	return frame_graph_image(
		in_target.get_color_output(in_output_index, in_image_index));
}

inline FrameGraphBuffer frame_graph_buffer(VkBuffer in_buffer)
{
	return { .buffer = in_buffer };
}

inline FrameGraphImage frame_graph_select(
	bool in_condition,
	FrameGraphImage in_enabled,
	FrameGraphImage in_fallback)
{
	return in_condition ? in_enabled : in_fallback;
}

struct FrameGraphColorAttachment
{
	FrameGraphImage image;
	VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	VkAttachmentStoreOp store_op = VK_ATTACHMENT_STORE_OP_STORE;
	VkClearValue clear_value = {};
};

inline VkAccessFlags2 frame_graph_attachment_access(VkAttachmentLoadOp in_load_op)
{
	return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
		| (in_load_op == VK_ATTACHMENT_LOAD_OP_LOAD
			? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : 0);
}

inline bool frame_graph_attachment_discards(VkAttachmentLoadOp in_load_op)
{
	return in_load_op != VK_ATTACHMENT_LOAD_OP_LOAD;
}

struct FrameRenderGraph
{
	explicit FrameRenderGraph(VulkanContext* in_ctx) : ctx(in_ctx)
	{
		pending_usage.images.reserve(16);
		pending_usage.buffers.reserve(16);
	}

	FrameGraphImage sampled(
		FrameGraphImage in_resource,
		VkPipelineStageFlags2 in_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT)
	{
		return image_usage(in_resource, in_stage,
			VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	FrameGraphImage storage_read(
		FrameGraphImage in_resource,
		VkPipelineStageFlags2 in_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
	{
		return image_usage(in_resource, in_stage,
			VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_IMAGE_LAYOUT_GENERAL);
	}

	FrameGraphImage storage_write(
		FrameGraphImage in_resource,
		VkPipelineStageFlags2 in_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		bool in_discard = false)
	{
		return image_usage(in_resource, in_stage,
			VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, in_discard);
	}

	FrameGraphImage storage_read_write(
		FrameGraphImage in_resource,
		VkPipelineStageFlags2 in_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
	{
		return image_usage(in_resource, in_stage,
			VK_ACCESS_2_SHADER_STORAGE_READ_BIT
				| VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_IMAGE_LAYOUT_GENERAL);
	}

	FrameGraphImage transfer_source(FrameGraphImage in_resource)
	{
		return image_usage(in_resource, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	}

	FrameGraphImage transfer_destination(
		FrameGraphImage in_resource, bool in_discard = false)
	{
		return image_usage(in_resource, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			in_discard);
	}

	FrameGraphBuffer storage_read(
		FrameGraphBuffer in_resource,
		VkPipelineStageFlags2 in_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
	{
		return buffer_usage(in_resource, in_stage, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	}

	FrameGraphBuffer uniform(
		FrameGraphBuffer in_resource,
		VkPipelineStageFlags2 in_stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT)
	{
		return buffer_usage(in_resource, in_stage, VK_ACCESS_2_UNIFORM_READ_BIT);
	}

	FrameGraphBuffer storage_write(
		FrameGraphBuffer in_resource,
		VkPipelineStageFlags2 in_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
	{
		return buffer_usage(in_resource, in_stage, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	}

	FrameGraphBuffer storage_read_write(
		FrameGraphBuffer in_resource,
		VkPipelineStageFlags2 in_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT)
	{
		return buffer_usage(in_resource, in_stage,
			VK_ACCESS_2_SHADER_STORAGE_READ_BIT
				| VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	}

	FrameGraphBuffer vertex(FrameGraphBuffer in_resource)
	{
		return buffer_usage(in_resource, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
			VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
	}

	FrameGraphBuffer index(FrameGraphBuffer in_resource)
	{
		return buffer_usage(in_resource, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
			VK_ACCESS_2_INDEX_READ_BIT);
	}

	FrameGraphBuffer transfer_source(FrameGraphBuffer in_resource)
	{
		return buffer_usage(in_resource, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_READ_BIT);
	}

	FrameGraphBuffer transfer_destination(FrameGraphBuffer in_resource)
	{
		return buffer_usage(in_resource, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
			VK_ACCESS_2_TRANSFER_WRITE_BIT);
	}

	void apply()
	{
		vulkan_apply_pass_resource_usage(ctx, pending_usage);
		pending_usage.images.clear();
		pending_usage.buffers.clear();
	}

	void make_sampled(FrameGraphImage in_resource)
	{
		sampled(in_resource);
		apply();
	}

	void make_sampled(RenderPass& in_target)
	{
		for (GpuImage& output : in_target.color_outputs)
			sampled(frame_graph_image(output));
		apply();
	}

	void execute(
		RenderPass& in_target,
		const std::function<void(i32)>& in_callback,
		i32 in_pass_count = -1)
	{
		apply();
		in_target.execute(ctx, in_callback, in_pass_count);
	}

	void render(
		const FrameGraphColorAttachment* in_attachments,
		u32 in_attachment_count,
		const std::function<void()>& in_callback)
	{
		assert(in_attachments && in_attachment_count > 0);
		const VkExtent2D extent = in_attachments[0].image.extent;
		DynamicArray<VkRenderingAttachmentInfo> rendering_attachments;
		rendering_attachments.resize(in_attachment_count);
		for (u32 index = 0; index < in_attachment_count; ++index)
		{
			const FrameGraphColorAttachment& attachment = in_attachments[index];
			assert(attachment.image && attachment.image.extent.width == extent.width
				&& attachment.image.extent.height == extent.height);
			image_usage(attachment.image,
				VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				frame_graph_attachment_access(attachment.load_op),
				VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				frame_graph_attachment_discards(attachment.load_op));
			rendering_attachments[index] = {
				.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
				.imageView = attachment.image.view(),
				.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				.loadOp = attachment.load_op,
				.storeOp = attachment.store_op,
				.clearValue = attachment.clear_value,
			};
		}
		apply();

		VkCommandBuffer command_buffer = vulkan_current_command_buffer(ctx);
		VkRenderingInfo rendering_info = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.renderArea = { .offset = {0, 0}, .extent = extent },
			.layerCount = 1,
			.colorAttachmentCount = in_attachment_count,
			.pColorAttachments = rendering_attachments.data(),
		};
		vkCmdBeginRendering(command_buffer, &rendering_info);
		VkViewport viewport = {
			.x = 0.0f,
			.y = (f32)extent.height,
			.width = (f32)extent.width,
			.height = -(f32)extent.height,
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};
		VkRect2D scissor = { .offset = {0, 0}, .extent = extent };
		vkCmdSetViewport(command_buffer, 0, 1, &viewport);
		vkCmdSetScissor(command_buffer, 0, 1, &scissor);
		in_callback();
		vkCmdEndRendering(command_buffer);
	}

	void compute(const std::function<void()>& in_callback)
	{
		apply();
		in_callback();
	}

	void transfer(const std::function<void()>& in_callback)
	{
		apply();
		in_callback();
	}

	const PassResourceUsage& pending() const { return pending_usage; }

private:
	FrameGraphImage image_usage(
		FrameGraphImage in_resource,
		VkPipelineStageFlags2 in_stage,
		VkAccessFlags2 in_access,
		VkImageLayout in_layout,
		bool in_discard = false)
	{
		assert(in_resource.image);
		pending_usage.images.add({
			.image = in_resource.image,
			.range = in_resource.range,
			.stage = in_stage,
			.access = in_access,
			.layout = in_layout,
			.discard = in_discard,
		});
		return in_resource;
	}

	FrameGraphBuffer buffer_usage(
		FrameGraphBuffer in_resource,
		VkPipelineStageFlags2 in_stage,
		VkAccessFlags2 in_access)
	{
		assert(in_resource.buffer != VK_NULL_HANDLE);
		pending_usage.buffers.add({
			.buffer = in_resource.buffer,
			.offset = 0,
			.size = VK_WHOLE_SIZE,
			.stage = in_stage,
			.access = in_access,
		});
		return in_resource;
	}

	VulkanContext* ctx = nullptr;
	PassResourceUsage pending_usage;
};
