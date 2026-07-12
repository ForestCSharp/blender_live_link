@cs skin_vertices

layout(binding=0) uniform skinning_params {
	int vertex_count;
	int base_vertex;
	int padding0;
	int padding1;
};

struct SkinningVertex
{
	vec4 position;
	vec4 normal;
	vec2 texcoord;
	float padding0;
	float padding1;
};

struct SkinningWeights
{
	vec4 joint_indices;
	vec4 joint_weights;
};

struct SkinMatrix
{
	mat4 value;
};

layout(binding=0) readonly buffer SourceVerticesBuffer {
	SkinningVertex source_vertices[];
};

layout(binding=1) readonly buffer SourceSkinningBuffer {
	SkinningWeights source_skinning[];
};

layout(binding=2) readonly buffer SkinMatricesBuffer {
	SkinMatrix skin_matrices[];
};

layout(binding=3) buffer SkinnedVerticesBuffer {
	SkinningVertex skinned_vertices[];
};

layout(local_size_x=64, local_size_y=1, local_size_z=1) in;

mat4 skinning_matrix(vec4 joint_indices, vec4 joint_weights)
{
	float total_weight = joint_weights.x + joint_weights.y + joint_weights.z + joint_weights.w;
	if (total_weight <= 0.0)
	{
		return mat4(1.0);
	}

	return
		skin_matrices[int(joint_indices.x)].value * joint_weights.x +
		skin_matrices[int(joint_indices.y)].value * joint_weights.y +
		skin_matrices[int(joint_indices.z)].value * joint_weights.z +
		skin_matrices[int(joint_indices.w)].value * joint_weights.w;
}

void main()
{
	uint vertex_index = uint(base_vertex) + gl_GlobalInvocationID.x;
	if (vertex_index >= uint(max(vertex_count, 0)))
	{
		return;
	}

	SkinningVertex source_vertex = source_vertices[vertex_index];
	SkinningWeights weights = source_skinning[vertex_index];
	mat4 skin_matrix = skinning_matrix(weights.joint_indices, weights.joint_weights);

	SkinningVertex out_vertex;
	out_vertex.position = skin_matrix * source_vertex.position;
	out_vertex.normal = vec4(normalize((skin_matrix * vec4(source_vertex.normal.xyz, 0.0)).xyz), 0.0);
	out_vertex.texcoord = source_vertex.texcoord;
	out_vertex.padding0 = 0.0;
	out_vertex.padding1 = 0.0;
	skinned_vertices[vertex_index] = out_vertex;
}
@end

@program skin_vertices skin_vertices
