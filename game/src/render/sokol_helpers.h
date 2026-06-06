#pragma once

#include "core/types.h"
#include "sokol/sokol_gfx.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>

#if defined(SOKOL_D3D11)
	#include <d3d11_1.h>
#endif

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI

#include "core/timings.h"

#if defined(SOKOL_D3D11)
static constexpr i32 GPU_FRAME_TIMING_QUERY_SLOT_COUNT = 8;
static constexpr i32 GPU_FRAME_TIMING_MAX_SCOPE_EVENTS = 128;

struct D3D11GpuScopeTiming
{
	ID3D11Query* start_query = nullptr;
	ID3D11Query* end_query = nullptr;
	char name[CPU_TIMINGS_MAX_NAME_LENGTH] = "(unnamed)";
	i32 depth = 0;
	i32 parent_index = -1;
	bool used = false;
	bool ended = false;
};

struct D3D11GpuFrameTimingSlot
{
	ID3D11Query* disjoint_query = nullptr;
	ID3D11Query* start_query = nullptr;
	ID3D11Query* end_query = nullptr;
	D3D11GpuScopeTiming scopes[GPU_FRAME_TIMING_MAX_SCOPE_EVENTS];
	i32 scope_count = 0;
	i32 active_scope_stack[GPU_FRAME_TIMING_MAX_SCOPE_EVENTS];
	i32 active_scope_stack_count = 0;
	i64 frame_index = -1;
	bool available = false;
	bool pending = false;
	bool active = false;
};

struct D3D11GpuFrameTimingState
{
	D3D11GpuFrameTimingSlot slots[GPU_FRAME_TIMING_QUERY_SLOT_COUNT];
	i32 next_slot_index = 0;
	i32 active_slot_index = -1;
	bool initialized = false;
};

static D3D11GpuFrameTimingState& d3d11_gpu_frame_timing_state()
{
	static D3D11GpuFrameTimingState state;
	return state;
}

static ID3D11Device* gpu_frame_timings_d3d11_device()
{
	return static_cast<ID3D11Device*>(const_cast<void*>(sg_d3d11_device()));
}

static ID3D11DeviceContext* gpu_frame_timings_d3d11_context()
{
	return static_cast<ID3D11DeviceContext*>(const_cast<void*>(sg_d3d11_device_context()));
}

static void gpu_frame_timings_release_d3d11_scope(D3D11GpuScopeTiming& scope)
{
	if (scope.start_query)
	{
		scope.start_query->Release();
	}
	if (scope.end_query)
	{
		scope.end_query->Release();
	}
	scope = {};
}

static void gpu_frame_timings_release_d3d11_slot(D3D11GpuFrameTimingSlot& slot)
{
	if (slot.disjoint_query)
	{
		slot.disjoint_query->Release();
	}
	if (slot.start_query)
	{
		slot.start_query->Release();
	}
	if (slot.end_query)
	{
		slot.end_query->Release();
	}
	for (D3D11GpuScopeTiming& scope : slot.scopes)
	{
		gpu_frame_timings_release_d3d11_scope(scope);
	}
	slot = {};
}

static bool gpu_frame_timings_ensure_d3d11_scope_queries(D3D11GpuScopeTiming& scope)
{
	if (scope.start_query && scope.end_query)
	{
		return true;
	}

	ID3D11Device* device = gpu_frame_timings_d3d11_device();
	if (!device)
	{
		return false;
	}

	D3D11_QUERY_DESC timestamp_desc = {};
	timestamp_desc.Query = D3D11_QUERY_TIMESTAMP;
	if (
		FAILED(device->CreateQuery(&timestamp_desc, &scope.start_query)) ||
		FAILED(device->CreateQuery(&timestamp_desc, &scope.end_query))
	)
	{
		gpu_frame_timings_release_d3d11_scope(scope);
		return false;
	}

	return true;
}

