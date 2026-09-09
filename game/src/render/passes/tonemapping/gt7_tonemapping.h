#pragma once

// GT7 tone-mapping reference implementation and LUT generator.
//
// This file is derived from the sample implementation published by
// Polyphony Digital Inc. with "Driving Toward Reality: Physically Based Tone
// Mapping". The original sample is Copyright (c) 2025 Polyphony Digital Inc.
// and is used under the MIT License.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "core/dynamic_array.h"

#include <bit>
#include <cmath>

namespace GT7Tonemapping
{
	static constexpr u32 LUT_RESOLUTION = 64;
	static constexpr f32 LUT_INPUT_MAX = 64.0f;
	static constexpr f32 SDR_PAPER_WHITE_NITS = 250.0f;
	static constexpr f32 HDR_PEAK_NITS = 1000.0f;
	static constexpr f32 HDR_PAPER_WHITE_NITS = 203.0f;
	static constexpr f32 REFERENCE_LUMINANCE_NITS = 100.0f;
	static constexpr f32 SDR_INTEGRATION_EXPOSURE_EV = 1.575f;
	static constexpr f32 SDR_INTEGRATION_SCALE = 2.978276f;
	static constexpr f32 HDR_INTEGRATION_SCALE = 11.2777778f;

	struct RGB
	{
		f32 r = 0.0f;
		f32 g = 0.0f;
		f32 b = 0.0f;
	};

