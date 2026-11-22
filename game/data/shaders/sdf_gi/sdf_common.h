#ifndef SDF_COMMON_H
#define SDF_COMMON_H

// SDF GI (render/passes/sdf_gi/sdf_gi.h): camera-centred clipmap of
// SDF_LEVEL_COUNT cubes of SDF_LEVEL_RES^3 voxels, level N's voxel twice
// level N-1's. All levels share one volume per quantity, stacked along z
// (level N occupies z in [N*RES, (N+1)*RES)), so one binding serves every level.

#include "shared/octahedral_helpers.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SDF_LEVEL_COUNT 4
#define SDF_LEVEL_RES 128
#define SDF_SKY_RES 32
#define SDF_PROBE_TILE 16
#define SDF_PROBE_RES 8
#define SDF_JFA_INVALID 0xFFFFFFFFu
// SDF reflections trace up to this roughness; rougher keeps the probe SH (faded in above 70%).
#define SDF_REFLECTION_MAX_ROUGHNESS 0.5

// std140; mirrors SdfGi::GpuParams.
struct SdfGiParams
{
	vec4 level_origin_voxel[SDF_LEVEL_COUNT];	// xyz = level min corner, w = voxel size
	mat4 view_projection;
	mat4 prev_view_projection;
	vec4 camera_position;						// w = unused
	vec4 sun_direction;							// xyz = direction TO the sun, w = 1 when a sun exists
	ivec4 counts;								// x = point lights, y = spot lights, z = frame index, w = ready level mask
	ivec4 screen;								// xy = render size, zw = probe grid size
	vec4 settings;								// x = voxel hysteresis, y = probe hysteresis, z = sdf shadows, w = voxel bounce
	ivec4 debug;								// x = ESdfGiDebugView, y = temporal on, z = spatial filter on, w = history valid
	mat4 inv_view_projection;
};

#if defined(SDF_PARAMS_SET)

layout(std140, set = SDF_PARAMS_SET, binding = 0) uniform SdfGiParamsBlock { SdfGiParams sdf; };
layout(set = SDF_PARAMS_SET, binding = 1) uniform sampler3D sdf_distance_tex;
layout(set = SDF_PARAMS_SET, binding = 2) uniform sampler3D sdf_albedo_tex;			// a = occupancy
layout(set = SDF_PARAMS_SET, binding = 3) uniform sampler3D sdf_normal_emission_tex;	// xyz = n*0.5+0.5, a = encoded emission
#if defined(SDF_RADIANCE_STORAGE)
// Voxel lighting reads the previous radiance from the images it is updating
// (in place: a neighbour may already hold this frame's value, which only
// speeds up convergence). a = accumulated sample count.
layout(set = SDF_PARAMS_SET, binding = 4, rgba16f) uniform image3D sdf_radiance_front_image;
layout(set = SDF_PARAMS_SET, binding = 5, rgba16f) uniform image3D sdf_radiance_back_image;
#define SDF_FETCH_RADIANCE_FRONT(texel) imageLoad(sdf_radiance_front_image, texel)
#define SDF_FETCH_RADIANCE_BACK(texel) imageLoad(sdf_radiance_back_image, texel)
#else
layout(set = SDF_PARAMS_SET, binding = 4) uniform sampler3D sdf_radiance_front_tex;	// lit with +n
layout(set = SDF_PARAMS_SET, binding = 5) uniform sampler3D sdf_radiance_back_tex;	// lit with -n
#define SDF_FETCH_RADIANCE_FRONT(texel) texelFetch(sdf_radiance_front_tex, texel, 0)
#define SDF_FETCH_RADIANCE_BACK(texel) texelFetch(sdf_radiance_back_tex, texel, 0)
#endif
layout(set = SDF_PARAMS_SET, binding = 6) uniform sampler2D sdf_sky_tex;				// octahedral sky radiance

float sdf_voxel_size(int level) { return sdf.level_origin_voxel[level].w; }

