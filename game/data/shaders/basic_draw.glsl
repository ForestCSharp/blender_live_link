@ctype mat4 HMM_Mat4

@vs vs

layout(binding=0) uniform vs_params {
    mat4 vp;
};

struct ObjectData
{
	mat4 model_matrix;
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
	world_normal = object_data_array[0].model_matrix * normal;
    gl_Position = vp * world_position;
    color = vec4(1,1,1,1);
}
@end

@fs fs

#define MAX_POINT_LIGHTS 100
#define M_PI 3.1415926535897932384626433832795

struct PointLight {
	vec4 location;
	vec4 color;
	float power;
	float padding[3];
};

struct LightsData {
	int num_point_lights;
	PointLight point_lights[MAX_POINT_LIGHTS];
};

layout(binding=1) readonly buffer LightsDataBuffer {
	LightsData light_data_array[];
};

in vec4 world_position;
in vec4 world_normal;
in vec4 color;

out vec4 frag_color;

float dist_squared( vec3 a, vec3 b)
{
    vec3 c = a - b;
    return dot(c,c);
}

float cosine_angle(vec3 a, vec3 b)
{
	return dot(normalize(a), normalize(b));
}

vec4 sample_point_light(PointLight in_point_light, vec3 in_world_position, vec3 in_normal, vec4 in_color)
{
	float numerator = in_point_light.power * cosine_angle(in_world_position, in_normal);
	float denominator = 4 * M_PI * dist_squared(in_point_light.location.xyz, in_world_position);
	float lighting_factor = numerator / denominator;
	return in_color * in_point_light.color * lighting_factor;
}

void main() {
	LightsData lights_data = light_data_array[0];

	vec4 final_color = vec4(0);
	for (int i = 0; i < lights_data.num_point_lights; ++i)
	{
		PointLight point_light = lights_data.point_lights[i];
		final_color += sample_point_light(point_light, world_position.xyz, world_normal.xyz, color);
	}

	frag_color = final_color;
}
@end

@program cube vs fs