	inline RGB operator+(RGB a, RGB b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
	inline RGB operator*(RGB a, f32 scale) { return {a.r * scale, a.g * scale, a.b * scale}; }

	inline f32 smooth_step(f32 x, f32 edge_0, f32 edge_1)
	{
		if (x <= edge_0) return 0.0f;
		if (x >= edge_1) return 1.0f;
		const f32 t = (x - edge_0) / (edge_1 - edge_0);
		return t * t * (3.0f - 2.0f * t);
	}

	inline f32 framebuffer_to_physical(f32 value)
	{
		return value * REFERENCE_LUMINANCE_NITS;
	}

	inline f32 physical_to_framebuffer(f32 value)
	{
		return value / REFERENCE_LUMINANCE_NITS;
	}

	struct Curve
	{
		f32 peak_intensity = 1.0f;
		f32 alpha = 0.25f;
		f32 midpoint = 0.538f;
		f32 linear_section = 0.444f;
		f32 toe_strength = 1.280f;
		f32 shoulder_a = 0.0f;
		f32 shoulder_b = 0.0f;
		f32 shoulder_c = 0.0f;

		void initialize(f32 monitor_intensity)
		{
			peak_intensity = monitor_intensity;
			const f32 k = (linear_section - 1.0f) / (alpha - 1.0f);
			shoulder_a = peak_intensity * linear_section + peak_intensity * k;
			shoulder_b = -peak_intensity * k * std::exp(linear_section / k);
			shoulder_c = -1.0f / (k * peak_intensity);
		}

		f32 evaluate(f32 x) const
		{
			if (x < 0.0f) return 0.0f;
			const f32 linear_weight = smooth_step(x, 0.0f, midpoint);
			const f32 toe_weight = 1.0f - linear_weight;
			const f32 shoulder = shoulder_a + shoulder_b * std::exp(x * shoulder_c);
			if (x < linear_section * peak_intensity)
			{
				const f32 toe = midpoint * std::pow(x / midpoint, toe_strength);
				return toe_weight * toe + linear_weight * x;
			}
			return shoulder;
		}
	};

	inline f32 eotf_st2084(f32 n)
	{
		n = CLAMP(n, 0.0f, 1.0f);
		constexpr f32 m1 = 0.1593017578125f;
		constexpr f32 m2 = 78.84375f;
		constexpr f32 c1 = 0.8359375f;
		constexpr f32 c2 = 18.8515625f;
		constexpr f32 c3 = 18.6875f;
		constexpr f32 pq_peak = 10000.0f;
		const f32 np = std::pow(n, 1.0f / m2);
		const f32 numerator = MAX(np - c1, 0.0f);
		const f32 linear = std::pow(numerator / (c2 - c3 * np), 1.0f / m1);
		return physical_to_framebuffer(linear * pq_peak);
	}

	inline f32 inverse_eotf_st2084(f32 value)
	{
		constexpr f32 m1 = 0.1593017578125f;
		constexpr f32 m2 = 78.84375f;
		constexpr f32 c1 = 0.8359375f;
		constexpr f32 c2 = 18.8515625f;
		constexpr f32 c3 = 18.6875f;
		constexpr f32 pq_peak = 10000.0f;
		const f32 normalized = MAX(framebuffer_to_physical(value) / pq_peak, 0.0f);
		const f32 ym = std::pow(normalized, m1);
		return std::exp2(m2 * (std::log2(c1 + c2 * ym) - std::log2(1.0f + c3 * ym)));
	}

	inline RGB rgb_to_ictcp(RGB rgb)
	{
		const f32 l = (rgb.r * 1688.0f + rgb.g * 2146.0f + rgb.b * 262.0f) / 4096.0f;
		const f32 m = (rgb.r * 683.0f + rgb.g * 2951.0f + rgb.b * 462.0f) / 4096.0f;
		const f32 s = (rgb.r * 99.0f + rgb.g * 309.0f + rgb.b * 3688.0f) / 4096.0f;
		const f32 l_pq = inverse_eotf_st2084(l);
		const f32 m_pq = inverse_eotf_st2084(m);
		const f32 s_pq = inverse_eotf_st2084(s);
		return {
			(2048.0f * l_pq + 2048.0f * m_pq) / 4096.0f,
			(6610.0f * l_pq - 13613.0f * m_pq + 7003.0f * s_pq) / 4096.0f,
			(17933.0f * l_pq - 17390.0f * m_pq - 543.0f * s_pq) / 4096.0f,
		};
	}

	inline RGB ictcp_to_rgb(RGB ictcp)
	{
		const f32 l = ictcp.r + 0.00860904f * ictcp.g + 0.11103f * ictcp.b;
		const f32 m = ictcp.r - 0.00860904f * ictcp.g - 0.11103f * ictcp.b;
		const f32 s = ictcp.r + 0.560031f * ictcp.g - 0.320627f * ictcp.b;
		const f32 l_linear = eotf_st2084(l);
		const f32 m_linear = eotf_st2084(m);
		const f32 s_linear = eotf_st2084(s);
		return {
			MAX(3.43661f * l_linear - 2.50645f * m_linear + 0.0698454f * s_linear, 0.0f),
			MAX(-0.79133f * l_linear + 1.9836f * m_linear - 0.192271f * s_linear, 0.0f),
			MAX(-0.0259499f * l_linear - 0.0989137f * m_linear + 1.12486f * s_linear, 0.0f),
		};
	}

	inline RGB linear_srgb_to_rec2020(RGB rgb)
	{
		return {
			0.6274039f * rgb.r + 0.3292830f * rgb.g + 0.0433131f * rgb.b,
			0.0690973f * rgb.r + 0.9195404f * rgb.g + 0.0113623f * rgb.b,
			0.0163914f * rgb.r + 0.0880133f * rgb.g + 0.8955953f * rgb.b,
		};
	}

	inline RGB rec2020_to_linear_srgb(RGB rgb)
	{
		return {
			1.6604910f * rgb.r - 0.5876411f * rgb.g - 0.0728499f * rgb.b,
			-0.1245505f * rgb.r + 1.1328999f * rgb.g - 0.0083494f * rgb.b,
			-0.0181508f * rgb.r - 0.1005789f * rgb.g + 1.1187297f * rgb.b,
		};
	}

	inline RGB apply_rec2020(RGB rgb, f32 peak_nits)
	{
		const f32 target = physical_to_framebuffer(peak_nits);
		Curve curve;
		curve.initialize(target);
		const RGB original_ucs = rgb_to_ictcp(rgb);
		const RGB skewed = {
			curve.evaluate(rgb.r),
			curve.evaluate(rgb.g),
			curve.evaluate(rgb.b),
		};
		const RGB skewed_ucs = rgb_to_ictcp(skewed);
		const RGB target_ucs = rgb_to_ictcp({target, target, target});
		const f32 chroma_scale = 1.0f - smooth_step(
			original_ucs.r / target_ucs.r, 0.98f, 1.16f);
		const RGB scaled = ictcp_to_rgb({
			skewed_ucs.r,
			original_ucs.g * chroma_scale,
			original_ucs.b * chroma_scale,
		});
		constexpr f32 blend = 0.6f;
		const f32 correction = 1.0f / target;
		return {
			correction * MIN((1.0f - blend) * skewed.r + blend * scaled.r, target),
			correction * MIN((1.0f - blend) * skewed.g + blend * scaled.g, target),
			correction * MIN((1.0f - blend) * skewed.b + blend * scaled.b, target),
		};
	}

	inline RGB apply_sdr_rec2020(RGB rgb)
	{
		return apply_rec2020(rgb, SDR_PAPER_WHITE_NITS);
	}

	inline RGB apply_sdr_linear_srgb(RGB rgb)
	{
		RGB result = rec2020_to_linear_srgb(apply_sdr_rec2020(linear_srgb_to_rec2020(rgb)));
		result.r = CLAMP(result.r, 0.0f, 1.0f);
		result.g = CLAMP(result.g, 0.0f, 1.0f);
		result.b = CLAMP(result.b, 0.0f, 1.0f);
		return result;
	}

	inline RGB apply_hdr_rec2020(RGB rgb)
	{
		return apply_rec2020(rgb, HDR_PEAK_NITS);
	}

	inline RGB apply_hdr_linear_srgb(RGB rgb)
	{
		RGB result = rec2020_to_linear_srgb(apply_hdr_rec2020(linear_srgb_to_rec2020(rgb)));
		result.r = CLAMP(result.r, 0.0f, 1.0f);
		result.g = CLAMP(result.g, 0.0f, 1.0f);
		result.b = CLAMP(result.b, 0.0f, 1.0f);
		return result;
	}

	inline u16 float_to_half(f32 value)
	{
		const u32 bits = std::bit_cast<u32>(value);
		const u32 sign = (bits >> 16) & 0x8000u;
		i32 exponent = (i32)((bits >> 23) & 0xffu) - 127 + 15;
		u32 mantissa = bits & 0x7fffffu;
		if (exponent <= 0)
		{
			if (exponent < -10) return (u16)sign;
			mantissa = (mantissa | 0x800000u) >> (1 - exponent);
			return (u16)(sign | ((mantissa + 0x1000u) >> 13));
		}
		if (exponent >= 31) return (u16)(sign | 0x7c00u);
		mantissa += 0x1000u;
		if (mantissa & 0x800000u)
		{
			mantissa = 0;
			++exponent;
			if (exponent >= 31) return (u16)(sign | 0x7c00u);
		}
		return (u16)(sign | ((u32)exponent << 10) | (mantissa >> 13));
	}

	inline f32 half_to_float(u16 value)
	{
		const u32 sign = ((u32)value & 0x8000u) << 16;
		u32 exponent = ((u32)value >> 10) & 0x1fu;
		u32 mantissa = (u32)value & 0x3ffu;
		u32 bits = 0;
		if (exponent == 0)
		{
			if (mantissa == 0) bits = sign;
			else
			{
				i32 adjusted_exponent = -14;
				while ((mantissa & 0x400u) == 0) { mantissa <<= 1; --adjusted_exponent; }
				mantissa &= 0x3ffu;
				bits = sign | ((u32)(adjusted_exponent + 127) << 23) | (mantissa << 13);
			}
		}
		else if (exponent == 31) bits = sign | 0x7f800000u | (mantissa << 13);
		else bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
		return std::bit_cast<f32>(bits);
	}

	inline DynamicArray<u16> generate_lut(bool in_hdr)
	{
		DynamicArray<u16> pixels;
		pixels.resize((size_t)LUT_RESOLUTION * LUT_RESOLUTION * LUT_RESOLUTION * 4);
		for (u32 b = 0; b < LUT_RESOLUTION; ++b)
		{
			for (u32 g = 0; g < LUT_RESOLUTION; ++g)
			{
				for (u32 r = 0; r < LUT_RESOLUTION; ++r)
				{
					const f32 denominator = (f32)(LUT_RESOLUTION - 1);
					const f32 shaped_r = (f32)r / denominator;
					const f32 shaped_g = (f32)g / denominator;
					const f32 shaped_b = (f32)b / denominator;
					const RGB input = {
						LUT_INPUT_MAX * shaped_r * shaped_r * shaped_r * shaped_r,
						LUT_INPUT_MAX * shaped_g * shaped_g * shaped_g * shaped_g,
						LUT_INPUT_MAX * shaped_b * shaped_b * shaped_b * shaped_b,
					};
					const RGB output = in_hdr
						? apply_hdr_linear_srgb(input)
						: apply_sdr_linear_srgb(input);
					const size_t index = (((size_t)b * LUT_RESOLUTION + g) * LUT_RESOLUTION + r) * 4;
					pixels[index + 0] = float_to_half(output.r);
					pixels[index + 1] = float_to_half(output.g);
					pixels[index + 2] = float_to_half(output.b);
					pixels[index + 3] = float_to_half(1.0f);
				}
			}
		}
		return pixels;
	}

	inline DynamicArray<u16> generate_sdr_lut()
	{
		return generate_lut(false);
	}

	inline DynamicArray<u16> generate_hdr_lut()
	{
		return generate_lut(true);
	}

	inline RGB lut_texel(const DynamicArray<u16>& lut, u32 r, u32 g, u32 b)
	{
		const size_t index = (((size_t)b * LUT_RESOLUTION + g) * LUT_RESOLUTION + r) * 4;
		return {
			half_to_float(lut[index + 0]),
			half_to_float(lut[index + 1]),
			half_to_float(lut[index + 2]),
		};
	}

	inline RGB sample_lut(
		const DynamicArray<u16>& lut,
		RGB input,
		f32 integration_scale,
		bool apply_integration_scale)
	{
		const f32 scale = apply_integration_scale ? integration_scale : 1.0f;
		const RGB calibrated = {
			CLAMP(input.r * scale, 0.0f, LUT_INPUT_MAX),
			CLAMP(input.g * scale, 0.0f, LUT_INPUT_MAX),
			CLAMP(input.b * scale, 0.0f, LUT_INPUT_MAX),
		};
		const RGB position = {
			std::pow(calibrated.r / LUT_INPUT_MAX, 0.25f) * (LUT_RESOLUTION - 1),
			std::pow(calibrated.g / LUT_INPUT_MAX, 0.25f) * (LUT_RESOLUTION - 1),
			std::pow(calibrated.b / LUT_INPUT_MAX, 0.25f) * (LUT_RESOLUTION - 1),
		};
		const u32 r0 = (u32)std::floor(position.r);
		const u32 g0 = (u32)std::floor(position.g);
		const u32 b0 = (u32)std::floor(position.b);
		const u32 r1 = MIN(r0 + 1, LUT_RESOLUTION - 1);
		const u32 g1 = MIN(g0 + 1, LUT_RESOLUTION - 1);
		const u32 b1 = MIN(b0 + 1, LUT_RESOLUTION - 1);
		const f32 fr = position.r - r0;
		const f32 fg = position.g - g0;
		const f32 fb = position.b - b0;
		auto lerp = [](RGB a, RGB b, f32 t) { return a * (1.0f - t) + b * t; };
		const RGB c00 = lerp(lut_texel(lut, r0, g0, b0), lut_texel(lut, r1, g0, b0), fr);
		const RGB c10 = lerp(lut_texel(lut, r0, g1, b0), lut_texel(lut, r1, g1, b0), fr);
		const RGB c01 = lerp(lut_texel(lut, r0, g0, b1), lut_texel(lut, r1, g0, b1), fr);
		const RGB c11 = lerp(lut_texel(lut, r0, g1, b1), lut_texel(lut, r1, g1, b1), fr);
		return lerp(lerp(c00, c10, fg), lerp(c01, c11, fg), fb);
	}

	inline RGB sample_sdr_lut(
		const DynamicArray<u16>& lut,
		RGB input,
		bool apply_integration_scale = true)
	{
		return sample_lut(lut, input, SDR_INTEGRATION_SCALE, apply_integration_scale);
	}

	inline RGB sample_hdr_lut(
		const DynamicArray<u16>& lut,
		RGB input,
		bool apply_integration_scale = true)
	{
		return sample_lut(lut, input, HDR_INTEGRATION_SCALE, apply_integration_scale);
	}
}
