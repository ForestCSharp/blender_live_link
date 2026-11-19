#pragma once

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/types.h"
#include "core/dynamic_array.h"

// On-disk GI bake file format (no Vulkan here; tests/gi_bake_tests.cpp).
// A header followed by the blobs in GiBakeBlob order, raw and contiguous.
// A file is used only when its header equals the header the current probe
// layout expects, so any layout/format/size change makes it a miss.

enum GiBakeBlob
{
	// All four share the moments slot grid (16^2 entries, slot == probe index).
	// Lighting-derived data (irradiance, specular, SH9/SG9) is not stored:
	// loading a bake relights it.
	GI_BAKE_BLOB_MOMENTS,		// radial-depth moments
	GI_BAKE_BLOB_CACHE_SURFACE,	// geometry cache: albedo, roughness
	GI_BAKE_BLOB_CACHE_NORMAL,	// geometry cache: normal, metallic
	GI_BAKE_BLOB_CACHE_HIT,		// geometry cache: hit distance, emission
	GI_BAKE_BLOB_COUNT,
};

constexpr u32 GI_BAKE_MAGIC = 0x4B414247;	// "GBAK"
constexpr u32 GI_BAKE_VERSION = 4;	// 4: geometry cache only; lighting is relit on load
constexpr size_t GI_BAKE_MAX_KEY_LENGTH = 128;

struct GiBakeHeader
{
	u32 magic = GI_BAKE_MAGIC;
	u32 version = GI_BAKE_VERSION;
	u64 layout_hash[2] = {};
	u64 probe_count = 0;
	u64 blob_bytes[GI_BAKE_BLOB_COUNT] = {};
};
static_assert(sizeof(GiBakeHeader) == 64, "GiBakeHeader must have no padding so memcmp compares it");

inline bool gi_bake_header_equal(const GiBakeHeader& in_a, const GiBakeHeader& in_b)
{
	return memcmp(&in_a, &in_b, sizeof(GiBakeHeader)) == 0;
}

inline u64 gi_bake_total_blob_bytes(const GiBakeHeader& in_header)
{
	u64 total = 0;
	for (u64 bytes : in_header.blob_bytes) total += bytes;
	return total;
}

inline u64 gi_bake_blob_offset(const GiBakeHeader& in_header, const GiBakeBlob in_blob)
{
	u64 offset = 0;
	for (i32 i = 0; i < in_blob; ++i) offset += in_header.blob_bytes[i];
	return offset;
}

// The key arrives over the live-link socket, so it never reaches the path
// unfiltered: anything outside [A-Za-z0-9_-] (including '/' and '.') is '_'.
// Empty key -> empty path (persistence off).
inline std::string gi_bake_path(const std::string& in_scene_key, const char* in_directory = "bin/gi_bakes")
{
	if (in_scene_key.empty()) return {};
	std::string name;
	for (char c : in_scene_key.substr(0, GI_BAKE_MAX_KEY_LENGTH))
	{
		name += (std::isalnum((unsigned char) c) || c == '-' || c == '_') ? c : '_';
	}
	return std::string(in_directory) + "/" + name + ".gibake";
}

// Writes via a sibling .tmp + rename so a crash never leaves a partial bake.
inline bool gi_bake_write(const std::string& in_path, const GiBakeHeader& in_header, const void* in_blobs)
{
	std::error_code error;
	std::filesystem::create_directories(std::filesystem::path(in_path).parent_path(), error);
	const std::string temporary_path = in_path + ".tmp";
	FILE* file = fopen(temporary_path.c_str(), "wb");
	if (!file) return false;
	const u64 blob_bytes = gi_bake_total_blob_bytes(in_header);
	bool ok = fwrite(&in_header, sizeof(in_header), 1, file) == 1
		&& (blob_bytes == 0 || fwrite(in_blobs, blob_bytes, 1, file) == 1);
	ok = (fclose(file) == 0) && ok;
	if (ok)
	{
		std::filesystem::rename(temporary_path, in_path, error);
		ok = !error;
	}
	if (!ok) std::remove(temporary_path.c_str());
	return ok;
}

// Reads the blobs only when the file's header equals in_expected and the file
// holds exactly that many bytes; the header check alone is cheap, which matters
// because layout rebuilds repeat while a scene streams in.
inline bool gi_bake_read(const std::string& in_path, const GiBakeHeader& in_expected, DynamicArray<u8>& out_blobs)
{
	out_blobs.reset();
	FILE* file = fopen(in_path.c_str(), "rb");
	if (!file) return false;
	GiBakeHeader header;
	bool ok = fread(&header, sizeof(header), 1, file) == 1 && gi_bake_header_equal(header, in_expected);
	if (ok)
	{
		const u64 blob_bytes = gi_bake_total_blob_bytes(header);
		out_blobs.add_uninitialized(blob_bytes);
		ok = (blob_bytes == 0 || fread(out_blobs.data(), blob_bytes, 1, file) == 1)
			&& fgetc(file) == EOF;
	}
	fclose(file);
	if (!ok) out_blobs.reset();
	return ok;
}
