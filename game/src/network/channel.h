
#include <mutex>
#include <optional>
#include <queue>

template <typename T>
struct Channel
{
public:
	void send(const T& data)
	{
		queue_mutex.lock();	
		data_queue.push(data);
		queue_mutex.unlock();
	}

	std::optional<T> receive()
	{
		std::optional<T> out_optional_value;
		queue_mutex.lock();	
		if (data_queue.size() > 0)
		{
			out_optional_value = data_queue.front();
			data_queue.pop();
		}
		queue_mutex.unlock();
		return out_optional_value;
	}
	
protected:
	std::queue<T> data_queue;
	std::mutex queue_mutex;
};
