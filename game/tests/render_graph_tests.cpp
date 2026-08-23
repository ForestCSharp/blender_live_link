#include <cassert>

#define VK_NO_PROTOTYPES
#define VOLK_IMPLEMENTATION
#include "volk/volk.h"
#include "vma/vk_mem_alloc.h"
#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"
#include "handmade_math/HandmadeMath.h"

#include "render/frame_render_graph.h"
#include "render/fullscreen_pipeline.h"

struct TestComputePushConstants
{
	u32 x;
	u32 y;
};

static_assert(TypedComputeEffect<TestComputePushConstants>::PUSH_CONSTANT_SIZE == 8);

int main()
{
	const RenderTargetExtent fixed = render_target_extent_fixed(512, 256);
	assert(fixed.type == ERenderTargetExtent::Fixed);
	assert(fixed.width == 512 && fixed.height == 256);

	const RenderTargetExtent scaled = render_target_extent_scaled(0.5f);
	assert(scaled.type == ERenderTargetExtent::Render);
	assert(scaled.width_scale == 0.5f && scaled.height_scale == 0.5f);

	const RenderPassDesc color = render_target_color_desc(
		"Color", VK_FORMAT_R16G16B16A16_SFLOAT, render_target_extent_output());
	assert(color.num_outputs == 1);
	assert(color.outputs[0].format == VK_FORMAT_R16G16B16A16_SFLOAT);
	assert(color.outputs[0].load_op == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
	assert(color.outputs[0].store_op == VK_ATTACHMENT_STORE_OP_STORE);
	assert(color.extent.type == ERenderTargetExtent::Output);

	const RenderPassDesc mrt = render_target_mrt_desc(
		"MRT", VK_FORMAT_R8_UNORM, 3, {}, VK_ATTACHMENT_LOAD_OP_CLEAR);
	assert(mrt.num_outputs == 3);
	for (i32 output_index = 0; output_index < mrt.num_outputs; ++output_index)
	{
		assert(mrt.outputs[output_index].format == VK_FORMAT_R8_UNORM);
		assert(mrt.outputs[output_index].load_op == VK_ATTACHMENT_LOAD_OP_CLEAR);
	}

	GpuImage first;
	GpuImage second;
	first.extent = { 512, 256 };
	first.mip_levels = 4;
	first.array_layers = 3;
	first.aspects = VK_IMAGE_ASPECT_COLOR_BIT;
	first.mip_views.resize(4);
	first.layer_views.resize(3);
	const FrameGraphImage whole = frame_graph_image(first);
	assert(whole.extent.width == 512 && whole.extent.height == 256);
	assert(whole.range.levelCount == 4 && whole.range.layerCount == 3);
	const FrameGraphImage mip = frame_graph_mip(first, 2);
	assert(mip.extent.width == 128 && mip.extent.height == 64);
	assert(mip.range.baseMipLevel == 2 && mip.range.levelCount == 1);
	assert(mip.range.baseArrayLayer == 0 && mip.range.layerCount == 3);
	const FrameGraphImage layer = frame_graph_layer(first, 1);
	assert(layer.range.baseMipLevel == 0 && layer.range.levelCount == 4);
	assert(layer.range.baseArrayLayer == 1 && layer.range.layerCount == 1);

	const FrameGraphImage selected = frame_graph_select(
		true, whole, { .image = &second });
	assert(selected.image == &first);
	assert(frame_graph_select(false, { .image = &first }, { .image = &second }).image
		== &second);
	const FrameGraphBuffer buffer = frame_graph_buffer(reinterpret_cast<VkBuffer>(4));
	assert(buffer.buffer == reinterpret_cast<VkBuffer>(4));
	FrameRenderGraph graph(nullptr);
	graph.sampled(mip);
	graph.storage_write(buffer);
	assert(graph.pending().images.length() == 1);
	assert(graph.pending().images[0].range.baseMipLevel == 2);
	assert(graph.pending().images[0].range.levelCount == 1);
	assert(graph.pending().images[0].layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	assert(graph.pending().buffers.length() == 1);
	assert(graph.pending().buffers[0].offset == 0);
	assert(graph.pending().buffers[0].size == VK_WHOLE_SIZE);
	assert(graph.pending().buffers[0].access == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	assert(frame_graph_attachment_access(VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		== VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	assert(frame_graph_attachment_discards(VK_ATTACHMENT_LOAD_OP_CLEAR));
	assert(frame_graph_attachment_access(VK_ATTACHMENT_LOAD_OP_LOAD)
		== (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT
			| VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
	assert(!frame_graph_attachment_discards(VK_ATTACHMENT_LOAD_OP_LOAD));

	const DescriptorBindingSpec bindings[] = {
		{ .binding = 0, .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			.stages = VK_SHADER_STAGE_COMPUTE_BIT },
		{ .binding = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.stages = VK_SHADER_STAGE_COMPUTE_BIT },
	};
	DescriptorWriter writer = {
		.set = reinterpret_cast<VkDescriptorSet>(1),
		.specs = bindings,
		.spec_count = 2,
		.allow_cache = false,
	};
	assert(writer.find(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE).stages
		== VK_SHADER_STAGE_COMPUTE_BIT);
	writer.storage_image(0, reinterpret_cast<VkImageView>(2));
	writer.buffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		reinterpret_cast<VkBuffer>(3), 64);
	assert(writer.write_count == 2);
	assert(writer.writes[0].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	assert(writer.image_infos[0].imageLayout == VK_IMAGE_LAYOUT_GENERAL);
	assert(writer.writes[1].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	assert(writer.buffer_infos[0].range == 64);
	assert(!writer.allow_cache);
	return 0;
}
