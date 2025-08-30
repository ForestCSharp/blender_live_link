#pragma once

#if !defined(__cplusplus) || !defined(__STDC__)
#define TYPE_DEF(c_type, glsl_type) @ctype glsl_type c_type 
#else
#define TYPE_DEF(c_type, glsl_type) typedef c_type glsl_type;
#endif

TYPE_DEF(HMM_Vec2,vec2)
TYPE_DEF(HMM_Vec3,vec3)
TYPE_DEF(HMM_Vec4,vec4)
TYPE_DEF(HMM_Mat4,mat4)

#if !defined(M_PI)
#define M_PI 3.1415926535897932384626433832795
#endif

#define TOKEN_PASTE(x, y) x##y
#define COMBINE(a,b) TOKEN_PASTE(a,b)

#define _PADDING1 float COMBINE(padding_line_, __LINE__)
#define _PADDING2 vec2 COMBINE(padding_line_, __LINE__)
#define _PADDING3 vec3 COMBINE(padding_line_, __LINE__)

#define PADDING(count) _PADDING ## count


#if !defined(__cplusplus) || !defined(__STDC__)
@block material
#endif

struct Material
{
	vec4 base_color;
	float metallic;
	float roughness;
	PADDING(2);
};

#if !defined(__cplusplus) || !defined(__STDC__)
@end
#endif
