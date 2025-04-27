@ctype mat4 HMM_Mat4

@vs vs

layout(binding=0) uniform vs_params {
    mat4 vp;
};

struct ObjectData
{
	mat4 model_matrix;
	mat4 rotation_matrix;
};

layout(binding=0) readonly buffer ObjectDataBuffer {
	ObjectData object_data_array[];
};

in vec4 position;
in vec4 normal;

out vec4 world_position;
out vec4 world_normal;
out vec4 color;

void main() {
	world_position = object_data_array[0].model_matrix * position;
	world_normal = object_data_array[0].rotation_matrix * normal;
    gl_Position = vp * world_position;
    color = vec4(1,1,1,1);
}
@end

@fs fs

#define M_PI 3.1415926535897932384626433832795

#define TOKEN_PASTE(x, y) x##y
#define COMBINE(a,b) TOKEN_PASTE(a,b)
#define PADDING(count) float COMBINE(padding_line_, __LINE__)[count]

struct PointLight {
	vec4 location;
	vec4 color;

	float power;
	PADDING(3);
};

struct SpotLight {
	vec4 location;
	vec4 color;

	float power;
	float spot_angle_radians;
	float edge_blend;
	PADDING(1);

	vec3 direction;
	PADDING(1);
};

layout(binding=1) uniform fs_params {
    int num_point_lights;
	int num_spot_lights;
};

layout(binding=1) readonly buffer PointLightsBuffer {
	PointLight point_lights[];
};

layout(binding=2) readonly buffer SpotLightsBuffer {
	SpotLight spot_lights[];
};

in vec4 world_position;
in vec4 world_normal;
in vec4 color;

out vec4 frag_color;

float vec2_length_squared(vec2 v)
{
	return dot(v,v);
}

float dist_squared(vec3 a, vec3 b)
{
    vec3 c = a - b;
    return dot(c,c);
}

float dist(vec3 a, vec3 b)
{
	return sqrt(dist_squared(a,b));
}

float cosine_angle(vec3 a, vec3 b)
{
	return dot(normalize(a), normalize(b));
}

float positive_cosine_angle(vec3 a, vec3 b)
{
	return max(0.0, cosine_angle(a,b));
}

float mix_clamped(float a, float b, float alpha)
{
	return mix(a, b, clamp(alpha, 0.0, 1.0));
}

float remap(float value, float old_min, float old_max, float new_min, float new_max)
{
  return new_min + (value - old_min) * (new_max - new_min) / (old_max - old_min);
}

float remap_clamped(float value, float old_min, float old_max, float new_min, float new_max)
{
	float clamped_value = clamp(value, old_min, old_max);
	return remap(clamped_value, old_min, old_max, new_min, new_max);
}

vec4 sample_point_light(PointLight in_point_light, vec3 in_world_position, vec3 in_world_normal, vec4 in_color)
{
	vec3 light_location = in_point_light.location.xyz;
	vec3 world_pos_to_light = normalize(light_location - in_world_position);

	const float surface_angle = positive_cosine_angle(world_pos_to_light, in_world_normal);
	const float numerator = in_point_light.power * surface_angle;
	const float denominator = 4 * M_PI * dist_squared(light_location, in_world_position);
	const float lighting_factor = numerator / denominator;
	return in_color * in_point_light.color * lighting_factor;
}

vec4 sample_spot_light(SpotLight in_spot_light, vec3 in_world_position, vec3 in_world_normal, vec4 in_color)
{
	vec3 spot_light_location = in_spot_light.location.xyz;
	vec3 light_to_world_pos = normalize(in_world_position - spot_light_location);

	float surface_cosine_angle = cosine_angle(light_to_world_pos, in_spot_light.direction);
	float outer_cone_cosine_angle = cos(in_spot_light.spot_angle_radians);

	// Check if we're inside spotlight's outer cone
	if (surface_cosine_angle > outer_cone_cosine_angle)
	{
		const float outer_cone_angle	= in_spot_light.spot_angle_radians;
		const float inner_cone_angle 	= mix_clamped(0.0, outer_cone_angle, 1.0 - in_spot_light.edge_blend);
		const float spot_attenuation 	= in_spot_light.edge_blend > 0.0 
										? 1.0 - remap_clamped(acos(surface_cosine_angle), inner_cone_angle, outer_cone_angle, 0.0, 1.0) 
										: 1.0;

		// Create a point light based on spot-lights location, color, power and sample that
		PointLight point_light;
		point_light.location = in_spot_light.location;
		point_light.color = in_spot_light.color;
		point_light.power = in_spot_light.power;
		return sample_point_light(point_light, in_world_position, in_world_normal, in_color) * spot_attenuation;		
	}

	// Outside spotlight cone, return black
	return vec4(0,0,0,1);
}

vec3 tonemap_filmic(vec3 x)
{
    x = max(vec3(0.0), x - 0.004);
    return (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
}

vec3 tonemap_reinhard(vec3 x)
{
    return x / (x + vec3(1.0));
}

vec3 tonemap_uncharted_2_helper(vec3 x)
{
    const float A = 0.15; const float B = 0.50; const float C = 0.10;
	const float D = 0.20; const float E = 0.02; const float F = 0.30;
    // Apply the tone mapping curve
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 tonemap_uncharted_2(vec3 color) {
    const float W = 11.2; // white point — tweak to taste or keep as 11.2
    return tonemap_uncharted_2_helper(color) / tonemap_uncharted_2_helper(vec3(W));
}

vec3 gamma_correct(vec3 color)
{
    return pow(color, vec3(1.0 / 2.2));
}

void main() {
	vec4 final_color = vec4(0);
	for (int i = 0; i < num_point_lights; ++i)
	{
		final_color += sample_point_light(point_lights[i], world_position.xyz, world_normal.xyz, color);
	}

	for (int i = 0; i < num_spot_lights; ++i)
	{
		final_color += sample_spot_light(spot_lights[i], world_position.xyz, world_normal.xyz, color);
	}

	// Tonemapping
	final_color.xyz = tonemap_reinhard(final_color.xyz);

	frag_color = final_color;
}
@end

@program lighting vs fs
