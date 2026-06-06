#pragma once

#include "core/types.h"
#include "sokol/sokol_gfx.h"

#include <cstdio>
#include <cstring>

static constexpr i32 GPU_PROFILER_MAX_TRACKED_RESOURCES = 2048;
static constexpr i32 GPU_PROFILER_MAX_RESOURCE_NAME_LENGTH = 96;

#if defined(WITH_DEBUG_UI) && WITH_DEBUG_UI

#include "core/timings.h"

struct GpuProfilerTrackedResource
{
	u32 id = SG_INVALID_ID;
	char name[GPU_PROFILER_MAX_RESOURCE_NAME_LENGTH] = {};
};

struct GpuProfilerResourceRegistry
{
	GpuProfilerTrackedResource views[GPU_PROFILER_MAX_TRACKED_RESOURCES] = {};
	i32 view_count = 0;
};

static GpuProfilerResourceRegistry& gpu_profiler_resource_registry()
{
	static GpuProfilerResourceRegistry registry;
	return registry;
}

static void gpu_profiler_register_view_name(sg_view in_view, const char* in_name)
{
	if (in_view.id == SG_INVALID_ID || !in_name || !in_name[0])
	{
		return;
	}

	GpuProfilerResourceRegistry& registry = gpu_profiler_resource_registry();
	for (i32 resource_index = 0; resource_index < registry.view_count; ++resource_index)
	{
		GpuProfilerTrackedResource& resource = registry.views[resource_index];
		if (resource.id == in_view.id)
		{
			snprintf(resource.name, sizeof(resource.name), "%s", in_name);
			return;
		}
	}

	if (registry.view_count >= GPU_PROFILER_MAX_TRACKED_RESOURCES)
	{
		return;
	}

	GpuProfilerTrackedResource& resource = registry.views[registry.view_count++];
	resource.id = in_view.id;
	snprintf(resource.name, sizeof(resource.name), "%s", in_name);
}

static const char* gpu_profiler_lookup_view_name(sg_view in_view)
{
	if (in_view.id == SG_INVALID_ID)
	{
		return nullptr;
	}

	GpuProfilerResourceRegistry& registry = gpu_profiler_resource_registry();
	for (i32 resource_index = 0; resource_index < registry.view_count; ++resource_index)
	{
		const GpuProfilerTrackedResource& resource = registry.views[resource_index];
		if (resource.id == in_view.id)
		{
			return resource.name;
		}
	}

	return nullptr;
}

static void gpu_profiler_append_dependency_name(char* io_text, size_t in_text_size, const char* in_name)
{
	if (!io_text || in_text_size == 0 || !in_name || !in_name[0])
	{
		return;
	}

	if (strstr(io_text, in_name))
	{
		return;
	}

	const size_t length = strlen(io_text);
	if (length >= in_text_size - 1)
	{
		return;
	}

	snprintf(io_text + length, in_text_size - length, "%s%s", length > 0 ? ", " : "", in_name);
}

#else

static void gpu_profiler_register_view_name(sg_view, const char*) {}
static const char* gpu_profiler_lookup_view_name(sg_view) { return nullptr; }
static void gpu_profiler_append_dependency_name(char*, size_t, const char*) {}

#endif
