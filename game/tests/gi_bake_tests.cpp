#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

#include "render/passes/gi/gi_bake.h"

// Exercises the GI bake file format without Vulkan: the scene key comes off
// the live-link socket, and a stale or truncated bake must never be uploaded.

static void test_path_sanitizes_scene_key()
{
	assert(gi_bake_path("").empty());
	assert(gi_bake_path("level_01/Scene", "d") == "d/level_01_Scene.gibake");
	assert(gi_bake_path("../../etc/passwd", "d") == "d/______etc_passwd.gibake");
	assert(gi_bake_path("a b\\c:d", "d") == "d/a_b_c_d.gibake");
	const std::string long_key(500, 'x');
	assert(gi_bake_path(long_key, "d") == "d/" + std::string(GI_BAKE_MAX_KEY_LENGTH, 'x') + ".gibake");
}

static GiBakeHeader make_header()
{
	GiBakeHeader header;
	header.layout_hash[0] = 0x1234;
	header.layout_hash[1] = 0x5678;
	header.probe_count = 3;
	header.blob_bytes[GI_BAKE_BLOB_MOMENTS] = 8;
	header.blob_bytes[GI_BAKE_BLOB_CACHE_NORMAL] = 4;
	header.blob_bytes[GI_BAKE_BLOB_CACHE_HIT] = 2;
	return header;
}

static void test_round_trip_and_rejections(const std::string& in_directory)
{
	const std::string path = gi_bake_path("round/trip", in_directory.c_str());
	const GiBakeHeader header = make_header();
	assert(gi_bake_total_blob_bytes(header) == 14);
	assert(gi_bake_blob_offset(header, GI_BAKE_BLOB_CACHE_NORMAL) == 8);
	assert(gi_bake_blob_offset(header, GI_BAKE_BLOB_CACHE_HIT) == 12);

	u8 blobs[14];
	for (u8 i = 0; i < sizeof(blobs); ++i) blobs[i] = (u8) (i * 7);
	assert(gi_bake_write(path, header, blobs));
	assert(!std::filesystem::exists(path + ".tmp"));

	DynamicArray<u8> read;
	assert(gi_bake_read(path, header, read));
	assert(read.length() == sizeof(blobs));
	for (u8 i = 0; i < sizeof(blobs); ++i) assert(read[i] == blobs[i]);

	// Any header difference is a miss and leaves nothing behind.
	GiBakeHeader other_layout = header;
	other_layout.layout_hash[1] += 1;
	assert(!gi_bake_read(path, other_layout, read) && read.length() == 0);
	GiBakeHeader other_count = header;
	other_count.probe_count += 1;
	assert(!gi_bake_read(path, other_count, read));
	GiBakeHeader other_size = header;
	other_size.blob_bytes[GI_BAKE_BLOB_CACHE_SURFACE] = 4;
	assert(!gi_bake_read(path, other_size, read));

	// Truncated and over-long files are rejected even with a matching header.
	std::filesystem::resize_file(path, sizeof(GiBakeHeader) + 10);
	assert(!gi_bake_read(path, header, read));
	assert(gi_bake_write(path, header, blobs));
	FILE* file = fopen(path.c_str(), "ab");
	fputc(0, file);
	fclose(file);
	assert(!gi_bake_read(path, header, read));

	assert(!gi_bake_read(in_directory + "/missing.gibake", header, read));
}

int main()
{
	const std::string directory =
		(std::filesystem::temp_directory_path() / "gi_bake_tests").string();
	std::filesystem::remove_all(directory);
	test_path_sanitizes_scene_key();
	test_round_trip_and_rejections(directory);
	std::filesystem::remove_all(directory);
	puts("GI_BAKE_TESTS_PASSED");
	return 0;
}
