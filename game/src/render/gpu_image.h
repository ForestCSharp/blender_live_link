#pragma once

#include "core/types.h"
#include "sokol/sokol_gfx.h"

#include <cassert>
#include <optional>
#include <string>

struct GpuImageDesc
{
	sg_image_type type;
	sg_image_usage usage;
	i32 width;
	i32 height;
	sg_pixel_format pixel_format;	
	const u8* data = nullptr;
	const char* label;
};

struct GpuImage
{
public:
	GpuImage() = default;

	GpuImage(const GpuImageDesc& in_desc)
	: desc(in_desc)
	{		
		i32 bytes_per_pixel = sg_query_pixelformat(desc.pixel_format).bytes_per_pixel;

		sg_image_desc sokol_image_desc = {
			.type = desc.type,
			.usage = desc.usage,
			.width = desc.width,
			.height = desc.height,
			.pixel_format = desc.pixel_format,
			.sample_count = 1,
			.data = {0},
			.label = desc.label,
		};

		const u8* initial_data = desc.data;
		if (initial_data)
		{
			const size_t initial_data_size = initial_data ? static_cast<size_t>(desc.width * desc.height * bytes_per_pixel) : 0;
			sokol_image_desc.data.subimage[0][0].ptr = initial_data;		
			sokol_image_desc.data.subimage[0][0].size = initial_data_size;
		}

		gpu_image = sg_make_image(&sokol_image_desc);
	}

	sg_image get_gpu_image() const { return gpu_image; }

protected:
	GpuImageDesc desc;
	sg_image gpu_image;
};

