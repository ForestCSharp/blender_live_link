// Sokol type defines
@ctype vec2 HMM_Vec2
@ctype vec3 HMM_Vec3
@ctype vec4 HMM_Vec4
@ctype mat4 HMM_Mat4

#define M_PI 3.1415926535897932384626433832795

#define TOKEN_PASTE(x, y) x##y
#define COMBINE(a,b) TOKEN_PASTE(a,b)
#define PADDING(count) float COMBINE(padding_line_, __LINE__)[count]
