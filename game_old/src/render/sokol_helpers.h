#pragma once

#include "core/types.h"
#include "render/gpu_profiler_resources.h"
#include "sokol/sokol_gfx.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(SOKOL_D3D11)
	#include <d3d11_1.h>
#endif

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI

#include "core/timings.h"

struct GpuProfilerPendingScopeMetadata
{
	char writes[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
};

static GpuProfilerPendingScopeMetadata& gpu_profiler_pending_scope_metadata()
{
	static GpuProfilerPendingScopeMetadata metadata;
	return metadata;
}

static void gpu_frame_timings_set_next_scope_writes(const char* in_writes)
{
	GpuProfilerPendingScopeMetadata& metadata = gpu_profiler_pending_scope_metadata();
	snprintf(metadata.writes, sizeof(metadata.writes), "%s", in_writes ? in_writes : "");
}

static void gpu_frame_timings_consume_next_scope_writes(char* out_writes, size_t in_writes_size)
{
	if (!out_writes || in_writes_size == 0)
	{
		return;
	}

	GpuProfilerPendingScopeMetadata& metadata = gpu_profiler_pending_scope_metadata();
	snprintf(out_writes, in_writes_size, "%s", metadata.writes);
	metadata.writes[0] = '\0';
}

static void gpu_frame_timings_append_bindings_reads_to_text(const sg_bindings* in_bindings, char* io_reads, size_t in_reads_size)
{
	if (!in_bindings || !io_reads || in_reads_size == 0)
	{
		return;
	}

	for (i32 view_index = 0; view_index < SG_MAX_VIEW_BINDSLOTS; ++view_index)
	{
		const sg_view view = in_bindings->views[view_index];
		if (view.id == SG_INVALID_ID)
		{
			continue;
		}

		const char* view_name = gpu_profiler_lookup_view_name(view);
		char fallback_name[64] = {};
		if (!view_name)
		{
			snprintf(fallback_name, sizeof(fallback_name), "view:%u", view.id);
			view_name = fallback_name;
		}
		gpu_profiler_append_dependency_name(io_reads, in_reads_size, view_name);
	}
}

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
	i32 command_order_index = -1;
	u64 cpu_encode_start_ticks = 0;
	u64 cpu_encode_end_ticks = 0;
	char reads[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
	char writes[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
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
	i64 submission_id = -1;
	i64 command_buffer_id = -1;
	u64 cpu_encode_start_ticks = 0;
	u64 cpu_encode_end_ticks = 0;
	u64 cpu_submit_ticks = 0;
	u64 cpu_completed_ticks = 0;
	bool available = false;
	bool pending = false;
	bool active = false;
};

struct D3D11GpuFrameTimingState
{
	D3D11GpuFrameTimingSlot slots[GPU_FRAME_TIMING_QUERY_SLOT_COUNT];
	i32 next_slot_index = 0;
	i32 active_slot_index = -1;
	i64 next_submission_id = 0;
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
			slot.cpu_completed_ticks = stm_now();
			GpuTimingEvent events[GPU_FRAME_TIMING_MAX_SCOPE_EVENTS + 1] = {};
			i32 event_count = 0;

			GpuTimingEvent& root_event = events[event_count++];
			gpu_timing_event_set_name(root_event, "GPU Frame");
			root_event.depth = 0;
			root_event.parent_index = -1;
			root_event.lane_index = 0;
			root_event.type = GpuTimingEventType::Frame;
			root_event.timestamp_source = GpuTimingTimestampSource::D3D11Timestamp;
			root_event.timestamp_confidence = GpuTimingTimestampConfidence::Authoritative;
			root_event.submission_id = slot.submission_id;
			root_event.command_buffer_id = slot.command_buffer_id;
			root_event.command_order_index = 0;
			root_event.start_offset_ms = 0.0;
			root_event.elapsed_ms = ((f64)(end_ticks - start_ticks) * 1000.0) / (f64)disjoint_data.Frequency;
			root_event.cpu_encode_start_ticks = slot.cpu_encode_start_ticks;
			root_event.cpu_encode_end_ticks = slot.cpu_encode_end_ticks;
			root_event.cpu_submit_ticks = slot.cpu_submit_ticks;
			root_event.cpu_completed_ticks = slot.cpu_completed_ticks;
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
				event.lane_index = 0;
				event.type = GpuTimingEventType::Scope;
				event.timestamp_source = GpuTimingTimestampSource::D3D11Timestamp;
				event.timestamp_confidence = GpuTimingTimestampConfidence::Sampled;
				event.submission_id = slot.submission_id;
				event.command_buffer_id = slot.command_buffer_id;
				event.command_order_index = scope.command_order_index;
				event.start_offset_ms = scope_start_ticks >= start_ticks
					? ((f64)(scope_start_ticks - start_ticks) * 1000.0) / (f64)disjoint_data.Frequency
					: 0.0;
				event.elapsed_ms = ((f64)(scope_end_ticks - scope_start_ticks) * 1000.0) / (f64)disjoint_data.Frequency;
				event.cpu_encode_start_ticks = scope.cpu_encode_start_ticks;
				event.cpu_encode_end_ticks = scope.cpu_encode_end_ticks;
				event.cpu_submit_ticks = slot.cpu_submit_ticks;
				event.cpu_completed_ticks = slot.cpu_completed_ticks;
				snprintf(event.reads, sizeof(event.reads), "%s", scope.reads);
				snprintf(event.writes, sizeof(event.writes), "%s", scope.writes);
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
	slot.submission_id = state.next_submission_id++;
	slot.command_buffer_id = slot.submission_id;
	slot.pending = true;
	slot.active = true;
	slot.scope_count = 0;
	slot.active_scope_stack_count = 0;
	slot.cpu_encode_start_ticks = stm_now();
	slot.cpu_encode_end_ticks = 0;
	slot.cpu_submit_ticks = 0;
	slot.cpu_completed_ticks = 0;
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
	slot.cpu_encode_end_ticks = stm_now();
	slot.cpu_submit_ticks = slot.cpu_encode_end_ticks;
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
	scope.command_order_index = scope_index + 1;
	scope.cpu_encode_start_ticks = stm_now();
	scope.cpu_encode_end_ticks = 0;
	scope.reads[0] = '\0';
	gpu_frame_timings_consume_next_scope_writes(scope.writes, sizeof(scope.writes));
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
		scope.cpu_encode_end_ticks = stm_now();
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
static constexpr i32 GPU_FRAME_TIMING_METAL_METADATA_FRAME_COUNT = 32;

struct MetalGpuScopeCpuMetadata
{
	bool valid = false;
	bool cpu_only_marker = false;
	char name[CPU_TIMINGS_MAX_NAME_LENGTH] = "(unnamed)";
	i32 command_order_index = -1;
	u64 cpu_encode_start_ticks = 0;
	u64 cpu_encode_end_ticks = 0;
	char reads[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
	char writes[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
};

struct MetalGpuFrameCpuMetadata
{
	bool valid = false;
	i64 frame_index = -1;
	i64 submission_id = -1;
	i64 command_buffer_id = -1;
	i32 next_command_order_index = 1;
	u64 cpu_frame_begin_ticks = 0;
	u64 cpu_submit_ticks = 0;
	u64 cpu_scheduled_ticks = 0;
	u64 cpu_completed_ticks = 0;
	MetalGpuScopeCpuMetadata scopes[SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS] = {};
	i32 active_scope_stack[SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS] = {};
	i32 active_scope_stack_count = 0;
};

struct MetalGpuFrameTimingState
{
	MetalGpuFrameCpuMetadata frames[GPU_FRAME_TIMING_METAL_METADATA_FRAME_COUNT] = {};
	std::mutex mutex;
	i32 next_frame_index = 0;
	i64 active_frame_index = -1;
};

static MetalGpuFrameTimingState& metal_gpu_frame_timing_state()
{
	static MetalGpuFrameTimingState state;
	return state;
}

static void gpu_frame_timings_metal_set_scope_status(bool in_scope_timing_initialized)
{
	const sg_mtl_gpu_scope_timing_caps caps = sg_mtl_query_gpu_scope_timing_caps();
	char status_message[GPU_TIMINGS_MAX_DEPENDENCY_TEXT_LENGTH] = {};
	if (in_scope_timing_initialized)
	{
		if (caps.render_boundary || caps.compute_boundary)
		{
			snprintf(
				status_message,
				sizeof(status_message),
				"Metal GPU scope timestamps: %s%s%s%s%s",
				caps.render_boundary ? "render" : "",
				(caps.render_boundary && caps.compute_boundary) ? " + " : "",
				caps.compute_boundary ? "compute" : "",
				caps.stage_boundary ? "; stage samples available" : "",
				caps.stage_boundary ? "" : ""
			);
		}
		else if (caps.stage_boundary)
		{
			snprintf(
				status_message,
				sizeof(status_message),
				"Metal GPU timestamps: stage-boundary pass samples; explicit scopes use CPU markers"
			);
		}
		else
		{
			snprintf(status_message, sizeof(status_message), "%s", "Metal GPU scope timestamps unavailable; showing CPU markers");
		}
	}
	else if (!caps.timestamp_counter_set)
	{
		snprintf(
			status_message,
			sizeof(status_message),
			"Metal GPU scope timestamps unsupported: MTLCommonCounterSetTimestamp unavailable; showing CPU markers"
		);
	}
	else
	{
		snprintf(
			status_message,
			sizeof(status_message),
			"Metal GPU scope timestamps unsupported: render=%s compute=%s stage=%s; showing CPU markers",
			caps.render_boundary ? "yes" : "no",
			caps.compute_boundary ? "yes" : "no",
			caps.stage_boundary ? "yes" : "no"
		);
	}
	gpu_timings_set_status_message(status_message);
}

static MetalGpuFrameCpuMetadata* gpu_frame_timings_find_metal_frame_locked(MetalGpuFrameTimingState& state, i64 in_frame_index)
{
	for (MetalGpuFrameCpuMetadata& frame : state.frames)
	{
		if (frame.valid && frame.frame_index == in_frame_index)
		{
			return &frame;
		}
	}
	return nullptr;
}

static MetalGpuFrameCpuMetadata& gpu_frame_timings_prepare_metal_frame_locked(MetalGpuFrameTimingState& state, i64 in_frame_index)
{
	if (MetalGpuFrameCpuMetadata* existing_frame = gpu_frame_timings_find_metal_frame_locked(state, in_frame_index))
	{
		*existing_frame = {};
		existing_frame->valid = true;
		existing_frame->frame_index = in_frame_index;
		return *existing_frame;
	}

	MetalGpuFrameCpuMetadata& frame = state.frames[state.next_frame_index];
	state.next_frame_index = (state.next_frame_index + 1) % GPU_FRAME_TIMING_METAL_METADATA_FRAME_COUNT;
	frame = {};
	frame.valid = true;
	frame.frame_index = in_frame_index;
	return frame;
}

static GpuTimingEventType gpu_frame_timings_metal_event_type(sg_mtl_gpu_timing_event_type in_type)
{
	switch (in_type)
	{
		case SG_MTL_GPU_TIMING_EVENT_FRAME: return GpuTimingEventType::Frame;
		case SG_MTL_GPU_TIMING_EVENT_RENDER_PASS: return GpuTimingEventType::RenderPass;
		case SG_MTL_GPU_TIMING_EVENT_COMPUTE_PASS: return GpuTimingEventType::ComputePass;
		case SG_MTL_GPU_TIMING_EVENT_SCOPE: return GpuTimingEventType::Scope;
		case SG_MTL_GPU_TIMING_EVENT_UNKNOWN:
		default: return GpuTimingEventType::Unknown;
	}
}

static bool gpu_frame_timings_metal_is_duplicate_pass_scope(
	const sg_mtl_gpu_timing_event* in_events,
	i32 in_event_count,
	i32 in_event_index)
{
	const sg_mtl_gpu_timing_event& source_event = in_events[in_event_index];
	if (source_event.type != SG_MTL_GPU_TIMING_EVENT_SCOPE)
	{
		return false;
	}

	for (i32 event_index = 0; event_index < in_event_count; ++event_index)
	{
		if (event_index == in_event_index)
		{
			continue;
		}

		const sg_mtl_gpu_timing_event& candidate_event = in_events[event_index];
		const bool is_pass_event =
			candidate_event.type == SG_MTL_GPU_TIMING_EVENT_RENDER_PASS ||
			candidate_event.type == SG_MTL_GPU_TIMING_EVENT_COMPUTE_PASS;
		if (is_pass_event && strcmp(candidate_event.name, source_event.name) == 0)
		{
			return true;
		}
	}

	return false;
}

static bool gpu_frame_timings_metal_has_pass_event_named(
	const sg_mtl_gpu_timing_event* in_events,
	i32 in_event_count,
	const char* in_name)
{
	if (!in_events || !in_name)
	{
		return false;
	}

	for (i32 event_index = 0; event_index < in_event_count; ++event_index)
	{
		const sg_mtl_gpu_timing_event& event = in_events[event_index];
		const bool is_pass_event =
			event.type == SG_MTL_GPU_TIMING_EVENT_RENDER_PASS ||
			event.type == SG_MTL_GPU_TIMING_EVENT_COMPUTE_PASS;
		if (is_pass_event && strcmp(event.name, in_name) == 0)
		{
			return true;
		}
	}
	return false;
}

static void gpu_frame_timings_metal_lifecycle(
	i64 in_frame_index,
	i64 in_command_buffer_id,
	i64 in_submission_id,
	sg_mtl_gpu_frame_lifecycle_event in_event,
	void* in_user_data)
{
	(void)in_user_data;
	MetalGpuFrameTimingState& state = metal_gpu_frame_timing_state();
	std::lock_guard<std::mutex> lock(state.mutex);
	MetalGpuFrameCpuMetadata* frame = gpu_frame_timings_find_metal_frame_locked(state, in_frame_index);
	if (!frame)
	{
		return;
	}

	frame->command_buffer_id = in_command_buffer_id;
	frame->submission_id = in_submission_id;
	const u64 now_ticks = stm_now();
	switch (in_event)
	{
		case SG_MTL_GPU_FRAME_LIFECYCLE_CREATED:
			break;
		case SG_MTL_GPU_FRAME_LIFECYCLE_COMMITTED:
			frame->cpu_submit_ticks = now_ticks;
			break;
		case SG_MTL_GPU_FRAME_LIFECYCLE_SCHEDULED:
			frame->cpu_scheduled_ticks = now_ticks;
			break;
		case SG_MTL_GPU_FRAME_LIFECYCLE_COMPLETED:
			frame->cpu_completed_ticks = now_ticks;
			break;
	}
}

static f64 gpu_frame_timings_metal_marker_elapsed_ms(const MetalGpuScopeCpuMetadata& marker)
{
	if (marker.cpu_encode_start_ticks == 0 || marker.cpu_encode_end_ticks == 0 || marker.cpu_encode_end_ticks < marker.cpu_encode_start_ticks)
	{
		return 0.001;
	}

	return std::max(0.001, stm_ms(stm_diff(marker.cpu_encode_end_ticks, marker.cpu_encode_start_ticks)));
}

static void gpu_frame_timings_metal_fill_marker_event(
	GpuTimingEvent& event,
	const MetalGpuScopeCpuMetadata& marker,
	const MetalGpuFrameCpuMetadata& frame_metadata)
{
	gpu_timing_event_set_name(event, marker.name);
	event.depth = 1;
	event.parent_index = 0;
	event.lane_index = 0;
	event.type = GpuTimingEventType::Scope;
	event.timestamp_source = GpuTimingTimestampSource::CpuOnlyMarker;
	event.timestamp_confidence = GpuTimingTimestampConfidence::Unavailable;
	event.submission_id = frame_metadata.submission_id;
	event.command_buffer_id = frame_metadata.command_buffer_id;
	event.command_order_index = marker.command_order_index;
	event.start_offset_ms = 0.0;
	event.elapsed_ms = gpu_frame_timings_metal_marker_elapsed_ms(marker);
	event.cpu_encode_start_ticks = marker.cpu_encode_start_ticks;
	event.cpu_encode_end_ticks = marker.cpu_encode_end_ticks;
	event.cpu_submit_ticks = frame_metadata.cpu_submit_ticks;
	event.cpu_scheduled_ticks = frame_metadata.cpu_scheduled_ticks;
	event.cpu_completed_ticks = frame_metadata.cpu_completed_ticks;
	snprintf(event.reads, sizeof(event.reads), "%s", marker.reads);
	snprintf(event.writes, sizeof(event.writes), "%s", marker.writes);
	event.valid = true;
}

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

	MetalGpuFrameCpuMetadata frame_metadata_storage = {};
	bool has_frame_metadata = false;
	{
		MetalGpuFrameTimingState& state = metal_gpu_frame_timing_state();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (const MetalGpuFrameCpuMetadata* frame_metadata = gpu_frame_timings_find_metal_frame_locked(state, in_frame_index))
		{
			frame_metadata_storage = *frame_metadata;
			has_frame_metadata = true;
		}
	}
	const MetalGpuFrameCpuMetadata* frame_metadata = has_frame_metadata ? &frame_metadata_storage : nullptr;
	GpuTimingEvent events[SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS + 1] = {};
	const i32 source_event_count = std::min(in_event_count, (i32)(sizeof(events) / sizeof(events[0])));
	i32 event_count = 0;
	for (i32 event_index = 0; event_index < source_event_count; ++event_index)
	{
		const sg_mtl_gpu_timing_event& source_event = in_events[event_index];
		if (gpu_frame_timings_metal_is_duplicate_pass_scope(in_events, source_event_count, event_index))
		{
			continue;
		}

		GpuTimingEvent& event = events[event_count++];
		gpu_timing_event_set_name(event, source_event.name);
		event.depth = source_event.depth;
		event.parent_index = source_event.parent_index;
		event.lane_index = source_event.lane_index;
		event.type = gpu_frame_timings_metal_event_type(source_event.type);
		if (event.type == GpuTimingEventType::Frame)
		{
			event.timestamp_source = GpuTimingTimestampSource::CommandBuffer;
			event.timestamp_confidence = GpuTimingTimestampConfidence::Authoritative;
		}
		else if (event.type == GpuTimingEventType::Scope)
		{
			event.timestamp_source = GpuTimingTimestampSource::MetalEncoderSample;
			event.timestamp_confidence = GpuTimingTimestampConfidence::Sampled;
		}
		else if (event.type == GpuTimingEventType::RenderPass || event.type == GpuTimingEventType::ComputePass)
		{
			event.timestamp_source = GpuTimingTimestampSource::MetalStageSample;
			event.timestamp_confidence = GpuTimingTimestampConfidence::Approximate;
		}
		event.submission_id = frame_metadata ? frame_metadata->submission_id : -1;
		event.command_buffer_id = frame_metadata ? frame_metadata->command_buffer_id : -1;
		event.command_order_index = source_event.event_index >= 0 ? source_event.event_index + 1 : 0;
		event.start_offset_ms = source_event.start_offset_ms;
		event.elapsed_ms = source_event.elapsed_ms;
		event.cpu_submit_ticks = frame_metadata ? frame_metadata->cpu_submit_ticks : 0;
		event.cpu_scheduled_ticks = frame_metadata ? frame_metadata->cpu_scheduled_ticks : 0;
		event.cpu_completed_ticks = frame_metadata ? frame_metadata->cpu_completed_ticks : 0;
		if (source_event.event_index >= 0 && frame_metadata && source_event.event_index < SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS)
		{
			const MetalGpuScopeCpuMetadata& scope_metadata = frame_metadata->scopes[source_event.event_index];
			if (scope_metadata.valid)
			{
				event.command_order_index = scope_metadata.command_order_index;
				event.cpu_encode_start_ticks = scope_metadata.cpu_encode_start_ticks;
				event.cpu_encode_end_ticks = scope_metadata.cpu_encode_end_ticks;
				snprintf(event.reads, sizeof(event.reads), "%s", scope_metadata.reads);
				snprintf(event.writes, sizeof(event.writes), "%s", scope_metadata.writes);
			}
		}
		else if (source_event.event_index < 0 && frame_metadata)
		{
			event.cpu_encode_start_ticks = frame_metadata->cpu_frame_begin_ticks;
			event.cpu_encode_end_ticks = frame_metadata->cpu_submit_ticks;
		}
		event.valid = source_event.valid;
	}

	if (frame_metadata)
	{
		for (
			i32 marker_index = 0;
			marker_index < SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS && event_count < (i32)(sizeof(events) / sizeof(events[0]));
			++marker_index
		)
		{
			const MetalGpuScopeCpuMetadata& marker = frame_metadata->scopes[marker_index];
			if (!marker.valid || !marker.cpu_only_marker)
			{
				continue;
			}
			if (gpu_frame_timings_metal_has_pass_event_named(in_events, source_event_count, marker.name))
			{
				continue;
			}

			gpu_frame_timings_metal_fill_marker_event(events[event_count++], marker, *frame_metadata);
		}
	}

	gpu_timings_record_completed_frame_events(in_frame_index, events, event_count);
}

static i32 gpu_frame_timings_begin_scope(const char* in_name)
{
	const u64 start_ticks = stm_now();
	const i32 scope_index = sg_mtl_begin_gpu_scope_timing(in_name);
	MetalGpuFrameTimingState& state = metal_gpu_frame_timing_state();
	std::lock_guard<std::mutex> lock(state.mutex);
	MetalGpuFrameCpuMetadata* frame = gpu_frame_timings_find_metal_frame_locked(state, state.active_frame_index);
	if (scope_index >= 0)
	{
		if (frame && scope_index < SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS)
		{
			MetalGpuScopeCpuMetadata& scope = frame->scopes[scope_index];
			scope = {};
			scope.valid = true;
			snprintf(scope.name, sizeof(scope.name), "%s", in_name ? in_name : "(unnamed)");
			scope.command_order_index = frame->next_command_order_index++;
			scope.cpu_encode_start_ticks = start_ticks;
			gpu_frame_timings_consume_next_scope_writes(scope.writes, sizeof(scope.writes));
			if (frame->active_scope_stack_count < SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS)
			{
				frame->active_scope_stack[frame->active_scope_stack_count++] = scope_index;
			}
		}
	}
	else if (frame && frame->next_command_order_index < SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS)
	{
		const i32 marker_index = frame->next_command_order_index++;
		MetalGpuScopeCpuMetadata& marker = frame->scopes[marker_index];
		marker = {};
		marker.valid = true;
		marker.cpu_only_marker = true;
		snprintf(marker.name, sizeof(marker.name), "%s", in_name ? in_name : "(unnamed)");
		marker.command_order_index = marker_index;
		marker.cpu_encode_start_ticks = start_ticks;
		gpu_frame_timings_consume_next_scope_writes(marker.writes, sizeof(marker.writes));
		if (frame->active_scope_stack_count < SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS)
		{
			frame->active_scope_stack[frame->active_scope_stack_count++] = marker_index;
		}
		return -marker_index - 2;
	}
	return scope_index;
}

static void gpu_frame_timings_end_scope(i32 in_scope_index)
{
	const i32 metadata_scope_index = in_scope_index >= 0 ? in_scope_index : (-in_scope_index - 2);
	if (metadata_scope_index >= 0)
	{
		MetalGpuFrameTimingState& state = metal_gpu_frame_timing_state();
		std::lock_guard<std::mutex> lock(state.mutex);
		MetalGpuFrameCpuMetadata* frame = gpu_frame_timings_find_metal_frame_locked(state, state.active_frame_index);
		if (frame && metadata_scope_index < SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS)
		{
			frame->scopes[metadata_scope_index].cpu_encode_end_ticks = stm_now();
			if (
				frame->active_scope_stack_count > 0 &&
				frame->active_scope_stack[frame->active_scope_stack_count - 1] == metadata_scope_index
			)
			{
				--frame->active_scope_stack_count;
			}
		}
	}
	if (in_scope_index >= 0)
	{
		sg_mtl_end_gpu_scope_timing(in_scope_index);
	}
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

	MetalGpuFrameCpuMetadata frame_metadata_storage = {};
	bool has_frame_metadata = false;
	{
		MetalGpuFrameTimingState& state = metal_gpu_frame_timing_state();
		std::lock_guard<std::mutex> lock(state.mutex);
		if (const MetalGpuFrameCpuMetadata* frame_metadata = gpu_frame_timings_find_metal_frame_locked(state, in_frame_index))
		{
			frame_metadata_storage = *frame_metadata;
			has_frame_metadata = true;
		}
	}
	const MetalGpuFrameCpuMetadata* frame_metadata = has_frame_metadata ? &frame_metadata_storage : nullptr;
	GpuTimingEvent events[SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS + 1] = {};
	i32 event_count = 0;
	GpuTimingEvent& root_event = events[event_count++];
	gpu_timing_event_set_name(root_event, "GPU Frame");
	root_event.depth = 0;
	root_event.parent_index = -1;
	root_event.lane_index = 0;
	root_event.type = GpuTimingEventType::Frame;
	root_event.timestamp_source = GpuTimingTimestampSource::CommandBuffer;
	root_event.timestamp_confidence = GpuTimingTimestampConfidence::Authoritative;
	root_event.submission_id = frame_metadata ? frame_metadata->submission_id : -1;
	root_event.command_buffer_id = frame_metadata ? frame_metadata->command_buffer_id : -1;
	root_event.command_order_index = 0;
	root_event.start_offset_ms = 0.0;
	root_event.elapsed_ms = in_gpu_end_sec > in_gpu_start_sec
		? (in_gpu_end_sec - in_gpu_start_sec) * 1000.0
		: 0.0;
	root_event.cpu_encode_start_ticks = frame_metadata ? frame_metadata->cpu_frame_begin_ticks : 0;
	root_event.cpu_encode_end_ticks = frame_metadata ? frame_metadata->cpu_submit_ticks : 0;
	root_event.cpu_submit_ticks = frame_metadata ? frame_metadata->cpu_submit_ticks : 0;
	root_event.cpu_scheduled_ticks = frame_metadata ? frame_metadata->cpu_scheduled_ticks : 0;
	root_event.cpu_completed_ticks = frame_metadata ? frame_metadata->cpu_completed_ticks : 0;
	root_event.valid = root_event.elapsed_ms > 0.0;
	if (!root_event.valid)
	{
		root_event.timestamp_confidence = GpuTimingTimestampConfidence::Unavailable;
	}

	if (frame_metadata)
	{
		for (
			i32 marker_index = 0;
			marker_index < SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS && event_count < (i32)(sizeof(events) / sizeof(events[0]));
			++marker_index
		)
		{
			const MetalGpuScopeCpuMetadata& marker = frame_metadata->scopes[marker_index];
			if (!marker.valid || !marker.cpu_only_marker)
			{
				continue;
			}

			gpu_frame_timings_metal_fill_marker_event(events[event_count++], marker, *frame_metadata);
		}
	}
	if (root_event.valid || event_count > 1)
	{
		gpu_timings_record_completed_frame_events(in_frame_index, events, event_count);
	}
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
		sg_mtl_set_gpu_frame_lifecycle_callback(gpu_frame_timings_metal_lifecycle, nullptr);
		sg_mtl_set_gpu_frame_wait_callback(gpu_frame_timings_metal_wait, nullptr);
		const bool scope_timing_initialized = sg_mtl_init_gpu_scope_timing();
		gpu_frame_timings_metal_set_scope_status(scope_timing_initialized);
		if (scope_timing_initialized)
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
		sg_mtl_set_gpu_frame_lifecycle_callback(nullptr, nullptr);
		sg_mtl_set_gpu_frame_wait_callback(nullptr, nullptr);
		sg_mtl_set_gpu_scope_timing_callback(nullptr, nullptr);
		sg_mtl_shutdown_gpu_scope_timing();
		sg_mtl_set_gpu_frame_timing_frame_index(-1);
		gpu_timings_set_status_message("");
		gpu_timings_set_available(false, "GPU timings shut down");
	#elif defined(SOKOL_D3D11)
		gpu_frame_timings_shutdown_d3d11();
		gpu_timings_set_available(false, "GPU timings shut down");
	#endif
}

static void gpu_frame_timings_begin_frame(i64 in_frame_index)
{
	#if defined(SOKOL_METAL)
		MetalGpuFrameTimingState& state = metal_gpu_frame_timing_state();
		std::lock_guard<std::mutex> lock(state.mutex);
		state.active_frame_index = in_frame_index;
		if (in_frame_index >= 0)
		{
			MetalGpuFrameCpuMetadata& frame = gpu_frame_timings_prepare_metal_frame_locked(state, in_frame_index);
			frame.cpu_frame_begin_ticks = stm_now();
		}
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
	#elif defined(SOKOL_METAL)
		(void)in_frame_index;
	#else
		(void)in_frame_index;
	#endif
}

static void gpu_frame_timings_record_bindings_reads(const sg_bindings* in_bindings)
{
	if (!in_bindings)
	{
		return;
	}

	#if defined(SOKOL_D3D11)
		D3D11GpuFrameTimingState& state = d3d11_gpu_frame_timing_state();
		if (!state.initialized || state.active_slot_index < 0)
		{
			return;
		}

		D3D11GpuFrameTimingSlot& slot = state.slots[state.active_slot_index];
		if (!slot.active || slot.active_scope_stack_count <= 0)
		{
			return;
		}

		const i32 scope_index = slot.active_scope_stack[slot.active_scope_stack_count - 1];
		if (scope_index >= 0 && scope_index < slot.scope_count)
		{
			gpu_frame_timings_append_bindings_reads_to_text(in_bindings, slot.scopes[scope_index].reads, sizeof(slot.scopes[scope_index].reads));
		}
	#elif defined(SOKOL_METAL)
		MetalGpuFrameTimingState& metal_state = metal_gpu_frame_timing_state();
		std::lock_guard<std::mutex> lock(metal_state.mutex);
		MetalGpuFrameCpuMetadata* frame = gpu_frame_timings_find_metal_frame_locked(metal_state, metal_state.active_frame_index);
		if (!frame || frame->active_scope_stack_count <= 0)
		{
			return;
		}

		const i32 scope_index = frame->active_scope_stack[frame->active_scope_stack_count - 1];
		if (scope_index >= 0 && scope_index < SG_MTL_GPU_SCOPE_TIMING_MAX_EVENTS)
		{
			gpu_frame_timings_append_bindings_reads_to_text(in_bindings, frame->scopes[scope_index].reads, sizeof(frame->scopes[scope_index].reads));
		}
	#else
		(void)in_bindings;
	#endif
}

#else

static void gpu_frame_timings_init() {}
static void gpu_frame_timings_shutdown() {}
static void gpu_frame_timings_begin_frame(i64) {}
static void gpu_frame_timings_end_frame(i64) {}
static i32 gpu_frame_timings_begin_scope(const char*) { return -1; }
static void gpu_frame_timings_end_scope(i32) {}
static void gpu_frame_timings_set_next_scope_writes(const char*) {}
static void gpu_frame_timings_record_bindings_reads(const sg_bindings*) {}

#endif

static void gpu_apply_bindings(const sg_bindings* in_bindings)
{
	gpu_frame_timings_record_bindings_reads(in_bindings);
	sg_apply_bindings(in_bindings);
}

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
		if (timing_scope_index != -1)
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

template<typename Callback>
static void gpu_execute_compute_pass(const char* debug_label, sg_pipeline pipeline, Callback callback)
{
	const char* label = debug_label ? debug_label : "(unnamed compute)";
	{
		CPU_TIMING_BACKEND_SCOPE("sg_begin_pass", label);
		sg_begin_pass((sg_pass) { .compute = true, .label = label });
	}
	{
		GpuDebugScope debug_scope(label);
		sg_apply_pipeline(pipeline);
		callback();
	}
	{
		CPU_TIMING_BACKEND_SCOPE("sg_end_pass", label);
		sg_end_pass();
	}
}

#define GPU_DEBUG_SCOPE_JOIN_INNER(a, b) a##b
#define GPU_DEBUG_SCOPE_JOIN(a, b) GPU_DEBUG_SCOPE_JOIN_INNER(a, b)
#define GPU_DEBUG_SCOPE(name) GpuDebugScope GPU_DEBUG_SCOPE_JOIN(gpu_debug_scope_, __LINE__)(name)
