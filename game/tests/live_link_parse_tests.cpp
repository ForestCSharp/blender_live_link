// Integration test for the live-link parse -> drain path: payload validation,
// resource limits, and in-place material/image edits. Builds its own payloads,
// so no Blender fixture is needed. Run via tests/run_live_link_parse_tests.sh.
#define GAME_BUILD_CONFIG_NAME "LiveLinkParseTest"
#define main live_link_parse_test_game_main
#include "../src/main.cpp"
#undef main
#include <cassert>
#include <vector>

namespace LL = Blender::LiveLink;

constexpr i32 IMAGE_UID = 5;
constexpr i32 MATERIAL_UID = 7;

// Parses like the socket thread, then drains like the main thread.
void apply(flatbuffers::FlatBufferBuilder& in_builder)
{
	DynamicArray<u8> payload;
	payload.add_uninitialized((i32) in_builder.GetSize());
	memcpy(payload.data(), in_builder.GetBufferPointer(), in_builder.GetSize());
	LiveLinkSystem::parse_flatbuffer_data(payload);
	LiveLinkSystem::drain(state);
}

// One 2x2 image and one material that samples it.
void apply_material(f32 in_red, u8 in_pixel, i32 in_image_id = IMAGE_UID)
{
	flatbuffers::FlatBufferBuilder builder;
	std::vector<u8> pixels;
	for (int i = 0; i < 4; ++i) pixels.insert(pixels.end(), {in_pixel, 0, 0, 255});
	const auto image = LL::CreateImage(builder, IMAGE_UID, 2, 2, builder.CreateVector(pixels));
	const LL::Vec4 color(in_red, 0.0f, 0.0f, 1.0f);
	const auto material = LL::CreateMaterial(builder, MATERIAL_UID, 0, &color, in_image_id);
	builder.FinishSizePrefixed(LL::CreateUpdate(builder, 0, 0,
		builder.CreateVector(&material, 1), builder.CreateVector(&image, 1)));
	apply(builder);
}

const Material& material_7()
{
	return state.materials.items[state.materials.id_to_index.at(MATERIAL_UID)];
}

void test_malformed_frames_are_dropped()
{
	// Size prefix matches, but the root offset points far outside the buffer.
	DynamicArray<u8> junk;
	const u8 bytes[] = {16, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0x7f, 1, 2, 3, 4, 5, 6, 7, 8};
	junk.add_uninitialized(sizeof(bytes));
	memcpy(junk.data(), bytes, sizeof(bytes));
	LiveLinkSystem::parse_flatbuffer_data(junk);
	assert(!state.live_link.scene_updates.receive());
}

void test_material_and_image_edits_apply_in_place()
{
	apply_material(1.0f, 10);
	assert(state.images.items.length() == 1 && state.materials.items.length() == 1);
	assert(material_7().base_color.X == 1.0f && material_7().base_color_image_index == 0);
	const VkImageView first_view = state.images.items[0].view;

	// Blender resends unchanged resources with every export: nothing is re-uploaded.
	apply_material(1.0f, 10);
	assert(state.images.items[0].view == first_view);

	// Changed values replace the existing slots; indices stay stable.
	apply_material(0.5f, 200);
	assert(state.images.items.length() == 1 && state.materials.items.length() == 1);
	assert(state.images.items[0].view != first_view);
	assert(material_7().base_color.X == 0.5f && material_7().base_color_image_index == 0);
}

void test_missing_image_resolves_to_no_texture()
{
	apply_material(0.5f, 200, 999);
	assert(material_7().base_color_image_index == -1);
	// Unchanged resend: the "not registered" warning must not repeat (see log).
	apply_material(0.5f, 200, 999);
	assert(material_7().base_color_image_index == -1);
}

void test_material_limit_skips_instead_of_exiting()
{
	flatbuffers::FlatBufferBuilder builder;
	std::vector<flatbuffers::Offset<LL::Material>> materials;
	for (i32 uid = 1000; uid < 1000 + MAX_MATERIALS + 1; ++uid)
	{
		materials.push_back(LL::CreateMaterial(builder, uid));
	}
	builder.FinishSizePrefixed(LL::CreateUpdate(builder, 0, 0, builder.CreateVector(materials)));
	apply(builder);
	assert(state.materials.items.length() == MAX_MATERIALS);
}

void test_unknown_light_type_drops_only_the_light()
{
	flatbuffers::FlatBufferBuilder builder;
	const LL::Vec3 zero(0, 0, 0), one(1, 1, 1);
	const LL::Quat identity(0, 0, 0, 1);
	const auto light = LL::CreateLight(builder, (LL::LightType) 42, &one);
	const auto object = LL::CreateObject(builder, 0, 321, true, &zero, &one, &identity, 0, 0, nullptr, light);
	builder.FinishSizePrefixed(LL::CreateUpdate(builder, builder.CreateVector(&object, 1)));
	apply(builder);
	assert(state.scene.objects.contains(321) && !state.scene.objects.at(321).has_light);
}

