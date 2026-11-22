#version 450

// GI relight R3: one irradiance atlas texel from the same probe's radiance
// entry. Integrates over the radiance entry's interior texels, weighting each
// by its solid angle (octahedral texels are not equal-area) and the cosine.
//
// Stored value = (1/pi) * integral(L cos) dw, so a white Lambertian surface
// under uniform radiance L reflects exactly L (shading multiplies by albedo
// with no 1/pi). The pre-relighting estimator computed integral(L cos^2) dw,
// about 2x brighter, which would make multi-bounce feedback diverge.

#include "shared/octahedral_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

layout(push_constant) uniform Params
{
	int irradiance_entry_size;
	int radiance_total_size;
	int radiance_entry_size;
	int compute_irradiance;	// 0: radiance along the normal (debug)
};

layout(set = 0, binding = 0) uniform sampler2D radiance_tex;

layout(location = 0) out vec4 out_color;

vec3 radiance_entry_direction(vec2 in_entry_texel_center)
{
	return octahedral_decode(make_padded_atlas_uv(
		in_entry_texel_center / float(radiance_entry_size), float(radiance_entry_size)));
}

void main()
{
	const ivec2 texel = ivec2(gl_FragCoord.xy);
	const ivec2 slot = texel / irradiance_entry_size;
	const vec2 entry_uv = (vec2(texel - slot * irradiance_entry_size) + 0.5) / float(irradiance_entry_size);
	const vec3 normal = octahedral_decode(make_padded_atlas_uv(entry_uv, float(irradiance_entry_size)));

	if (compute_irradiance == 0)
	{
		const int slots_per_dimension = radiance_total_size / radiance_entry_size;
		const vec2 uv = padded_atlas_uv_from_normal(
			normal, slot.y * slots_per_dimension + slot.x, radiance_total_size, radiance_entry_size);
		out_color = vec4(texture(radiance_tex, uv).rgb, 1.0);
		return;
	}

	const int gutter = int(ATLAS_GUTTER_SIZE);
	const ivec2 base = slot * radiance_entry_size;
	vec3 sum = vec3(0.0);
	for (int y = gutter; y < radiance_entry_size - gutter; ++y)
	{
		for (int x = gutter; x < radiance_entry_size - gutter; ++x)
		{
			const vec2 center = vec2(x, y) + 0.5;
			const vec3 direction = radiance_entry_direction(center);
			const float cosine = dot(normal, direction);
			if (cosine <= 0.0)
			{
				continue;
			}
			// Solid angle from the texel's footprint on the unit sphere.
			const vec3 du = radiance_entry_direction(center + vec2(0.5, 0.0)) - radiance_entry_direction(center - vec2(0.5, 0.0));
			const vec3 dv = radiance_entry_direction(center + vec2(0.0, 0.5)) - radiance_entry_direction(center - vec2(0.0, 0.5));
			const float solid_angle = length(cross(du, dv));
			sum += texelFetch(radiance_tex, base + ivec2(x, y), 0).rgb * cosine * solid_angle;
		}
	}
	out_color = vec4(sum / M_PI, 1.0);
}
