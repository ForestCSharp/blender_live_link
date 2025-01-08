#pragma once

#include "types.h"
#include "sokol/sokol_gfx.h"

#include <cassert>
#include <optional>
#include <string>

// Helper primarily used to pass between live link and main threads. 
// We want to wait until our data is back on the main thread to create GPU resources.
template<typename T> 
struct GpuBufferDesc
{
	/* Data to initialize the GpuBuffer with */
	T* data;
	
	/* Size of data */
	u64 data_size;

	/* max size of this buffer. Only meaningful if is_dynamic is true */
	u64 max_size;
	
	/* type of the GpuBuffer */
	sg_buffer_type type;

	/* can the GpuBuffer be updated after creation? */
	bool is_dynamic = false;

	/* Debug Label */
	const char* label;
};

template<typename T>
struct GpuBuffer
{
public:
	GpuBuffer() = default;

	GpuBuffer(GpuBufferDesc<T> in_desc)
	: data(in_desc.data)
	, data_size(in_desc.data_size)
	, max_size(in_desc.max_size)
	, gpu_buffer_type(in_desc.type)
	, is_dynamic(in_desc.is_dynamic)
	{
		//Static Buffers max_size and data_size should be identical
		if (!is_dynamic && max_size != data_size)
		{
			max_size = data_size;
		}

		set_label(in_desc.label);
	}

	bool is_gpu_buffer_valid()
	{
		return gpu_buffer.has_value();
	}

	sg_buffer get_gpu_buffer()
	{
		if (!gpu_buffer.has_value())
		{
			sg_usage usage = is_dynamic ? SG_USAGE_DYNAMIC : SG_USAGE_IMMUTABLE;

			sg_buffer_desc buffer_desc = {
				.type = gpu_buffer_type,
				.usage = usage,
				.data = {	
					/* 
					 * Static Buffers Set up their data in sg_make_buffer
					 * Dynamic Buffers only need to init to their max size
					 */
					.ptr = is_dynamic ? nullptr : data,
					.size = is_dynamic ? max_size : data_size,
				},
				.label = label ? label->c_str() : "",
			};

			gpu_buffer = sg_make_buffer(buffer_desc);

			// Dynamic Buffer Update can happen now
			if (is_dynamic && data != nullptr && data_size > 0)
			{
				// If Dynamic Buffer has data, send via sg_update_buffer now
				const sg_range update_range = {
					.ptr = data,
					.size = data_size,
				};

				sg_update_buffer(
					*gpu_buffer, 
					update_range				
				);
			}
	
			if (!keep_data)
			{
				free(data);	
			}
		}

		return *gpu_buffer;
	}

	void update_gpu_buffer(const sg_range& in_range)
	{
		assert(is_dynamic);
		sg_update_buffer(get_gpu_buffer(), &in_range);
	}

	void destroy_gpu_buffer()
	{	
		if (gpu_buffer.has_value())
		{
			sg_destroy_buffer(*gpu_buffer);
			gpu_buffer.reset();
		}
	}

	void set_label(const char* in_label)
	{
		label = std::string(in_label);
	}

	void set_keep_data(bool in_keep_data)
	{
		keep_data = in_keep_data;
	}

protected:
	// Underlying Buffer Data
	T* data;

	// Data Size
	u64 data_size;

	// Max Possible Size
	u64 max_size;

	// Gpu Buffer Type
	sg_buffer_type gpu_buffer_type;

	// is this Gpu Buffer dynamic
	bool is_dynamic = false;

	// Buffer used for gpu operations
	std::optional<sg_buffer> gpu_buffer;

	// If true, data is kept alive after initialing gpu_buffer
	bool keep_data = false;

	// Optional Label
	std::optional<std::string> label;
};
