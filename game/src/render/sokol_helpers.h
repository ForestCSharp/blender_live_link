#pragma once

#include "sokol/sokol_gfx.h"

#include <cstddef>

#if defined(SOKOL_D3D11)
	#include <d3d11_1.h>
#endif

struct GpuDebugScope
{
public:
	explicit GpuDebugScope(const char* in_name)
	{
		active = push(in_name);
	}

	~GpuDebugScope()
	{
		if (active)
		{
			pop();
		}
	}

	GpuDebugScope(const GpuDebugScope&) = delete;
	GpuDebugScope& operator=(const GpuDebugScope&) = delete;

private:
	bool active = false;

	static bool push(const char* in_name)
	{
		if (!sg_isvalid() || !in_name)
		{
			return false;
		}

		#if defined(SOKOL_METAL)
			if (sg_mtl_render_command_encoder() || sg_mtl_compute_command_encoder())
			{
				sg_push_debug_group(in_name);
				return true;
			}
			return false;
		#elif defined(SOKOL_D3D11)
			ID3DUserDefinedAnnotation* annotation = query_d3d11_annotation();
			if (!annotation)
			{
				return false;
			}

			wchar_t wide_name[256];
			to_wide_string(in_name, wide_name, sizeof(wide_name) / sizeof(wide_name[0]));
			const INT result = annotation->BeginEvent(wide_name);
			annotation->Release();
			return result >= 0;
		#else
			(void)in_name;
			return false;
		#endif
	}

	static void pop()
	{
		#if defined(SOKOL_METAL)
			sg_pop_debug_group();
		#elif defined(SOKOL_D3D11)
			ID3DUserDefinedAnnotation* annotation = query_d3d11_annotation();
			if (annotation)
			{
				annotation->EndEvent();
				annotation->Release();
			}
		#endif
	}

	#if defined(SOKOL_D3D11)
	static ID3DUserDefinedAnnotation* query_d3d11_annotation()
	{
		const void* context_ptr = sg_d3d11_device_context();
		if (!context_ptr)
		{
			return nullptr;
		}

		ID3D11DeviceContext* context = static_cast<ID3D11DeviceContext*>(const_cast<void*>(context_ptr));
		ID3DUserDefinedAnnotation* annotation = nullptr;
		if (FAILED(context->QueryInterface(__uuidof(ID3DUserDefinedAnnotation), reinterpret_cast<void**>(&annotation))))
		{
			return nullptr;
		}
		return annotation;
	}

	static void to_wide_string(const char* src, wchar_t* dst, const size_t dst_count)
	{
		if (!dst || dst_count == 0)
		{
			return;
		}

		size_t i = 0;
		if (src)
		{
			for (; (i + 1) < dst_count && src[i]; ++i)
			{
				dst[i] = static_cast<unsigned char>(src[i]);
			}
		}
		dst[i] = L'\0';
	}
	#endif
};

#define GPU_DEBUG_SCOPE_JOIN_INNER(a, b) a##b
#define GPU_DEBUG_SCOPE_JOIN(a, b) GPU_DEBUG_SCOPE_JOIN_INNER(a, b)
#define GPU_DEBUG_SCOPE(name) GpuDebugScope GPU_DEBUG_SCOPE_JOIN(gpu_debug_scope_, __LINE__)(name)
