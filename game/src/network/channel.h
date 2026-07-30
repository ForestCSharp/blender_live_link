#pragma once

#include <mutex>
#include <optional>
#include <queue>
#include <utility>

template <typename T>
struct Channel
{
public:
	void send(T&& data)
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		data_queue.push(std::move(data));
	}

	std::optional<T> receive()
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		if (!data_queue.empty())
		{
			std::optional<T> out_optional_value(std::move(data_queue.front()));
			data_queue.pop();
			return out_optional_value;
		}
		return std::nullopt;
	}
	
protected:
	std::queue<T> data_queue;
	std::mutex queue_mutex;
};
