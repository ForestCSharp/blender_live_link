#pragma once

// Whether or not to enable inverse depth
#define USE_INVERSE_DEPTH 1 
#define MAX_DEPTH (USE_INVERSE_DEPTH != 0 ? 0.0 : 1.0)
#define MIN_DEPTH (USE_INVERSE_DEPTH == 0 ? 1.0 : 0.0)

#if !defined(__cplusplus) || !defined(__STDC__)
#define TYPE_DEF(c_type, glsl_type) @ctype glsl_type c_type 
#else
#define TYPE_DEF(c_type, glsl_type) typedef c_type glsl_type;
#endif

TYPE_DEF(HMM_Vec2,vec2)
TYPE_DEF(HMM_Vec3,vec3)
TYPE_DEF(HMM_Vec4,vec4)
TYPE_DEF(HMM_Mat4,mat4)

#if !defined(__cplusplus) || !defined(__STDC__)
#define HMM_V2 vec2
#define HMM_V3 vec3
#define HMM_V4 vec4
#endif

#if !defined(M_PI)
#define M_PI 3.1415926535897932384626433832795
#endif

// Commenting out. sokol_shdc seems to handle padding for us now
//#define TOKEN_PASTE(x, y) x##y
//#define COMBINE(a,b) TOKEN_PASTE(a,b)
//#define _PADDING1 float COMBINE(padding_line_, __LINE__)
//#define _PADDING2 vec2 COMBINE(padding_line_, __LINE__)
//#define _PADDING3 vec3 COMBINE(padding_line_, __LINE__)
//#define _PADDING4 vec4 COMBINE(padding_line_, __LINE__)
//#define PADDING(count) _PADDING ## count

#if !defined(__cplusplus) || !defined(__STDC__)
@block material
#endif

struct Material
{
	vec4 base_color;
	vec4 emission_color;
	float metallic;
	float roughness;
	float emission_strength;
	int base_color_image_index;
	int emission_color_image_index;
	int metallic_image_index;
	int roughness_image_index;
};

#if !defined(__cplusplus) || !defined(__STDC__)
@end // @block material
#endif


#if !defined(__cplusplus) || !defined(__STDC__)
@block dither_noise 

/* Gradient noise from Jorge Jimenez's presentation: */
/* http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare */
float gradient_noise(in vec2 uv)
{
	return fract(52.9829189 * fract(dot(uv, vec2(0.06711056, 0.00583715))));
}

vec4 dither_noise()
{
	return vec4(vec3((1.0 / 255.0) * gradient_noise(gl_FragCoord.xy) - (0.5 / 255.0)), 0.0);
}

@end // @block dither_noise
#endif //!defined(__cplusplus) || !defined(__STDC__)

#if !defined(__cplusplus) || !defined(__STDC__)
@block remap 
float remap(float value, float old_min, float old_max, float new_min, float new_max)
{
  return new_min + (value - old_min) * (new_max - new_min) / (old_max - old_min);
}

float remap_clamped(float value, float old_min, float old_max, float new_min, float new_max)
{
	float clamped_value = clamp(value, old_min, old_max);
	return remap(clamped_value, old_min, old_max, new_min, new_max);
}
@end 
#endif //!defined(__cplusplus) || !defined(__STDC__)
