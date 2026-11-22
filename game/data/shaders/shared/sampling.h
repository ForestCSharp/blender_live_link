#ifndef SAMPLING_H
#define SAMPLING_H

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

float sampling_radical_inverse_vdc(uint bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

vec2 sampling_hammersley(uint index, uint count)
{
	return vec2(float(index) / float(count), sampling_radical_inverse_vdc(index));
}

void sampling_build_orthonormal_basis(vec3 normal, out vec3 tangent, out vec3 bitangent)
{
	vec3 up = abs(normal.z) < 0.999
		? vec3(0.0, 0.0, 1.0)
		: vec3(1.0, 0.0, 0.0);
	tangent = normalize(cross(up, normal));
	bitangent = cross(normal, tangent);
}

vec3 sampling_importance_sample_ggx(
	vec2 xi,
	vec3 normal,
	float perceptual_roughness)
{
	float alpha = perceptual_roughness * perceptual_roughness;
	float alpha_squared = alpha * alpha;
	float phi = 2.0 * M_PI * xi.x;
	float cos_theta = sqrt((1.0 - xi.y)
		/ max(1.0 + (alpha_squared - 1.0) * xi.y, 0.00001));
	float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
	vec3 half_vector = vec3(
		cos(phi) * sin_theta,
		sin(phi) * sin_theta,
		cos_theta);

	vec3 tangent;
	vec3 bitangent;
	sampling_build_orthonormal_basis(normal, tangent, bitangent);
	return normalize(
		tangent * half_vector.x
		+ bitangent * half_vector.y
		+ normal * half_vector.z);
}

vec3 sampling_cosine_hemisphere(vec2 xi, vec3 normal)
{
	float phi = 2.0 * M_PI * xi.x;
	float cos_theta = sqrt(1.0 - xi.y);
	float sin_theta = sqrt(xi.y);
	vec3 local_direction = vec3(
		cos(phi) * sin_theta,
		sin(phi) * sin_theta,
		cos_theta);

	vec3 tangent;
	vec3 bitangent;
	sampling_build_orthonormal_basis(normal, tangent, bitangent);
	return normalize(
		tangent * local_direction.x
		+ bitangent * local_direction.y
		+ normal * local_direction.z);
}

vec3 sampling_uniform_sphere(vec2 xi)
{
	float z = 1.0 - 2.0 * xi.x;
	float radius = sqrt(max(0.0, 1.0 - z * z));
	float phi = 2.0 * M_PI * xi.y;
	return vec3(radius * cos(phi), radius * sin(phi), z);
}

#endif // SAMPLING_H