bool sdf_level_ready(int level) { return (sdf.counts.w & (1 << level)) != 0; }

// Inside the level with a 1-voxel margin, so trilinear taps never leave its slab.
bool sdf_level_contains(int level, vec3 p)
{
	vec3 local = (p - sdf.level_origin_voxel[level].xyz) / sdf_voxel_size(level);
	return sdf_level_ready(level)
		&& all(greaterThanEqual(local, vec3(1.0)))
		&& all(lessThanEqual(local, vec3(float(SDF_LEVEL_RES) - 1.0)));
}

int sdf_finest_level(vec3 p)
{
	for (int level = 0; level < SDF_LEVEL_COUNT; ++level)
	{
		if (sdf_level_contains(level, p)) return level;
	}
	return -1;
}

vec3 sdf_level_uvw(int level, vec3 p)
{
	vec3 local = (p - sdf.level_origin_voxel[level].xyz) / sdf_voxel_size(level);
	return vec3(local.xy / float(SDF_LEVEL_RES),
		(local.z + float(level * SDF_LEVEL_RES)) / float(SDF_LEVEL_RES * SDF_LEVEL_COUNT));
}

float sdf_sample_distance(int level, vec3 p)
{
	return textureLod(sdf_distance_tex, sdf_level_uvw(level, p), 0.0).r;
}

// Ray distance to where it leaves the level's inner (margin) box.
float sdf_level_exit_distance(int level, vec3 p, vec3 rd)
{
	float voxel = sdf_voxel_size(level);
	vec3 box_min = sdf.level_origin_voxel[level].xyz + voxel;
	vec3 box_max = sdf.level_origin_voxel[level].xyz + voxel * float(SDF_LEVEL_RES - 1);
	vec3 inv = 1.0 / max(abs(rd), vec3(1e-6)) * sign(rd + vec3(1e-12));
	vec3 t_far = max((box_min - p) * inv, (box_max - p) * inv);
	return max(min(min(t_far.x, t_far.y), t_far.z), 0.0);
}

struct SdfHit
{
	vec3 position;
	int level;
	int steps;
};

// Sphere-traces the clipmap from the finest level containing ro, moving out a
// level whenever the ray leaves one. Steps are clamped to the level's exit, as
// a level's distances only know about geometry inside it.
bool sdf_trace(vec3 ro, vec3 rd, float t_max, out SdfHit hit)
{
	hit.position = ro;
	hit.level = sdf_finest_level(ro);
	hit.steps = 0;
	if (hit.level < 0) return false;
	float t = 0.0;
	for (int i = 0; i < 64; ++i)
	{
		vec3 p = ro + rd * t;
		while (hit.level < SDF_LEVEL_COUNT && !sdf_level_contains(hit.level, p)) hit.level++;
		if (hit.level >= SDF_LEVEL_COUNT || t > t_max) break;
		hit.steps = i + 1;
		float voxel = sdf_voxel_size(hit.level);
		float d = sdf_sample_distance(hit.level, p);
		if (d < 0.3 * voxel)
		{
			hit.position = p;
			return true;
		}
		float exit_t = sdf_level_exit_distance(hit.level, p, rd);
		t += max(min(d, exit_t + 0.5 * voxel), 0.25 * voxel);
	}
	hit.level = min(hit.level, SDF_LEVEL_COUNT - 1);
	return false;
}

// Soft-ish visibility toward a direction (1 = unoccluded).
float sdf_visibility(vec3 ro, vec3 rd, float t_max)
{
	SdfHit hit;
	return sdf_trace(ro, rd, t_max, hit) ? 0.0 : 1.0;
}

ivec3 sdf_level_texel(int level, ivec3 voxel)
{
	return ivec3(voxel.xy, voxel.z + level * SDF_LEVEL_RES);
}

