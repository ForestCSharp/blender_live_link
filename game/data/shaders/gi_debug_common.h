#ifndef GI_DEBUG_COMMON_H
#define GI_DEBUG_COMMON_H

layout(push_constant) uniform DebugParams
{
	mat4 view_projection;
	int octree_depth;
	float probe_debug_radius;
	int atlas_total_size;
	int atlas_entry_size;
	int probe_vis_mode;
	int isolated_probe_index;
	int specular_atlas_total_size;
	int specular_atlas_entry_size;
	int specular_mip_count;
	float specular_debug_roughness;
	int probe_level_filter_enable;
	int probe_level_filter_selection;
};

#endif // GI_DEBUG_COMMON_H
