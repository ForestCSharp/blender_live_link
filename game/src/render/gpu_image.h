#pragma once

#include "core/types.h"
#include "sokol/sokol_gfx.h"

#include <cassert>
#include <optional>
#include <string>

using std::optional;

struct GpuImageDesc
{
	sg_image_type type;
	sg_image_usage usage;
	i32 width;
	i32 height;
	i32 num_slices = 1;
	sg_pixel_format pixel_format;	
	const u8* data = nullptr;
	const char* label;
};

struct GpuImage
{
public:
	GpuImage() = default;

	GpuImage(const GpuImageDesc& in_desc)
	{		
		recreate(in_desc);
	}

	~GpuImage()
	{
		//FCS TODO: Get move-semantics working
		//cleanup();
	}

	void recreate(const GpuImageDesc& in_desc)
	{
		cleanup();

		desc = in_desc;

		i32 bytes_per_pixel = sg_query_pixelformat(desc.pixel_format).bytes_per_pixel;

		sg_image_desc sokol_image_desc = {
			.type = desc.type,
			.usage = desc.usage,
			.width = desc.width,
			.height = desc.height,
			.num_slices = desc.num_slices,
			.pixel_format = desc.pixel_format,
			.sample_count = 1,
			.data = {0},
			.label = desc.label,
		};

		const u8* initial_data = desc.data;
		if (initial_data)
		{
			const size_t initial_data_size	= desc.width * desc.height * desc.num_slices * bytes_per_pixel;

			sokol_image_desc.data.mip_levels[0].ptr = initial_data;		
			sokol_image_desc.data.mip_levels[0].size = initial_data_size;
		}

		gpu_image = sg_make_image(&sokol_image_desc);
	}

	void cleanup()
	{
		for (i32 i = 0; i < NUM_CUBE_FACES; ++i)
		{
			if (texture_views[i].has_value())
			{
				sg_destroy_view(texture_views[i].value());
			}
		}

		for (i32 i = 0; i < NUM_CUBE_FACES; ++i)
		{
			if (attachment_views[i].has_value())
			{
				sg_destroy_view(attachment_views[i].value());
			}
		}
		
		if (gpu_image.has_value() != SG_INVALID_ID)
		{
			sg_destroy_image(gpu_image.value());	
		}
	}

	sg_image get_gpu_image() const { return gpu_image.value(); }

	sg_view get_texture_view(const i32 slice_idx)
	{
		assert(gpu_image.has_value() && gpu_image.value().id != SG_INVALID_ID);

		assert(slice_idx < NUM_CUBE_FACES);

		if (!texture_views[slice_idx].has_value())
		{
			texture_views[slice_idx] = sg_make_view((sg_view_desc) {
				.texture = {
					.image = get_gpu_image(),	
					.mip_levels = { .base = 0, .count = 0, },
					.slices = { .base = slice_idx, .count = 1, },
				},
			});
		}

		return texture_views[slice_idx].value();
	}

	sg_view get_attachment_view(const i32 slice_idx)
	{
		assert(gpu_image.has_value() && gpu_image.value().id != SG_INVALID_ID);

		assert(slice_idx < NUM_CUBE_FACES);

		if (!attachment_views[slice_idx].has_value())
		{
			if (	desc.pixel_format == SG_PIXELFORMAT_DEPTH 
				||	desc.pixel_format == SG_PIXELFORMAT_DEPTH_STENCIL)
			{
				attachment_views[slice_idx] = sg_make_view((sg_view_desc) {
					.depth_stencil_attachment = {
						.image = get_gpu_image(),	
						.mip_level = 0,
						.slice = slice_idx,
					},
				});
			}
			else
			{
				attachment_views[slice_idx] = sg_make_view((sg_view_desc) {
					.color_attachment = {
						.image = get_gpu_image(),	
						.mip_level = 0,
						.slice = slice_idx,
					},
				});
			}
		}

		return attachment_views[slice_idx].value();
	}

protected:
	GpuImageDesc desc = {};
	optional<sg_image> gpu_image;

	// Lazily-allocated texture view
	optional<sg_view> texture_views[NUM_CUBE_FACES];

	// Lazily-allocated attachment views, per slice idx
	optional<sg_view> attachment_views[NUM_CUBE_FACES];
};

