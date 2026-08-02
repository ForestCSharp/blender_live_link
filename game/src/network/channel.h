#pragma once

#include "core/dynamic_array.h"

#include <mutex>
#include <optional>
#include <utility>

template <typename T>
struct Channel
{
public:
	void send(T&& data)
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		data_queue.add(std::move(data));
	}

	std::optional<T> receive()
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		if (read_index < data_queue.length())
		{
			std::optional<T> out_optional_value(std::move(data_queue[read_index]));
			++read_index;
			if (read_index == data_queue.length())
			{
				data_queue.clear();
				read_index = 0;
			}
			return out_optional_value;
		}
		return std::nullopt;
	}
	
protected:
	DynamicArray<T> data_queue;
	size_t read_index = 0;
	std::mutex queue_mutex;
};
