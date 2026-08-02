#pragma once

#include "core/types.h"

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

// Contiguous engine-owned dynamic storage. Elements are constructed and
// destroyed explicitly so growth is safe for non-trivial C++ types.
template<typename T>
struct DynamicArray
{
public:
	DynamicArray() = default;
	~DynamicArray() { reset(); }

	DynamicArray(const DynamicArray& other)
	{
		reserve(other.length());
		try
		{
			for (const T& value : other)
			{
				new (&_data[_length]) T(value);
				++_length;
			}
		}
		catch (...)
		{
			reset();
			throw;
		}
	}

	DynamicArray(DynamicArray&& other) noexcept
		: _data(other._data), _length(other._length), _capacity(other._capacity)
	{
		other._data = nullptr;
		other._length = 0;
		other._capacity = 0;
	}

	DynamicArray& operator=(const DynamicArray& other)
	{
		if (this != &other)
		{
			DynamicArray copy(other);
			*this = std::move(copy);
		}
		return *this;
	}

	DynamicArray& operator=(DynamicArray&& other) noexcept
	{
		if (this != &other)
		{
			reset();
			_data = other._data;
			_length = other._length;
			_capacity = other._capacity;
			other._data = nullptr;
			other._length = 0;
			other._capacity = 0;
		}
		return *this;
	}

	void reserve(size_t in_capacity)
	{
		if (in_capacity <= _capacity) return;

		T* new_data = allocate(in_capacity);
		size_t constructed_count = 0;
		try
		{
			for (; constructed_count < _length; ++constructed_count)
			{
				new (&new_data[constructed_count]) T(std::move_if_noexcept(_data[constructed_count]));
			}
		}
		catch (...)
		{
			destroy_range(new_data, constructed_count);
			deallocate(new_data);
			throw;
		}

		destroy_range(_data, _length);
		deallocate(_data);
		_data = new_data;
		_capacity = in_capacity;
	}

	void resize(size_t in_length)
	{
		if (in_length < _length)
		{
			destroy_range(_data + in_length, _length - in_length);
			_length = in_length;
			return;
		}
		if (in_length == _length) return;

		ensure_capacity(in_length);
		const size_t old_length = _length;
		try
		{
			for (; _length < in_length; ++_length)
			{
				new (&_data[_length]) T();
			}
		}
		catch (...)
		{
			destroy_range(_data + old_length, _length - old_length);
			_length = old_length;
			throw;
		}
	}

	void resize(size_t in_length, const T& in_value)
	{
		if (in_length < _length)
		{
			destroy_range(_data + in_length, _length - in_length);
			_length = in_length;
			return;
		}
		if (in_length == _length) return;
		if (in_length > _capacity && contains_address(&in_value))
		{
			T copy(in_value);
			resize(in_length, copy);
			return;
		}

		ensure_capacity(in_length);
		const size_t old_length = _length;
		try
		{
			for (; _length < in_length; ++_length)
			{
				new (&_data[_length]) T(in_value);
			}
		}
		catch (...)
		{
			destroy_range(_data + old_length, _length - old_length);
			_length = old_length;
			throw;
		}
	}

	void add(const T& in_value)
	{
		if (_length == _capacity && contains_address(&in_value))
		{
			T copy(in_value);
			ensure_capacity(_length + 1);
			new (&_data[_length]) T(std::move(copy));
		}
		else
		{
			ensure_capacity(_length + 1);
			new (&_data[_length]) T(in_value);
		}
		++_length;
	}

	void add(T&& in_value)
	{
		if (_length == _capacity && contains_address(&in_value))
		{
			T moved(std::move(in_value));
			ensure_capacity(_length + 1);
			new (&_data[_length]) T(std::move(moved));
		}
		else
		{
			ensure_capacity(_length + 1);
			new (&_data[_length]) T(std::move(in_value));
		}
		++_length;
	}

	template<typename... Args>
	T& emplace(Args&&... args)
	{
		ensure_capacity(_length + 1);
		T* value = &_data[_length];
		new (value) T(std::forward<Args>(args)...);
		++_length;
		return *value;
	}

	void add_uninitialized(size_t in_count)
	{
		static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
			"add_uninitialized is only valid for trivial storage types");
		if (in_count > std::numeric_limits<size_t>::max() - _length)
		{
			throw std::bad_array_new_length();
		}
		ensure_capacity(_length + in_count);
		_length += in_count;
	}

	void pop()
	{
		if (_length == 0) return;
		--_length;
		destroy_range(&_data[_length], 1);
	}

	void clear()
	{
		destroy_range(_data, _length);
		_length = 0;
	}

	void reset()
	{
		clear();
		deallocate(_data);
		_data = nullptr;
		_capacity = 0;
	}

	size_t length() const { return _length; }
	size_t capacity() const { return _capacity; }
	bool empty() const { return _length == 0; }
	T* data() { return _data; }
	const T* data() const { return _data; }
	T& operator[](size_t in_index) { return _data[in_index]; }
	const T& operator[](size_t in_index) const { return _data[in_index]; }
	T& last() { return _data[_length - 1]; }
	const T& last() const { return _data[_length - 1]; }
	bool is_valid_index(i32 in_index) const { return in_index >= 0 && (size_t)in_index < _length; }

	T* begin() { return _data; }
	T* end() { return _data ? _data + _length : nullptr; }
	const T* begin() const { return _data; }
	const T* end() const { return _data ? _data + _length : nullptr; }

private:
	static T* allocate(size_t in_capacity)
	{
		if (in_capacity > std::numeric_limits<size_t>::max() / sizeof(T))
		{
			throw std::bad_array_new_length();
		}
		const size_t byte_count = sizeof(T) * in_capacity;
		if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
		{
			return (T*)::operator new(byte_count, std::align_val_t(alignof(T)));
		}
		return (T*)::operator new(byte_count);
	}

	static void deallocate(T* in_data)
	{
		if (!in_data) return;
		if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
		{
			::operator delete(in_data, std::align_val_t(alignof(T)));
		}
		else
		{
			::operator delete(in_data);
		}
	}

	static void destroy_range(T* in_data, size_t in_count)
	{
		if constexpr (!std::is_trivially_destructible_v<T>)
		{
			for (size_t index = 0; index < in_count; ++index)
			{
				in_data[index].~T();
			}
		}
	}

	void ensure_capacity(size_t in_required_capacity)
	{
		if (in_required_capacity <= _capacity) return;
		size_t new_capacity = _capacity > 0 ? _capacity : 8;
		while (new_capacity < in_required_capacity)
		{
			if (new_capacity > std::numeric_limits<size_t>::max() / 2)
			{
				new_capacity = in_required_capacity;
				break;
			}
			new_capacity *= 2;
		}
		reserve(new_capacity);
	}

	bool contains_address(const T* in_value) const
	{
		if (!_data) return false;
		const uintptr_t address = (uintptr_t)in_value;
		const uintptr_t begin_address = (uintptr_t)_data;
		const uintptr_t end_address = begin_address + sizeof(T) * _length;
		return address >= begin_address && address < end_address;
	}

	T* _data = nullptr;
	size_t _length = 0;
	size_t _capacity = 0;
};
