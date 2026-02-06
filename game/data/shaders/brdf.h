#pragma once

#include "shader_common.h"

#if !defined(__cplusplus) || !defined(__STDC__)
// Begin Sokol ShaderC Block
@block brdf
#endif

float distribution_ggx(const float n_dot_h, const float roughness)
{
    const float a = roughness*roughness;
    const float a2 = a*a;
    const float n_dot_h_squared = n_dot_h*n_dot_h;

    const float nom   = a2;
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

// Diffuse BRDF
#define MIN_N_DOT_V 1e-4

float clamp_n_dot_v(const float n_dot_v) {
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
    //Compute half-vector (between view and incident light)
    const vec3 h = normalize(v + l);

    //Dot products
    const float n_dot_v = saturate(dot(n,v));   
    const float n_dot_l = saturate(dot(n,l));
    const float n_dot_h = saturate(dot(n,h));
    const float h_dot_v = saturate(dot(h,v));

    // Specular --------------------------------------------------------/

    const float	ndf	= distribution_ggx(n_dot_h, roughness);   
    const float	g	= geometry_smith(n_dot_v, n_dot_l, roughness); 
    const vec3	f	= fresnel_schlick(h_dot_v, f0);
           
    const vec3 numerator  = ndf * g * f; 
    const float denominator = 4 * n_dot_v * n_dot_l;

    // prevent divide by zero for NdotV=0.0 or NdotL=0.0
    const vec3 specular = numerator / max(denominator, 0.001);

    // kS is equal to Fresnel
    const vec3 ks = f;

    // Diffuse ---------------------------------------------------------/
    
    // for energy conservation, the diffuse and specular light can't
    // be above 1.0 (unless the surface emits light); to preserve this
    // relationship the diffuse component (kD) should equal 1.0 - kS.
    vec3 kd = vec3(1,1,1) - ks;
    // multiply kD by the inverse metalness such that only non-metals 
    // have diffuse lighting, or a linear blend if partly metal (pure metals
    // have no diffuse light).
    kd *= 1.0 - metallic;

    const vec3 diffuse = kd * albedo / M_PI;

    const vec3 out_color = (diffuse + specular) * radiance * n_dot_l;
    
    return out_color;
}

// ----------------------------------------------------------------------------
// http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
// efficient VanDerCorpus calculation.
float radical_inverse_vdc(uint bits) 
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}
// ----------------------------------------------------------------------------
vec2 hammersley_sequence(uint i, uint N)
{
    return vec2(float(i)/float(N), radical_inverse_vdc(i));
}

vec3 importance_sample_ggx(vec2 Xi, vec3 N, float roughness)
{
    float a = roughness * roughness;
	
    float phi = 2.0 * M_PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta*cosTheta);
	
    // from spherical coordinates to cartesian coordinates - halfway vector
    vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;
	
    // from tangent-space H vector to world-space sample vector
    vec3 up          = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent   = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
	
    vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}

// Approximation of the BRDF Integration Map (So you don't need a LUT texture)
// Source: Karis 2014, "Real Shading in Unreal Engine 4"
vec2 env_brdf_approx(float roughness, float NoV)
{
    const vec4 c0 = vec4(-1, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return AB;
}

#if !defined(__cplusplus) || !defined(__STDC__)
// End Sokol ShaderC Block
@end // @block brdf
#endif