static void gpu_frame_timings_poll_d3d11()
{
	D3D11GpuFrameTimingState& state = d3d11_gpu_frame_timing_state();
	if (!state.initialized)
	{
		return;
	}

	ID3D11DeviceContext* context = gpu_frame_timings_d3d11_context();
	if (!context)
	{
		return;
	}

	for (D3D11GpuFrameTimingSlot& slot : state.slots)
	{
		if (!slot.available || !slot.pending || slot.active)
		{
			continue;
		}

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint_data = {};
		HRESULT result = context->GetData(
			slot.disjoint_query,
			&disjoint_data,
			sizeof(disjoint_data),
			D3D11_ASYNC_GETDATA_DONOTFLUSH
		);
		if (FAILED(result))
		{
			slot.pending = false;
			slot.frame_index = -1;
			continue;
		}
		if (result != S_OK)
		{
			continue;
		}

		UINT64 start_ticks = 0;
		result = context->GetData(
			slot.start_query,
			&start_ticks,
			sizeof(start_ticks),
			D3D11_ASYNC_GETDATA_DONOTFLUSH
		);
		if (FAILED(result))
		{
			slot.pending = false;
			slot.frame_index = -1;
			continue;
		}
		if (result != S_OK)
		{
			continue;
		}

		UINT64 end_ticks = 0;
		result = context->GetData(
			slot.end_query,
			&end_ticks,
			sizeof(end_ticks),
			D3D11_ASYNC_GETDATA_DONOTFLUSH
		);
		if (FAILED(result))
		{
			slot.pending = false;
			slot.frame_index = -1;
			continue;
		}
		if (result != S_OK)
		{
			continue;
		}

		if (!disjoint_data.Disjoint && disjoint_data.Frequency > 0 && end_ticks >= start_ticks)
		{
			GpuTimingEvent events[GPU_FRAME_TIMING_MAX_SCOPE_EVENTS + 1] = {};
			i32 event_count = 0;

			GpuTimingEvent& root_event = events[event_count++];
			gpu_timing_event_set_name(root_event, "GPU Frame");
			root_event.depth = 0;
			root_event.parent_index = -1;
			root_event.start_offset_ms = 0.0;
			root_event.elapsed_ms = ((f64)(end_ticks - start_ticks) * 1000.0) / (f64)disjoint_data.Frequency;
			root_event.valid = true;

			bool all_scope_queries_ready = true;
			for (i32 scope_index = 0; scope_index < slot.scope_count; ++scope_index)
			{
				D3D11GpuScopeTiming& scope = slot.scopes[scope_index];
				if (!scope.used || !scope.ended)
				{
					continue;
				}

				UINT64 scope_start_ticks = 0;
				result = context->GetData(
					scope.start_query,
					&scope_start_ticks,
					sizeof(scope_start_ticks),
					D3D11_ASYNC_GETDATA_DONOTFLUSH
				);
				if (FAILED(result))
				{
					scope.used = false;
					continue;
				}
				if (result != S_OK)
				{
					all_scope_queries_ready = false;
					break;
				}

				UINT64 scope_end_ticks = 0;
				result = context->GetData(
					scope.end_query,
					&scope_end_ticks,
					sizeof(scope_end_ticks),
					D3D11_ASYNC_GETDATA_DONOTFLUSH
				);
				if (FAILED(result))
				{
					scope.used = false;
					continue;
				}
				if (result != S_OK)
				{
					all_scope_queries_ready = false;
					break;
				}

				if (scope_end_ticks < scope_start_ticks || event_count >= (i32)(sizeof(events) / sizeof(events[0])))
				{
					continue;
				}

				GpuTimingEvent& event = events[event_count++];
				gpu_timing_event_set_name(event, scope.name);
				event.depth = scope.depth;
				event.parent_index = scope.parent_index;
				event.start_offset_ms = scope_start_ticks >= start_ticks
					? ((f64)(scope_start_ticks - start_ticks) * 1000.0) / (f64)disjoint_data.Frequency
					: 0.0;
				event.elapsed_ms = ((f64)(scope_end_ticks - scope_start_ticks) * 1000.0) / (f64)disjoint_data.Frequency;
				event.valid = event.elapsed_ms > 0.0;
			}

			if (!all_scope_queries_ready)
			{
				continue;
			}

			gpu_timings_record_completed_frame_events(slot.frame_index, events, event_count);
		}

		slot.pending = false;
		slot.frame_index = -1;
		slot.scope_count = 0;
		slot.active_scope_stack_count = 0;
	}
}

