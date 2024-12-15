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
out vec4 color;

void main() {
	world_position = object_data_array[0].model_matrix * position;
    gl_Position = vp * world_position;
    color = vec4(1,1,1,1);
}
@end

@fs fs

#define MAX_POINT_LIGHTS 100

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
in vec4 color;

out vec4 frag_color;

float dist_squared( vec3 a, vec3 b)
{
    vec3 c = a - b;
    return dot(c,c);
}

void main() {
	LightsData lights_data = light_data_array[0];

	vec4 final_color = vec4(0);
	for (int i = 0; i < lights_data.num_point_lights; ++i)
	{
		PointLight point_light = lights_data.point_lights[i];
		float lighting_factor = point_light.power / dist_squared(point_light.location.xyz, world_position.xyz);
		final_color += (color * point_light.color * lighting_factor);
	}

	frag_color = final_color;
}
@end

@program cube vs fs
