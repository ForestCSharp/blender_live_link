#version 450

// Exponential height fog with optional ceiling + Henyey-Greenstein sun
// in-scatter (port of game/data/shaders/fog.glsl fs). Reads the lit scene
// color + G-buffer world position; writes the fogged scene color.

const float M_PI = 3.14159265358979323846;

layout(set = 0, binding = 0) uniform fs_params
{
	vec3 camera_position;
	float fog_base_height;
	vec3 fog_color;
	float density;
	float scale_height;
	float max_distance;
	int ceiling_enabled;
	float ceiling_height;
	float ceiling_fade;
	float ambient_intensity;
	float sun_intensity;
	float anisotropy;
	vec3 sun_direction;
	vec3 sun_color;
};

layout(set = 0, binding = 1) uniform sampler2D color_tex;
layout(set = 0, binding = 2) uniform sampler2D position_tex;

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 frag_color;

float height_fog_optical_depth(vec3 ray_origin, vec3 ray_dir, float ray_length)
{
	float safe_scale_height = max(scale_height, 0.001);
	float safe_ray_length = max(ray_length, 0.0);
	float base_density = max(density, 0.0) * exp(-(ray_origin.z - fog_base_height) / safe_scale_height);
	float height_slope = ray_dir.z / safe_scale_height;

	if (abs(height_slope) < 0.00001)
	{
		return base_density * safe_ray_length;
	}

	return base_density * (1.0 - exp(-height_slope * safe_ray_length)) / height_slope;
}

float ceiling_density_factor(float height)
{
	if (ceiling_enabled == 0)
	{
		return 1.0;
	}

	float safe_fade = max(ceiling_fade, 0.0);
	if (safe_fade <= 0.0001)
	{
		return height < ceiling_height ? 1.0 : 0.0;
	}

	return 1.0 - smoothstep(ceiling_height - safe_fade, ceiling_height, height);
}

float bounded_height_fog_optical_depth(vec3 ray_origin, vec3 ray_dir, float ray_length)
{
	if (ceiling_enabled == 0)
	{
		return height_fog_optical_depth(ray_origin, ray_dir, ray_length);
	}

	float safe_scale_height = max(scale_height, 0.001);
	float safe_ray_length = max(ray_length, 0.0);
	const int sample_count = 16;
	float step_size = safe_ray_length / float(sample_count);
	float optical_depth = 0.0;

	for (int i = 0; i < sample_count; ++i)
	{
		float ray_time = (float(i) + 0.5) * step_size;
		vec3 sample_position = ray_origin + ray_dir * ray_time;
		float sample_density = max(density, 0.0) * exp(-(sample_position.z - fog_base_height) / safe_scale_height);
		optical_depth += sample_density * ceiling_density_factor(sample_position.z) * step_size;
	}

	return optical_depth;
}

float henyey_greenstein_phase(float cos_theta, float g)
{
	float clamped_g = clamp(g, -0.95, 0.95);
	float g2 = clamped_g * clamped_g;
	float denominator = max(1.0 + g2 - 2.0 * clamped_g * cos_theta, 0.0001);
	return (1.0 - g2) / (4.0 * M_PI * denominator * sqrt(denominator));
}

void main()
{
	vec4 lit_color = texture(color_tex, uv);
	vec4 world_position = texture(position_tex, uv);

	if (world_position.a == 0.0 || density <= 0.0 || max_distance <= 0.0)
	{
		frag_color = lit_color;
		return;
	}

	vec3 camera_to_pixel = world_position.xyz - camera_position;
	float pixel_distance = length(camera_to_pixel);
	if (pixel_distance <= 0.00001)
	{
		frag_color = lit_color;
		return;
	}

	vec3 ray_dir = camera_to_pixel / pixel_distance;
	float fog_distance = min(pixel_distance, max_distance);
	float optical_depth = max(bounded_height_fog_optical_depth(camera_position, ray_dir, fog_distance), 0.0);
	float transmittance = exp(-min(optical_depth, 80.0));
	float fog_amount = 1.0 - transmittance;

	float sun_phase = henyey_greenstein_phase(dot(ray_dir, -normalize(sun_direction)), anisotropy) * 4.0 * M_PI;
	vec3 ambient_inscatter = fog_color * ambient_intensity;
	vec3 sun_inscatter = sun_color * fog_color * sun_intensity * sun_phase;
	vec3 inscatter = ambient_inscatter + sun_inscatter;

	frag_color = vec4(lit_color.rgb * transmittance + inscatter * fog_amount, lit_color.a);
}