static void gpu_frame_timings_init_d3d11()
{
	D3D11GpuFrameTimingState& state = d3d11_gpu_frame_timing_state();
	if (state.initialized)
	{
		return;
	}

	ID3D11Device* device = gpu_frame_timings_d3d11_device();
	if (!device)
	{
		gpu_timings_set_available(false, "D3D11 device unavailable");
		return;
	}

	i32 available_slot_count = 0;
	for (D3D11GpuFrameTimingSlot& slot : state.slots)
	{
		D3D11_QUERY_DESC disjoint_desc = {};
		disjoint_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		D3D11_QUERY_DESC timestamp_desc = {};
		timestamp_desc.Query = D3D11_QUERY_TIMESTAMP;

		if (
			FAILED(device->CreateQuery(&disjoint_desc, &slot.disjoint_query)) ||
			FAILED(device->CreateQuery(&timestamp_desc, &slot.start_query)) ||
			FAILED(device->CreateQuery(&timestamp_desc, &slot.end_query))
		)
		{
			gpu_frame_timings_release_d3d11_slot(slot);
			continue;
		}

		slot.available = true;
		++available_slot_count;
	}

	state.initialized = available_slot_count > 0;
	gpu_timings_set_available(state.initialized, "D3D11 timestamp query creation failed");
}

static void gpu_frame_timings_shutdown_d3d11()
{
	D3D11GpuFrameTimingState& state = d3d11_gpu_frame_timing_state();
	for (D3D11GpuFrameTimingSlot& slot : state.slots)
	{
		gpu_frame_timings_release_d3d11_slot(slot);
	}
	state = {};
}

static void gpu_frame_timings_begin_frame_d3d11(i64 in_frame_index)
{
	D3D11GpuFrameTimingState& state = d3d11_gpu_frame_timing_state();
	gpu_frame_timings_poll_d3d11();
	if (!state.initialized || state.active_slot_index >= 0 || in_frame_index < 0)
	{
		return;
	}

	ID3D11DeviceContext* context = gpu_frame_timings_d3d11_context();
	if (!context)
	{
		return;
	}

	i32 slot_index = -1;
	for (i32 attempt = 0; attempt < GPU_FRAME_TIMING_QUERY_SLOT_COUNT; ++attempt)
	{
		const i32 candidate_index = state.next_slot_index;
		state.next_slot_index = (state.next_slot_index + 1) % GPU_FRAME_TIMING_QUERY_SLOT_COUNT;

		D3D11GpuFrameTimingSlot& candidate = state.slots[candidate_index];
		if (candidate.available && !candidate.pending && !candidate.active)
		{
			slot_index = candidate_index;
			break;
		}
	}

	if (slot_index < 0)
	{
		return;
	}

	D3D11GpuFrameTimingSlot& slot = state.slots[slot_index];
	slot.frame_index = in_frame_index;
	slot.pending = true;
	slot.active = true;
	slot.scope_count = 0;
	slot.active_scope_stack_count = 0;
	state.active_slot_index = slot_index;

	context->Begin(slot.disjoint_query);
	context->End(slot.start_query);
}

static void gpu_frame_timings_end_frame_d3d11(i64 in_frame_index)
{
	(void)in_frame_index;

	D3D11GpuFrameTimingState& state = d3d11_gpu_frame_timing_state();
	if (!state.initialized || state.active_slot_index < 0)
	{
		return;
	}

	ID3D11DeviceContext* context = gpu_frame_timings_d3d11_context();
	if (!context)
	{
		D3D11GpuFrameTimingSlot& slot = state.slots[state.active_slot_index];
		slot.active = false;
		slot.pending = false;
		slot.frame_index = -1;
		state.active_slot_index = -1;
		return;
	}

	D3D11GpuFrameTimingSlot& slot = state.slots[state.active_slot_index];
	context->End(slot.end_query);
	context->End(slot.disjoint_query);
	slot.active = false;
	state.active_slot_index = -1;

	gpu_frame_timings_poll_d3d11();
}

