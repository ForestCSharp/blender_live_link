#include <cassert>

#define VK_NO_PROTOTYPES
#define VOLK_IMPLEMENTATION
#include "volk/volk.h"
#include "vma/vk_mem_alloc.h"
#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"
#include "handmade_math/HandmadeMath.h"

#include "render/frame_render_graph.h"

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
	const FrameGraphImage selected = frame_graph_select(
		true, { .image = &first }, { .image = &second });
	assert(selected.image == &first);
	assert(frame_graph_select(false, { .image = &first }, { .image = &second }).image
		== &second);
	return 0;
}
