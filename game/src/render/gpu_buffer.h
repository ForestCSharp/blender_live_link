#pragma once

#include "core/types.h"
#include "sokol/sokol_gfx.h"

#include <cassert>
#include <optional>
#include <string>

using std::optional;

// Helper primarily used to pass between live link and main threads. 
// We want to wait until our data is back on the main thread to create GPU resources.
template<typename T> 
struct GpuBufferDesc
{
	/* Data to initialize the GpuBuffer with */
	T* data;
	
	/* 
	 *	The size of the buffer to be created.
	 *	For static buffers this represents the size of T* data used to initialize this buffer
	 *	For dynamic buffers with no initial data: this represents the max possible size of the buffer
	*/
	u64 size;

	/* buffer usage info */
	sg_buffer_usage usage;
	
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
	, size(in_desc.size)
	, _length(in_desc.size / sizeof(T))
	, usage(in_desc.usage)
	{
		set_label(in_desc.label);
	}

	~GpuBuffer()
	{
		// TODO: move semantics
		// cleanup()
	}

	void cleanup()
	{
		if (storage_view.has_value())
		{
			sg_destroy_view(storage_view.value());
		}
		
		if (gpu_buffer.has_value())
		{
			sg_destroy_buffer(gpu_buffer.value());
		}
	}

	bool is_gpu_buffer_valid()
	{
		return gpu_buffer.has_value();
	}

	bool is_dynamic() const
	{
		return usage.dynamic_update || usage.stream_update;
	}

	sg_buffer get_gpu_buffer()
	{
		if (!gpu_buffer.has_value())
		{
			sg_buffer_desc buffer_desc = {
				.size = size,
				.usage = usage,
				.label = label ? label->c_str() : "",
			};

			// Non-Dynamic Buffers pass their initial data to sg_make_buffer
			if (!is_dynamic())
			{
				buffer_desc.data = {
					.ptr = data,
					.size = size,
				};
			}

			gpu_buffer = sg_make_buffer(buffer_desc);

			// Dynamic Buffer Update can happen now
			if (is_dynamic() && data != nullptr && size > 0)
			{
				// If Dynamic Buffer has data, send via sg_update_buffer now
				const sg_range update_range = {
					.ptr = data,
					.size = size,
				};

				sg_update_buffer(
					*gpu_buffer, 
					update_range				
				);
			}	
		}

		return *gpu_buffer;
	}

	void update_gpu_buffer(const sg_range& in_range)
	{
		assert(is_dynamic());
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

	u64 length() const { return _length; }

	sg_view get_storage_view()
	{	
		if (!storage_view.has_value())
		{
			printf("Making storage view for %s\n", label ? label->c_str() : "[NO LABEL]");

			storage_view = sg_make_view((sg_view_desc) {
				.storage_buffer = {
					.buffer = get_gpu_buffer(),
				}
			});

			static i32 storage_view_count = 0;
			++storage_view_count;
			printf("Storage View Count: %i\n", storage_view_count);
		}
		return storage_view.value();
	}

protected:
	/* Underlying Buffer Data */
	T* data;

	/* Data Size */ 
	u64 size;

	/* The element count of the buffer */
	u64 _length;

	/* buffer usage info */
	sg_buffer_usage usage;

	// Buffer used for gpu operations
	optional<sg_buffer> gpu_buffer;

	// Lazily-allocated storage view
	optional<sg_view> storage_view;

	// Optional Label
	optional<std::string> label;
};