static i32 gpu_frame_timings_begin_scope(const char* in_name)
{
	D3D11GpuFrameTimingState& state = d3d11_gpu_frame_timing_state();
	if (!state.initialized || state.active_slot_index < 0)
	{
		return -1;
	}

	ID3D11DeviceContext* context = gpu_frame_timings_d3d11_context();
	if (!context)
	{
		return -1;
	}

	D3D11GpuFrameTimingSlot& slot = state.slots[state.active_slot_index];
	if (!slot.active || slot.scope_count >= GPU_FRAME_TIMING_MAX_SCOPE_EVENTS)
	{
		return -1;
	}

	const i32 scope_index = slot.scope_count;
	D3D11GpuScopeTiming& scope = slot.scopes[scope_index];
	if (!gpu_frame_timings_ensure_d3d11_scope_queries(scope))
	{
		return -1;
	}

	snprintf(scope.name, sizeof(scope.name), "%s", in_name ? in_name : "(unnamed)");
	scope.depth = slot.active_scope_stack_count + 1;
	scope.parent_index = slot.active_scope_stack_count > 0
		? slot.active_scope_stack[slot.active_scope_stack_count - 1] + 1
		: 0;
	scope.used = true;
	scope.ended = false;

	slot.active_scope_stack[slot.active_scope_stack_count++] = scope_index;
	++slot.scope_count;

	context->End(scope.start_query);
	return scope_index;
}

static void gpu_frame_timings_end_scope(i32 in_scope_index)
{
	if (in_scope_index < 0)
	{
		return;
	}

	D3D11GpuFrameTimingState& state = d3d11_gpu_frame_timing_state();
	if (!state.initialized || state.active_slot_index < 0)
	{
		return;
	}

	D3D11GpuFrameTimingSlot& slot = state.slots[state.active_slot_index];
	if (!slot.active || in_scope_index >= slot.scope_count)
	{
		return;
	}

	ID3D11DeviceContext* context = gpu_frame_timings_d3d11_context();
	if (!context)
	{
		return;
	}

	D3D11GpuScopeTiming& scope = slot.scopes[in_scope_index];
	if (scope.used)
	{
		context->End(scope.end_query);
		scope.ended = true;
	}

	if (
		slot.active_scope_stack_count > 0 &&
		slot.active_scope_stack[slot.active_scope_stack_count - 1] == in_scope_index
	)
	{
		--slot.active_scope_stack_count;
	}
}
#endif

#if defined(SOKOL_METAL)
static void gpu_frame_timings_metal_scopes_completed(
	i64 in_frame_index,
	const sg_mtl_gpu_timing_event* in_events,
	i32 in_event_count,
	void* in_user_data)
{
	(void)in_user_data;
	if (in_frame_index < 0 || !in_events || in_event_count <= 0)
	{
		return;
	}

	GpuTimingEvent events[SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS + 1] = {};
	const i32 event_count = std::min(in_event_count, (i32)(sizeof(events) / sizeof(events[0])));
	for (i32 event_index = 0; event_index < event_count; ++event_index)
	{
		const sg_mtl_gpu_timing_event& source_event = in_events[event_index];
		GpuTimingEvent& event = events[event_index];
		gpu_timing_event_set_name(event, source_event.name);
		event.depth = source_event.depth;
		event.parent_index = source_event.parent_index;
		event.start_offset_ms = source_event.start_offset_ms;
		event.elapsed_ms = source_event.elapsed_ms;
		event.valid = source_event.valid;
	}

	gpu_timings_record_completed_frame_events(in_frame_index, events, event_count);
}

static i32 gpu_frame_timings_begin_scope(const char* in_name)
{
	return sg_mtl_begin_gpu_scope_timing(in_name);
}

static void gpu_frame_timings_end_scope(i32 in_scope_index)
{
	sg_mtl_end_gpu_scope_timing(in_scope_index);
}

static u64& gpu_frame_timings_metal_wait_start_ticks()
{
	static u64 wait_start_ticks = 0;
	return wait_start_ticks;
}

