#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <thread>

#include "core/dynamic_array.h"
#include "network/channel.h"

struct TrackedValue
{
	static inline i32 live_count = 0;
	static inline i32 copy_count = 0;
	static inline i32 move_count = 0;

	i32 value = 0;

	TrackedValue() { ++live_count; }
	explicit TrackedValue(i32 in_value) : value(in_value) { ++live_count; }
	TrackedValue(const TrackedValue& other) : value(other.value)
	{
		++live_count;
		++copy_count;
	}
	TrackedValue(TrackedValue&& other) noexcept : value(other.value)
	{
		other.value = -1;
		++live_count;
		++move_count;
	}
	TrackedValue& operator=(const TrackedValue&) = default;
	TrackedValue& operator=(TrackedValue&&) = default;
	~TrackedValue() { --live_count; }
};

struct alignas(64) AlignedValue
{
	u64 value = 0;
};

void test_trivial_storage()
{
	DynamicArray<i32> values;
	assert(values.empty());
	values.reserve(24);
	assert(values.capacity() >= 24);
	values.resize(4, 7);
	assert(values.length() == 4);
	for (i32 value : values) assert(value == 7);

	values.resize(values.capacity(), 3);
	values.add(values[0]);
	assert(values.last() == 7);
	values.resize(4);
	values.add(values[0]);
	assert(values.last() == 7);
	values.resize(8);
	assert(values.length() == 8);
	assert(values[5] == 0);
	values.resize(2);
	assert(values.length() == 2);
	values.clear();
	assert(values.empty());
	assert(values.capacity() >= 24);
	values.reset();
	assert(values.data() == nullptr);
	assert(values.capacity() == 0);

	DynamicArray<u8> bytes;
	bytes.add_uninitialized(32);
	for (size_t index = 0; index < bytes.length(); ++index) bytes[index] = (u8)index;
	for (size_t index = 0; index < bytes.length(); ++index) assert(bytes[index] == (u8)index);
}

void test_non_trivial_storage()
{
	TrackedValue::live_count = 0;
	TrackedValue::copy_count = 0;
	TrackedValue::move_count = 0;
	{
		DynamicArray<TrackedValue> values;
		for (i32 index = 0; index < 40; ++index) values.emplace(index);
		assert(values.length() == 40);
		assert(TrackedValue::live_count == 40);
		assert(TrackedValue::move_count > 0);

		DynamicArray<TrackedValue> copied = values;
		assert(copied.length() == values.length());
		assert(copied[17].value == 17);
		assert(TrackedValue::copy_count >= 40);

		DynamicArray<TrackedValue> moved = std::move(copied);
		assert(copied.empty());
		assert(moved.length() == 40);
		moved.resize(12);
		assert(moved.length() == 12);
		moved.resize(20, TrackedValue(99));
		for (size_t index = 12; index < moved.length(); ++index) assert(moved[index].value == 99);
		moved.clear();
		assert(moved.empty());
	}
	assert(TrackedValue::live_count == 0);
}

void test_aligned_storage()
{
	DynamicArray<AlignedValue> values;
	values.resize(4);
	assert((uintptr_t)values.data() % alignof(AlignedValue) == 0);
	values.reserve(32);
	assert((uintptr_t)values.data() % alignof(AlignedValue) == 0);
}

void test_channel_fifo_and_reuse()
{
	Channel<std::unique_ptr<i32>> channel;
	assert(!channel.receive());
	for (i32 index = 0; index < 20; ++index)
	{
		channel.send(std::make_unique<i32>(index));
	}
	for (i32 index = 0; index < 20; ++index)
	{
		auto received = channel.receive();
		assert(received && **received == index);
	}
	assert(!channel.receive());
	channel.send(std::make_unique<i32>(42));
	auto reused = channel.receive();
	assert(reused && **reused == 42);
}

void test_channel_concurrency()
{
	Channel<i32> channel;
	std::atomic<bool> producer_done = false;
	std::thread producer([&]() {
		for (i32 index = 0; index < 1000; ++index)
		{
			i32 value = index;
			channel.send(std::move(value));
		}
		producer_done = true;
	});

	i32 expected = 0;
	while (!producer_done || expected < 1000)
	{
		if (auto received = channel.receive())
		{
			assert(*received == expected);
			++expected;
		}
		else
		{
			std::this_thread::yield();
		}
	}
	producer.join();
	assert(expected == 1000);
	assert(!channel.receive());
}

int main()
{
	test_trivial_storage();
	test_non_trivial_storage();
	test_aligned_storage();
	test_channel_fifo_and_reuse();
	test_channel_concurrency();
	return 0;
}
