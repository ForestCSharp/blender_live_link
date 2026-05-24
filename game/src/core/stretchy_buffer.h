#pragma once

// To Use: 
//#define STB_DS_IMPLEMENTATION
//#include "stretchy_buffer.h"

// Types
#include "core/types.h"

// STB Dynamic Array
#include "stb/stb_ds.h"

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

	void add(const T& value) { arrput(_data, value); }
	void add_uninitialized(const size_t num) { arraddn(_data, num); }
	void reset() { if (_data) { arrfree(_data); _data = nullptr; } }
	size_t length() const { return arrlen(_data); }
	T* data() { return _data; }
	const T* data() const { return _data; }
	T& operator[](i32 idx) { return _data[idx]; }
	bool is_valid_index(i32 idx) const { return idx < length(); }

	// Range-for support
    T* begin() { return _data; }
    T* end()   { return _data + arrlen(_data); }

    // Const support (for const StretchyBuffer& usage)
    const T* begin() const { return _data; }
    const T* end()   const { return _data + arrlen(_data); }

protected:
	mutable T* _data = nullptr;
};