static void gpu_frame_timings_metal_wait(bool in_waiting, void* in_user_data)
{
	(void)in_user_data;
	u64& wait_start_ticks = gpu_frame_timings_metal_wait_start_ticks();
	if (in_waiting)
	{
		wait_start_ticks = stm_now();
		return;
	}

	if (wait_start_ticks != 0)
	{
		const u64 wait_end_ticks = stm_now();
		cpu_timings_record_event("Sokol GPU frame wait", wait_start_ticks, wait_end_ticks);
		wait_start_ticks = 0;
	}
}

static void gpu_frame_timings_metal_completed(i64 in_frame_index, f64 in_gpu_start_sec, f64 in_gpu_end_sec, void* in_user_data)
{
	(void)in_user_data;
	if (in_gpu_end_sec <= in_gpu_start_sec)
	{
		return;
	}

	gpu_timings_record_completed_frame(in_frame_index, (in_gpu_end_sec - in_gpu_start_sec) * 1000.0);
}
#endif

#if !defined(SOKOL_D3D11) && !defined(SOKOL_METAL)
static i32 gpu_frame_timings_begin_scope(const char*)
{
	return -1;
}

static void gpu_frame_timings_end_scope(i32)
{
}
#endif

static void gpu_frame_timings_init()
{
	#if defined(SOKOL_METAL)
		gpu_timings_set_available(true);
		sg_mtl_set_gpu_frame_timing_callback(gpu_frame_timings_metal_completed, nullptr);
		sg_mtl_set_gpu_frame_wait_callback(gpu_frame_timings_metal_wait, nullptr);
		if (sg_mtl_init_gpu_scope_timing())
		{
			sg_mtl_set_gpu_scope_timing_callback(gpu_frame_timings_metal_scopes_completed, nullptr);
		}
	#elif defined(SOKOL_D3D11)
		gpu_frame_timings_init_d3d11();
	#else
		gpu_timings_set_available(false, "GPU timings are not implemented for this backend");
	#endif
}

static void gpu_frame_timings_shutdown()
{
	#if defined(SOKOL_METAL)
		sg_mtl_set_gpu_frame_timing_callback(nullptr, nullptr);
		sg_mtl_set_gpu_frame_wait_callback(nullptr, nullptr);
		sg_mtl_set_gpu_scope_timing_callback(nullptr, nullptr);
		sg_mtl_shutdown_gpu_scope_timing();
		sg_mtl_set_gpu_frame_timing_frame_index(-1);
		gpu_timings_set_available(false, "GPU timings shut down");
	#elif defined(SOKOL_D3D11)
		gpu_frame_timings_shutdown_d3d11();
		gpu_timings_set_available(false, "GPU timings shut down");
	#endif
}

static void gpu_frame_timings_begin_frame(i64 in_frame_index)
{
	#if defined(SOKOL_METAL)
		sg_mtl_set_gpu_frame_timing_frame_index(in_frame_index);
	#elif defined(SOKOL_D3D11)
		gpu_frame_timings_begin_frame_d3d11(in_frame_index);
	#else
		(void)in_frame_index;
	#endif
}

static void gpu_frame_timings_end_frame(i64 in_frame_index)
{
	#if defined(SOKOL_D3D11)
		gpu_frame_timings_end_frame_d3d11(in_frame_index);
	#else
		(void)in_frame_index;
	#endif
}

#else

static void gpu_frame_timings_init() {}
static void gpu_frame_timings_shutdown() {}
static void gpu_frame_timings_begin_frame(i64) {}
static void gpu_frame_timings_end_frame(i64) {}
static i32 gpu_frame_timings_begin_scope(const char*) { return -1; }
static void gpu_frame_timings_end_scope(i32) {}

#endif

struct GpuDebugScope
{
public:
	explicit GpuDebugScope(const char* in_name)
	{
		active = push(in_name);
		timing_scope_index = gpu_frame_timings_begin_scope(in_name);
	}

	~GpuDebugScope()
	{
		if (timing_scope_index >= 0)
		{
			gpu_frame_timings_end_scope(timing_scope_index);
		}
		if (active)
		{
			pop();
		}
	}

	GpuDebugScope(const GpuDebugScope&) = delete;
	GpuDebugScope& operator=(const GpuDebugScope&) = delete;

private:
	bool active = false;
	i32 timing_scope_index = -1;

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
