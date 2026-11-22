#version 450

// Writes radial-distance moments for probe occlusion from the capture
// G-buffer's world positions (Chebyshev uses 2 moments and EVRP4 uses 4
// warped moments), plus the probe's packed geometry cache for relighting:
// what the probe sees in each direction, independent of lighting.

layout(push_constant) uniform fs_params
{
	mat4 inverse_view_projection;
	vec3 capture_location;
	int probe_occlusion_mode;
	int force_fully_visible;
	float max_radial_depth;
};

layout(set = 0, binding = 0) uniform sampler2D world_position_tex;
layout(set = 0, binding = 1) uniform sampler2D color_tex;
layout(set = 0, binding = 2) uniform sampler2D normal_tex;
layout(set = 0, binding = 3) uniform sampler2D roughness_metallic_emissive_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 out_cache_surface;	// albedo.rgb, roughness
layout(location = 2) out vec4 out_cache_normal;		// normal * 0.5 + 0.5, metallic
layout(location = 3) out vec4 out_cache_hit;		// hit distance (0 = sky), emission strength

const int PROBE_OCCLUSION_MODE_CHEBYSHEV = 0;
const int PROBE_OCCLUSION_MODE_EVRP4 = 1;

const float EVRP_POSITIVE_EXPONENT = 5.0;
const float EVRP_NEGATIVE_EXPONENT = 5.0;
const float EVRP_MOMENT_BIAS = 0.00001;

float sample_radial_depth(vec2 sample_uv)
{
	const vec4 world_position = texture(world_position_tex, sample_uv);
	return world_position.w > 0.0
		? min(length(world_position.xyz - capture_location), max_radial_depth)
		: max_radial_depth;
}

void main()
{
	// Variance Shadow Map second-moment stabilization term from GPU Gems:
	// m2 += 0.25 * (ddx(d)^2 + ddy(d)^2)
	const float center_depth = force_fully_visible != 0
		? max_radial_depth
		: sample_radial_depth(uv);
	if (probe_occlusion_mode == PROBE_OCCLUSION_MODE_EVRP4)
	{
		const float normalized_depth = clamp(center_depth / max_radial_depth, 0.0, 1.0);
		const vec2 warped_depth = vec2(
			exp(EVRP_POSITIVE_EXPONENT * normalized_depth),
			-exp(-EVRP_NEGATIVE_EXPONENT * normalized_depth)
		);

		const vec2 dx = dFdx(warped_depth);
		const vec2 dy = dFdy(warped_depth);
		const vec2 derivative_bias = 0.25 * ((dx * dx) + (dy * dy));
		const vec2 second_moments = warped_depth * warped_depth + derivative_bias + vec2(EVRP_MOMENT_BIAS);

		frag_color = vec4(warped_depth.x, second_moments.x, warped_depth.y, second_moments.y);
	}
	else
	{
		const float depth_dx = dFdx(center_depth);
		const float depth_dy = dFdy(center_depth);
		const float derivative_bias = 0.25 * ((depth_dx * depth_dx) + (depth_dy * depth_dy));

		frag_color = vec4(center_depth, (center_depth * center_depth) + derivative_bias, 0.0, 0.0);
	}

	// Geometry cache. The capture draws are 1:1 with the G-buffer, so these
	// reads are exact texels; sky (position.w == 0) stores distance 0.
	const vec4 world_position = texture(world_position_tex, uv);
	const vec4 rme = texture(roughness_metallic_emissive_tex, uv);
	const bool is_geometry = world_position.w > 0.0;
	out_cache_surface = is_geometry ? vec4(clamp(texture(color_tex, uv).rgb, 0.0, 1.0), rme.r) : vec4(0.0);
	out_cache_normal = is_geometry ? vec4(normalize(texture(normal_tex, uv).xyz) * 0.5 + 0.5, rme.g) : vec4(0.0);
	out_cache_hit = is_geometry ? vec4(length(world_position.xyz - capture_location), rme.b, 0.0, 0.0) : vec4(0.0);
}