// Radiance seen at a hit: occupancy-weighted trilinear over the 8 nearest
// voxels (empty voxels hold no light), front or back side by the ray's
// direction against each voxel's stored normal. a = 0 when nothing occupied.
vec4 sdf_sample_hit_radiance(int level, vec3 p, vec3 rd)
{
	vec3 local = (p - sdf.level_origin_voxel[level].xyz) / sdf_voxel_size(level) - 0.5;
	ivec3 base = ivec3(floor(local));
	vec3 f = local - vec3(base);
	vec3 sum = vec3(0.0);
	float weight_sum = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		ivec3 corner = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
		ivec3 voxel = clamp(base + corner, ivec3(0), ivec3(SDF_LEVEL_RES - 1));
		ivec3 texel = sdf_level_texel(level, voxel);
		float occupancy = texelFetch(sdf_albedo_tex, texel, 0).a;
		if (occupancy <= 0.0) continue;
		vec3 w3 = mix(1.0 - f, f, vec3(corner));
		float w = w3.x * w3.y * w3.z * occupancy + 1e-4;
		vec3 n = texelFetch(sdf_normal_emission_tex, texel, 0).xyz * 2.0 - 1.0;
		vec3 radiance = dot(n, rd) <= 0.0
			? SDF_FETCH_RADIANCE_FRONT(texel).rgb
			: SDF_FETCH_RADIANCE_BACK(texel).rgb;
		sum += radiance * w;
		weight_sum += w;
	}
	return weight_sum > 0.0 ? vec4(sum / weight_sum, 1.0) : vec4(0.0);
}

// Albedo at a hit, weighted like sdf_sample_hit_radiance (debug view).
vec3 sdf_sample_hit_albedo(int level, vec3 p)
{
	vec3 local = (p - sdf.level_origin_voxel[level].xyz) / sdf_voxel_size(level) - 0.5;
	ivec3 base = ivec3(floor(local));
	vec3 f = local - vec3(base);
	vec4 sum = vec4(0.0);
	for (int i = 0; i < 8; ++i)
	{
		ivec3 corner = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
		ivec3 voxel = clamp(base + corner, ivec3(0), ivec3(SDF_LEVEL_RES - 1));
		vec4 albedo = texelFetch(sdf_albedo_tex, sdf_level_texel(level, voxel), 0);
		vec3 w3 = mix(1.0 - f, f, vec3(corner));
		sum += vec4(albedo.rgb, 1.0) * (w3.x * w3.y * w3.z * albedo.a);
	}
	return sum.a > 0.0 ? sum.rgb / sum.a : vec3(1.0, 0.0, 1.0);
}

vec3 sdf_sample_sky(vec3 dir)
{
	return textureLod(sdf_sky_tex, octahedral_encode(dir) * 0.5 + 0.5, 0.0).rgb;
}

// Distance gradient: the surface normal at a hit.
vec3 sdf_gradient(int level, vec3 p)
{
	float h = 0.5 * sdf_voxel_size(level);
	vec2 e = vec2(h, 0.0);
	return normalize(vec3(
		sdf_sample_distance(level, p + e.xyy) - sdf_sample_distance(level, p - e.xyy),
		sdf_sample_distance(level, p + e.yxy) - sdf_sample_distance(level, p - e.yxy),
		sdf_sample_distance(level, p + e.yyx) - sdf_sample_distance(level, p - e.yyx)) + vec3(1e-6));
}

#endif // SDF_PARAMS_SET

// Emission strength in [0, ~65000] packed into an 8-bit unorm.
float sdf_encode_emission(float strength) { return clamp(log2(1.0 + strength) / 16.0, 0.0, 1.0); }
float sdf_decode_emission(float encoded) { return exp2(encoded * 16.0) - 1.0; }

uint sdf_jfa_pack(ivec3 v) { return uint(v.x) | (uint(v.y) << 8) | (uint(v.z) << 16); }
ivec3 sdf_jfa_unpack(uint packed) { return ivec3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu); }

// Hash -> [0,1) for per-frame jitter.
float sdf_hash(uvec3 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	return float(v.x & 0x00FFFFFFu) / float(0x01000000u);
}

#endif // SDF_COMMON_H