// One object at the origin with whatever mesh, armature or light is given.
const Object& apply_object(flatbuffers::FlatBufferBuilder& in_builder, i32 in_uid,
	flatbuffers::Offset<LL::Mesh> in_mesh, flatbuffers::Offset<LL::Armature> in_armature = 0,
	flatbuffers::Offset<LL::Light> in_light = 0)
{
	const LL::Vec3 zero(0, 0, 0), one(1, 1, 1);
	const LL::Quat identity(0, 0, 0, 1);
	const auto object = LL::CreateObject(in_builder, 0, in_uid, true, &zero, &one, &identity, in_mesh, in_armature, nullptr, in_light);
	in_builder.FinishSizePrefixed(LL::CreateUpdate(in_builder, in_builder.CreateVector(&object, 1)));
	apply(in_builder);
	return state.scene.objects.at(in_uid);
}

void test_light_with_missing_fields_drops_only_the_light()
{
	flatbuffers::FlatBufferBuilder no_color;
	assert(!apply_object(no_color, 330, 0, 0, LL::CreateLight(no_color, LL::LightType_Point)).has_light);

	const LL::Vec3 one(1, 1, 1);
	flatbuffers::FlatBufferBuilder no_point_table;
	assert(!apply_object(no_point_table, 331, 0, 0, LL::CreateLight(no_point_table, LL::LightType_Point, &one)).has_light);
}

void test_animation_counts_must_match_matrix_data()
{
	// Huge counts with no data: dropped instead of a 57 GB allocation.
	flatbuffers::FlatBufferBuilder huge;
	const auto huge_clip = LL::CreateAnimationDirect(huge, "huge", 24, 1, 30000, 30000);
	const Object& huge_object = apply_object(huge, 340, 0, LL::CreateArmature(huge, 0, huge.CreateVector(&huge_clip, 1)));
	assert(huge_object.has_armature && huge_object.armature.animations[0].skin_matrices == nullptr);

	// One frame of one bone, 16 floats: kept.
	flatbuffers::FlatBufferBuilder valid;
	const std::vector<f32> matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1};
	const auto valid_clip = LL::CreateAnimationDirect(valid, "valid", 24, 1, 1, 1, &matrix);
	const Object& valid_object = apply_object(valid, 341, 0, LL::CreateArmature(valid, 0, valid.CreateVector(&valid_clip, 1)));
	assert(valid_object.armature.animations[0].skin_matrices != nullptr);
	assert(valid_object.armature.animations[0].skin_matrices[0].Elements[3][0] == 2.0f);
}

void test_mesh_indices_must_address_existing_data()
{
	const std::vector<f32> positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
	const std::vector<f32> normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
	const std::vector<f32> texcoords = {0, 0, 1, 0, 0, 1};
	const std::vector<f32> weights = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

	const std::vector<u32> past_end = {0, 1, 5};
	flatbuffers::FlatBufferBuilder bad_indices;
	assert(!apply_object(bad_indices, 350, LL::CreateMeshDirect(bad_indices, &positions, &normals, &texcoords, &past_end)).has_mesh);

	const std::vector<u32> triangle = {0, 1, 2};
	const std::vector<i32> negative_joint = {0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0};
	flatbuffers::FlatBufferBuilder bad_joints;
	const Object& skinned = apply_object(bad_joints, 351, LL::CreateMeshDirect(bad_joints, &positions, &normals, &texcoords,
		&triangle, &negative_joint, &weights, nullptr, 1));
	assert(skinned.has_mesh && !skinned.mesh.has_skinned_vertices);
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	assert(glfwInit());
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	GLFWwindow* window = glfwCreateWindow(128, 128, "Live link parse tests", nullptr, nullptr);
	assert(window);
	state.window.handle = window;
	jolt_init();
	vulkan_context_init(&state.vk, window);
	init_materials_buffer(state);

	test_malformed_frames_are_dropped();
	test_material_and_image_edits_apply_in_place();
	test_missing_image_resolves_to_no_texture();
	test_material_limit_skips_instead_of_exiting();
	test_unknown_light_type_drops_only_the_light();
	test_light_with_missing_fields_drops_only_the_light();
	test_animation_counts_must_match_matrix_data();
	test_mesh_indices_must_address_existing_data();

	LiveLinkSystem::cleanup_imported_resources(state);
	state.materials.buffer.destroy_gpu_buffer();
	jolt_shutdown();
	vulkan_context_shutdown(&state.vk);
	glfwDestroyWindow(window);
	glfwTerminate();
	puts("LIVE_LINK_PARSE_TESTS_PASSED");
}
