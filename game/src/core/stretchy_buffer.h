#pragma once

// Types
#include "core/types.h"

// STB Dynamic Array
#include "stb/stb_ds.h"

#include <new>
#include <type_traits>
#include <utility>

// Basic Template-wrapper around stb_ds array functionality
template<typename T>
struct StretchyBuffer
{	
public:
	StretchyBuffer() : _data(nullptr) {}
	~StretchyBuffer() { reset(); }
	
	StretchyBuffer(const StretchyBuffer& other)
	{
		*this = other;
	};

	StretchyBuffer(StretchyBuffer&& other) noexcept
	{
		_data = other._data;
		other._data = nullptr;
	}

    StretchyBuffer& operator=(const StretchyBuffer& other) 
	{
		if (this == &other)
		{
			return *this;
		}

		reset();
		for (const T& value : other)
		{
			add(value);
		}

		return *this;
	};

	StretchyBuffer& operator=(StretchyBuffer&& other) noexcept
	{
		if (this == &other)
		{
			return *this;
		}

		reset();
		_data = other._data;
		other._data = nullptr;
		return *this;
	}

	void add(const T& value)
	{
		arraddn(_data, 1);
		new (&_data[arrlen(_data) - 1]) T(value);
	}

	void add(T&& value)
	{
		arraddn(_data, 1);
		new (&_data[arrlen(_data) - 1]) T(std::move(value));
	}

	template<typename... Args>
	T& emplace(Args&&... args)
	{
		arraddn(_data, 1);
		T* value = &_data[arrlen(_data) - 1];
		new (value) T(std::forward<Args>(args)...);
		return *value;
	}

	void add_uninitialized(const size_t num) { arraddn(_data, num); }
	void pop()
	{
		if (_data && arrlen(_data) > 0)
		{
			_data[arrlen(_data) - 1].~T();
			arrpop(_data);
		}
	}
	void reset()
	{
		if (_data)
		{
			if constexpr (!std::is_trivially_destructible_v<T>)
			{
				for (size_t i = 0; i < arrlen(_data); ++i)
				{
					_data[i].~T();
				}
			}
			arrfree(_data);
			_data = nullptr;
		}
	}
	size_t length() const { return arrlen(_data); }
	T* data() { return _data; }
	const T* data() const { return _data; }
	T& operator[](i32 idx) { return _data[idx]; }
	const T& operator[](i32 idx) const { return _data[idx]; }
	T& last() { return _data[arrlen(_data) - 1]; }
	const T& last() const { return _data[arrlen(_data) - 1]; }
	bool is_valid_index(i32 idx) const { return idx >= 0 && (size_t)idx < length(); }

	// Range-for support
    T* begin() { return _data; }
    T* end()   { return _data + arrlen(_data); }

    // Const support (for const StretchyBuffer& usage)
    const T* begin() const { return _data; }
    const T* end()   const { return _data + arrlen(_data); }

protected:
	mutable T* _data = nullptr;
};
