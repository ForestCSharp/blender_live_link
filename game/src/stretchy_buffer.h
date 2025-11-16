#pragma once

// To Use: 
//#define STB_DS_IMPLEMENTATION
//#include "stretchy_buffer.h"

// Types
#include "types.h"

// STB Dynamic Array
#include "stb/stb_ds.h"

// Basic Template-wrapper around stb_ds array functionality
template<typename T>
struct StretchyBuffer
{	
public:
	StretchyBuffer() : _data(nullptr) {}
	~StretchyBuffer() { reset(); }
	
	// Disable Copying + Assignment
	StretchyBuffer(const StretchyBuffer& other) = delete;
    StretchyBuffer& operator=(const StretchyBuffer& other) = delete;

	void add(const T& value) { arrput(_data, value); }
	void add_uninitialized(const size_t num) { arraddn(_data, num); }
	void reset() { if (_data) { arrfree(_data); _data = nullptr; } }
	size_t length() { return arrlen(_data); }
	T* data() { return _data; }
	T& operator[](i32 idx) { return _data[idx]; }
protected:
	mutable T* _data = nullptr;
};

