#ifndef BRDF_H
#define BRDF_H

// Cook-Torrance BRDF (port of game/data/shaders/brdf.h, sokol blocks removed)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

float distribution_ggx(const float n_dot_h, const float roughness)
{
	const float a = roughness * roughness;
	const float a2 = a * a;
	const float n_dot_h_squared = n_dot_h * n_dot_h;

	const float nom = a2;
	float denom = (n_dot_h_squared * (a2 - 1.0) + 1.0);
	denom = M_PI * denom * denom;

	return nom / max(denom, 0.0000001); // prevent divide by zero for roughness=0.0 and NdotH=1.0
}

float geometry_schlick_ggx(const float n_dot_v, const float roughness)
{
	float a = roughness;
	float k = (a * a) / 2.0;

	float nom = n_dot_v;
	float denom = n_dot_v * (1.0 - k) + k;

	return nom / denom;
}

float geometry_smith(const float n_dot_v, const float n_dot_l, const float roughness)
{
	const float ggx2 = geometry_schlick_ggx(n_dot_v, roughness);
	const float ggx1 = geometry_schlick_ggx(n_dot_l, roughness);
	return ggx1 * ggx2;
}

vec3 fresnel_schlick(const float cos_theta, const vec3 f0)
{
	return f0 + (1.0 - f0) * pow(max(1.0 - cos_theta, 0.0), 5.0);
}

vec3 fresnel_schlick_roughness(float cosTheta, vec3 F0, float roughness)
{
	return F0 + (max(vec3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

#define MIN_N_DOT_V 1e-4

float clamp_n_dot_v(const float n_dot_v)
{
	// Neubelt and Pettineo 2013, "Crafting a Next-gen Material Pipeline for The Order: 1886"
	return max(n_dot_v, MIN_N_DOT_V);
}

float saturate(float value)
{
	return clamp(value, 0.0, 1.0);
}

// Main Cook-Torrance BRDF Function
//  n	Surface normal unit vector
//  l	Incident light unit vector (from surface to light)
//  v	View unit vector (from surface to eye)
vec3 cook_torrance_brdf(
	const vec3 n,
	const vec3 l,
	const vec3 v,
	const vec3 albedo,
	const vec3 f0,
	const float roughness,
	const float metallic,
	const vec3 radiance
)
{
	// Compute half-vector (between view and incident light)
	const vec3 h = normalize(v + l);

	// Dot products
	const float n_dot_v = clamp_n_dot_v(saturate(dot(n, v)));
	const float n_dot_l = saturate(dot(n, l));
	const float n_dot_h = saturate(dot(n, h));
	const float h_dot_v = saturate(dot(h, v));

	if (n_dot_l <= 0.0)
	{
		return vec3(0.0, 0.0, 0.0);
	}

	// Specular
	const float ndf = distribution_ggx(n_dot_h, roughness);
	const float g = geometry_smith(n_dot_v, n_dot_l, roughness);
	const vec3 f = fresnel_schlick(h_dot_v, f0);

	const vec3 numerator = ndf * g * f;
	const float denominator = 4 * n_dot_v * n_dot_l;
	const vec3 specular = numerator / max(denominator, 0.001);

	// kS is equal to Fresnel
	const vec3 ks = f;

	// Diffuse: energy conservation, metals have no diffuse
	vec3 kd = vec3(1, 1, 1) - ks;
	kd *= 1.0 - metallic;

	const vec3 diffuse = kd * albedo / M_PI;

	return (diffuse + specular) * radiance * n_dot_l;
}

#endif // BRDF_H
