// -*- c++ -*-

/** Efficient GPU implementation of the octahedral unit vector encoding from 
  
    Cigolle, Donow, Evangelakos, Mara, McGuire, Meyer, 
    A Survey of Efficient Representations for Independent Unit Vectors, Journal of Computer Graphics Techniques (JCGT), vol. 3, no. 2, 1-30, 2014 

    Available online http://jcgt.org/published/0003/02/01/
*/
#ifndef G3D_octahedral_glsl
#define G3D_octahedral_glsl

#if !defined(__cplusplus) || !defined(__STDC__)
@block octahedral_helpers
#endif

float signNotZero(float value)
{
    return value >= 0.0 ? 1.0 : -1.0;
}

vec2 signNotZero(vec2 value)
{
    return vec2(
		signNotZero(value.x),
		signNotZero(value.y)
	);
}

vec3 signNotZero(vec3 value)
{
    return vec3(
		signNotZero(value.x),
		signNotZero(value.y),
		signNotZero(value.z)
	);
}

vec4 signNotZero(vec4 value)
{
    return vec4(
		signNotZero(value.x),
		signNotZero(value.y),
		signNotZero(value.z),
		signNotZero(value.w)
	);
}

/** Assumes that v is a unit vector. The result is an octahedral vector on the [-1, +1] square. */
vec2 octahedral_encode(in vec3 v)
{
    float l1norm = abs(v.x) + abs(v.y) + abs(v.z);
    vec2 result = v.xy * (1.0 / l1norm);
    if (v.z < 0.0)
	{
        result = (1.0 - abs(result.yx)) * signNotZero(result.xy);
    }
    return result;
}


/** Returns a unit vector. Argument o is an octahedral vector packed via octEncode,
    on the [-1, +1] square*/
vec3 octahedral_decode(vec2 o)
{
    vec3 v = vec3(o.x, o.y, 1.0 - abs(o.x) - abs(o.y));
    if (v.z < 0.0)
	{
        v.xy = (1.0 - abs(v.yx)) * signNotZero(v.xy);
    }
    return normalize(v);
}

#define ATLAS_GUTTER_SIZE 2.0

/** 
	Function used when rendering the cubemap data into the atlas. 
	While we use sg_apply_viewport to ensure that we are rendering into the correct tile of the atlas, 
	we also need to adjust our UV calculations to account for the gutter space we have in each tile to 
	prevent bleeding between tiles.
**/
vec2 make_padded_atlas_uv(vec2 quad_uv, float atlas_entry_size)
{
    // How much of the tile is actually the "inner" data
    float active_area = atlas_entry_size - (ATLAS_GUTTER_SIZE * 2.0);
    
    // Remap 0->1 quad to include the gutter space
    // If quad_uv is 0, this returns a value slightly less than 0
    vec2 remapped_uv = (quad_uv * atlas_entry_size - ATLAS_GUTTER_SIZE) / active_area;
    
    // Convert to [-1, 1] for the standard octahedral_decode
    return (remapped_uv * 2.0) - 1.0;
}

/**
	This function is used when sampling from the atlas during the lighting pass. It combines the logic of 
	calculating the correct tile in the atlas based on the normal, as well as adjusting for the gutter space 
	to prevent bleeding between tiles.
**/
vec2 padded_atlas_uv_from_normal(vec3 normal, int atlas_idx, int atlas_total_size, int atlas_entry_size)
{
    // Pure projection
    vec2 local_uv = octahedral_encode(normalize(normal)) * 0.5 + 0.5;

    // Shrink 0->1 into the inner "active" part of the entry
    float scale_to_entry = (float(atlas_entry_size) - (ATLAS_GUTTER_SIZE * 2.0)) / float(atlas_total_size);
    float margin_offset = ATLAS_GUTTER_SIZE / float(atlas_total_size);

    // Atlas Slot Calculation
    int slots_per_dim = atlas_total_size / atlas_entry_size;
    vec2 slot_offset = vec2(atlas_idx % slots_per_dim, atlas_idx / slots_per_dim) * (float(atlas_entry_size) / float(atlas_total_size));

    return (local_uv * scale_to_entry) + margin_offset + slot_offset;
}

#if !defined(__cplusplus) || !defined(__STDC__)
@end // @block octahedral_helpers
#endif

#endif // G3D_octahedral_glsl
