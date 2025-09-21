
#include "types.h"
#include "sokol/sokol_gfx.h"

#include <cassert>
#include <optional>
#include <string>

struct GpuImageDesc
{
	int width;
	int height;
	sg_pixel_format pixel_format;	
	const u8* data;
	const char* label;
};

struct GpuImage
{
public:
	GpuImage() = default;

	GpuImage(const GpuImageDesc& in_desc)
	: pixel_format(in_desc.pixel_format)	
	, width(in_desc.width)
	, height(in_desc.height)
	{		
		bytes_per_pixel = sg_query_pixelformat(pixel_format).bytes_per_pixel;

		sg_image_desc sokol_image_desc = {
			.width = width,
			.height = height,
			.pixel_format = pixel_format,
			.sample_count = 1,
			.data.subimage[0][0].ptr = in_desc.data,
			.data.subimage[0][0].size = static_cast<size_t>(width * height * bytes_per_pixel),
			.label = in_desc.label,
		};
		
		gpu_image = sg_make_image(sokol_image_desc);
	}

	sg_image get_gpu_image() const { return gpu_image; }

protected:
	sg_pixel_format pixel_format;

	int width;
	int height;
	int bytes_per_pixel;

	sg_image gpu_image;
};

//_sg_pixelformat_bytesize


//sg_image_desc ssao_noise_desc = {
//	.width = SSAO_TEXTURE_WIDTH,
//	.height = SSAO_TEXTURE_WIDTH,
//	.pixel_format = SG_PIXELFORMAT_RGBA32F,
//	.sample_count = SAMPLE_COUNT,
//	.data.subimage[0][0].ptr = &ssao_noise,
//	.data.subimage[0][0].size = SSAO_TEXTURE_SIZE * sizeof(HMM_Vec4),
//	.label = "ssao_noise_texture"
//};
//
//state.ssao_noise_texture = sg_make_image(ssao_noise_desc);
