#pragma once

#include "core/types.h"
#include "render/vulkan_context.h"

#include <cassert>
#include <cstring>
#include <optional>
#include <string>

using std::optional;

// Buffer usage info (mirrors the shape of sokol's sg_buffer_usage in game/)
struct GpuBufferUsage
{
	bool vertex_buffer = false;
	bool index_buffer = false;
	bool storage_buffer = false;
	bool uniform_buffer = false;
	bool stream_update = false;
	bool prefer_device_local = false;
	bool transfer_src = false;
	bool readback = false;
};

// Static (non-stream) buffers default to device-local + staging upload on
// discrete GPUs. Apple Silicon is UMA, so host-visible mapped is the default
// there; GAME2_FORCE_DEVICE_LOCAL=1 exercises the staging path anyway.
inline bool gpu_buffer_default_device_local()
{
	static const bool force_device_local = getenv("GAME2_FORCE_DEVICE_LOCAL") != nullptr;
#if defined(__APPLE__)
	return force_device_local;
#else
	return true;
#endif
}

inline u64 gpu_next_resource_generation()
{
	static u64 generation = 1;
	return generation++;
}

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
	GpuBufferUsage usage;

	/* Debug Label */
	const char* label = nullptr;
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
	}

	bool is_gpu_buffer_valid()
	{
		return gpu_buffer.has_value();
	}

	bool is_dynamic() const
	{
		return usage.stream_update;
	}

	// Lazily creates the VkBuffer on first call. Buffers are host-visible and
	// persistently mapped (Apple Silicon is UMA, so no staging copy needed).
	VkBuffer get_gpu_buffer()
	{
		if (!gpu_buffer.has_value())
		{
			assert(g_vulkan_context != nullptr);

			VkBufferUsageFlags usage_flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			if (usage.vertex_buffer)	{ usage_flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; }
			if (usage.index_buffer)		{ usage_flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT; }
			if (usage.storage_buffer)	{ usage_flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; }
			if (usage.uniform_buffer)	{ usage_flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT; }
			if (usage.transfer_src)		{ usage_flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT; }

			VkBufferCreateInfo buffer_create_info = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = size,
				.usage = usage_flags,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};

			const bool wants_device_local = !usage.stream_update
				&& (usage.prefer_device_local || gpu_buffer_default_device_local());

			VmaAllocationCreateInfo allocation_create_info = {
				.usage = VMA_MEMORY_USAGE_AUTO,
			};
			if (wants_device_local)
			{
				allocation_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			}
			else
			{
				allocation_create_info.flags = (usage.readback
					? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
					: VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)
					| VMA_ALLOCATION_CREATE_MAPPED_BIT;
			}

			VkBuffer new_buffer = VK_NULL_HANDLE;
			VmaAllocationInfo allocation_info = {};
			VK_CHECK(vmaCreateBuffer(
				g_vulkan_context->allocator,
				&buffer_create_info,
				&allocation_create_info,
				&new_buffer,
				&allocation,
				&allocation_info
			));

			mapped_data = allocation_info.pMappedData;
			gpu_buffer = new_buffer;
			generation = gpu_next_resource_generation();
			if (label.has_value())
			{
				vmaSetAllocationName(g_vulkan_context->allocator, allocation, label->c_str());
				vulkan_set_object_name(g_vulkan_context, VK_OBJECT_TYPE_BUFFER, (u64)new_buffer, label->c_str());
			}

			if (data != nullptr && size > 0)
			{
				if (!wants_device_local)
				{
					memcpy(mapped_data, data, size);
					VK_CHECK(vmaFlushAllocation(g_vulkan_context->allocator, allocation, 0, size));
				}
				else
				{
					upload_to_device_local(new_buffer);
				}
			}
		}

		return *gpu_buffer;
	}

	void update_gpu_buffer(const T* in_data, u64 in_size)
	{
		assert(is_dynamic());
		assert(in_size <= size);
		get_gpu_buffer();
		memcpy(mapped_data, in_data, in_size);
		VK_CHECK(vmaFlushAllocation(g_vulkan_context->allocator, allocation, 0, in_size));
	}

	void read_gpu_buffer(T* out_data, u64 in_size)
	{
		assert(usage.readback);
		assert(in_size <= size);
		get_gpu_buffer();
		VK_CHECK(vmaInvalidateAllocation(g_vulkan_context->allocator, allocation, 0, in_size));
		memcpy(out_data, mapped_data, in_size);
	}

	// Destruction is deferred until no in-flight frame can reference the buffer
	void destroy_gpu_buffer()
	{
		if (gpu_buffer.has_value())
		{
			vulkan_context_deferred_destroy_buffer(g_vulkan_context, *gpu_buffer, allocation);
			gpu_buffer.reset();
			allocation = VK_NULL_HANDLE;
			mapped_data = nullptr;
			generation = gpu_next_resource_generation();
		}
	}

	void set_label(const char* in_label)
	{
		if (in_label)
		{
			label = std::string(in_label);
		}
	}

	u64 length() const { return _length; }
	u64 resource_generation() const { return generation; }

