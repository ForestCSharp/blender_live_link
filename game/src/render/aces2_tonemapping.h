#pragma once

// ACES 2.0 LUT asset loading and CPU reference sampling.
//
// Assets are generated from ACES v2.0.0+2025.04.04 with OpenColorIO 2.5.0.
// See LICENSES/ACES-v2.0.0-License.txt, LICENSES/ACES-Apache-2.0.txt, and
// LICENSES/OpenColorIO-BSD-3-Clause.txt for upstream attribution and terms.

#include "core/dynamic_array.h"
#include "render/gt7_tonemapping.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace ACES2Tonemapping
{
	static constexpr u32 LUT_RESOLUTION = 64;
	static constexpr f32 LUT_INPUT_MAX = 64.0f;
	static constexpr u32 LUT_LAYER_OFFSET = 64;
	static constexpr u32 COMBINED_LUT_LAYER_COUNT = 128;
	static constexpr f32 SDR_INTEGRATION_SCALE = 2.0548065f;
	static constexpr f32 HDR_INTEGRATION_SCALE = 10.9398375f;

	static constexpr u32 LUT_FILE_VERSION = 1;
	static constexpr u32 LUT_SHAPER_ACESCCT = 1;
	static constexpr u32 LUT_PIXEL_FORMAT_RGBA16F = 1;
	static constexpr size_t LUT_HEADER_SIZE = 40;
	static constexpr size_t LUT_TEXEL_COUNT =
		(size_t)LUT_RESOLUTION * LUT_RESOLUTION * LUT_RESOLUTION;
	static constexpr size_t LUT_PAYLOAD_SIZE = LUT_TEXEL_COUNT * 4 * sizeof(u16);
	static constexpr u8 LUT_MAGIC[8] = {'G', '2', 'A', '2', 'L', 'U', 'T', 0};
	static_assert(std::endian::native == std::endian::little,
		"ACES 2 LUT payloads are serialized as little-endian RGBA16F");

	struct LoadedLUT
	{
		DynamicArray<u16> pixels;
		u32 target = 0;
		u32 crc32 = 0;
	};

	inline u32 read_u32_le(const u8* bytes)
	{
		return (u32)bytes[0]
			| ((u32)bytes[1] << 8)
			| ((u32)bytes[2] << 16)
			| ((u32)bytes[3] << 24);
	}

	inline u32 crc32(const u8* data, size_t size)
	{
		u32 crc = 0xffffffffu;
		for (size_t index = 0; index < size; ++index)
		{
			crc ^= data[index];
			for (u32 bit = 0; bit < 8; ++bit)
				crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
		return crc ^ 0xffffffffu;
	}

	inline const char* target_name(i32 output_mode)
	{
		switch (output_mode)
		{
			case 0: return "sdr";
			case 1: return "edr";
			case 2: return "hdr10";
			default: return nullptr;
		}
	}

	inline std::string asset_path(i32 output_mode)
	{
		const char* name = target_name(output_mode);
		return name ? std::string("data/tonemapping/aces2_") + name + ".lutbin" : std::string();
	}

	inline bool load_lut(i32 output_mode, LoadedLUT* out_lut, std::string* out_error)
	{
		const std::string path = asset_path(output_mode);
		if (path.empty())
		{
			if (out_error) *out_error = "unsupported display output target";
			return false;
		}

		FILE* file = fopen(path.c_str(), "rb");
		if (!file)
		{
			if (out_error) *out_error = "could not open " + path
				+ " (run from the game directory or regenerate with python3 tools/generate_aces2_luts.py)";
			return false;
		}
		fseek(file, 0, SEEK_END);
		const long file_size = ftell(file);
		rewind(file);
		if (file_size != (long)(LUT_HEADER_SIZE + LUT_PAYLOAD_SIZE))
		{
			fclose(file);
			if (out_error) *out_error = path
				+ " has an invalid byte size; regenerate with python3 tools/generate_aces2_luts.py";
			return false;
		}

		u8 header[LUT_HEADER_SIZE] = {};
		if (fread(header, 1, sizeof(header), file) != sizeof(header))
		{
			fclose(file);
			if (out_error) *out_error = "could not read " + path + " header";
			return false;
		}
		const u32 version = read_u32_le(header + 8);
		const u32 resolution = read_u32_le(header + 12);
		const u32 target = read_u32_le(header + 16);
		const u32 shaper = read_u32_le(header + 20);
		const u32 pixel_format = read_u32_le(header + 24);
		const u32 payload_size = read_u32_le(header + 28);
		const u32 expected_crc = read_u32_le(header + 32);
		const u32 reserved = read_u32_le(header + 36);
		if (memcmp(header, LUT_MAGIC, sizeof(LUT_MAGIC)) != 0
			|| version != LUT_FILE_VERSION
			|| resolution != LUT_RESOLUTION
			|| target != (u32)output_mode
			|| shaper != LUT_SHAPER_ACESCCT
			|| pixel_format != LUT_PIXEL_FORMAT_RGBA16F
			|| payload_size != LUT_PAYLOAD_SIZE
			|| reserved != 0)
		{
			fclose(file);
			if (out_error) *out_error = path
				+ " has an incompatible header; regenerate with python3 tools/generate_aces2_luts.py";
			return false;
		}

		out_lut->pixels.resize(LUT_PAYLOAD_SIZE / sizeof(u16));
		if (fread(out_lut->pixels.data(), 1, LUT_PAYLOAD_SIZE, file) != LUT_PAYLOAD_SIZE)
		{
			fclose(file);
			if (out_error) *out_error = "could not read " + path + " payload";
			return false;
		}
		fclose(file);
		const u32 actual_crc = crc32((const u8*)out_lut->pixels.data(), LUT_PAYLOAD_SIZE);
		if (actual_crc != expected_crc)
		{
			if (out_error) *out_error = path
				+ " failed CRC32 validation; regenerate with python3 tools/generate_aces2_luts.py";
			return false;
		}
		out_lut->target = target;
		out_lut->crc32 = actual_crc;
		return true;
	}

	inline f32 linear_to_acescct(f32 value)
	{
		return value <= 0.0078125f
			? 10.5402377416545f * value + 0.0729055341958355f
			: (std::log2(value) + 9.72f) / 17.52f;
	}

	inline GT7Tonemapping::RGB lut_texel(
		const DynamicArray<u16>& lut, u32 r, u32 g, u32 b)
	{
		const size_t index = (((size_t)b * LUT_RESOLUTION + g) * LUT_RESOLUTION + r) * 4;
		return {
			GT7Tonemapping::half_to_float(lut[index + 0]),
			GT7Tonemapping::half_to_float(lut[index + 1]),
			GT7Tonemapping::half_to_float(lut[index + 2]),
		};
	}

	inline GT7Tonemapping::RGB sample_lut(
		const DynamicArray<u16>& lut,
		GT7Tonemapping::RGB input,
		f32 integration_scale,
		bool apply_integration_scale = true)
	{
		const f32 scale = apply_integration_scale ? integration_scale : 1.0f;
		const f32 shaper_min = linear_to_acescct(0.0f);
		const f32 shaper_range = linear_to_acescct(LUT_INPUT_MAX) - shaper_min;
		const auto position = [&](f32 value)
		{
			const f32 calibrated = CLAMP(value * scale, 0.0f, LUT_INPUT_MAX);
			return (linear_to_acescct(calibrated) - shaper_min) / shaper_range
				* (LUT_RESOLUTION - 1);
		};
		const f32 pr = position(input.r);
		const f32 pg = position(input.g);
		const f32 pb = position(input.b);
		const u32 r0 = (u32)std::floor(pr), g0 = (u32)std::floor(pg), b0 = (u32)std::floor(pb);
		const u32 r1 = MIN(r0 + 1, LUT_RESOLUTION - 1);
		const u32 g1 = MIN(g0 + 1, LUT_RESOLUTION - 1);
		const u32 b1 = MIN(b0 + 1, LUT_RESOLUTION - 1);
		const auto lerp = [](GT7Tonemapping::RGB a, GT7Tonemapping::RGB b, f32 t)
		{
			return a * (1.0f - t) + b * t;
		};
		const GT7Tonemapping::RGB c00 = lerp(lut_texel(lut, r0, g0, b0), lut_texel(lut, r1, g0, b0), pr - r0);
		const GT7Tonemapping::RGB c10 = lerp(lut_texel(lut, r0, g1, b0), lut_texel(lut, r1, g1, b0), pr - r0);
		const GT7Tonemapping::RGB c01 = lerp(lut_texel(lut, r0, g0, b1), lut_texel(lut, r1, g0, b1), pr - r0);
		const GT7Tonemapping::RGB c11 = lerp(lut_texel(lut, r0, g1, b1), lut_texel(lut, r1, g1, b1), pr - r0);
		return lerp(
			lerp(c00, c10, pg - g0),
			lerp(c01, c11, pg - g0),
			pb - b0);
	}
}