protected:
	// Fills a device-local buffer: direct memcpy when the allocation happens
	// to be host-visible (UMA), otherwise via a throwaway staging buffer +
	// one-shot copy (safe to destroy immediately — the submit waits idle)
	void upload_to_device_local(VkBuffer in_target_buffer)
	{
		VkMemoryPropertyFlags memory_properties = 0;
		vmaGetAllocationMemoryProperties(g_vulkan_context->allocator, allocation, &memory_properties);

		if (memory_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
		{
			void* target_mapped = nullptr;
			VK_CHECK(vmaMapMemory(g_vulkan_context->allocator, allocation, &target_mapped));
			memcpy(target_mapped, data, size);
			VK_CHECK(vmaFlushAllocation(g_vulkan_context->allocator, allocation, 0, size));
			vmaUnmapMemory(g_vulkan_context->allocator, allocation);
			return;
		}

		if (vulkan_current_frame(g_vulkan_context).recording)
		{
			VkPipelineStageFlags2 dst_stage = 0;
			VkAccessFlags2 dst_access = 0;
			if (usage.vertex_buffer)
			{
				dst_stage |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
				dst_access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
			}
			if (usage.index_buffer)
			{
				dst_stage |= VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
				dst_access |= VK_ACCESS_2_INDEX_READ_BIT;
			}
			if (usage.storage_buffer)
			{
				dst_stage |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
						  | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
						  | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
				dst_access |= VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
			}
			if (usage.uniform_buffer)
			{
				dst_stage |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
				dst_access |= VK_ACCESS_2_UNIFORM_READ_BIT;
			}
			if (dst_stage == 0)
			{
				dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				dst_access = VK_ACCESS_2_MEMORY_READ_BIT;
			}
			vulkan_upload_record_buffer(
				g_vulkan_context,
				in_target_buffer,
				data,
				size,
				dst_stage,
				dst_access
			);
			return;
		}

		VkBufferCreateInfo staging_create_info = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = size,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};
		VmaAllocationCreateInfo staging_allocation_create_info = {
			.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
				   | VMA_ALLOCATION_CREATE_MAPPED_BIT,
			.usage = VMA_MEMORY_USAGE_AUTO,
		};

		VkBuffer staging_buffer = VK_NULL_HANDLE;
		VmaAllocation staging_allocation = VK_NULL_HANDLE;
		VmaAllocationInfo staging_allocation_info = {};
		VK_CHECK(vmaCreateBuffer(
			g_vulkan_context->allocator,
			&staging_create_info,
			&staging_allocation_create_info,
			&staging_buffer,
			&staging_allocation,
			&staging_allocation_info
		));
		vmaSetAllocationName(g_vulkan_context->allocator, staging_allocation, "Static Buffer Upload Staging");
		vulkan_set_object_name(g_vulkan_context, VK_OBJECT_TYPE_BUFFER, (u64)staging_buffer, "Static Buffer Upload Staging");

		memcpy(staging_allocation_info.pMappedData, data, size);
		VK_CHECK(vmaFlushAllocation(g_vulkan_context->allocator, staging_allocation, 0, size));
		g_vulkan_context->metrics.upload_bytes += size;
		g_vulkan_context->metrics.upload_requests += 1;

		const u64 copy_size = size;
		vulkan_context_immediate_submit(g_vulkan_context, [&](VkCommandBuffer in_command_buffer)
		{
			VkBufferCopy copy_region = {
				.srcOffset = 0,
				.dstOffset = 0,
				.size = copy_size,
			};
			vkCmdCopyBuffer(in_command_buffer, staging_buffer, in_target_buffer, 1, &copy_region);
		});

		vmaDestroyBuffer(g_vulkan_context->allocator, staging_buffer, staging_allocation);
	}

	/* Underlying Buffer Data */
	T* data = nullptr;

	/* Data Size */
	u64 size = 0;

	/* The element count of the buffer */
	u64 _length = 0;

	/* buffer usage info */
	GpuBufferUsage usage;

	// Buffer used for gpu operations
	optional<VkBuffer> gpu_buffer;
	VmaAllocation allocation = VK_NULL_HANDLE;
	void* mapped_data = nullptr;
	u64 generation = 0;

	// Optional Label
	optional<std::string> label;
};
