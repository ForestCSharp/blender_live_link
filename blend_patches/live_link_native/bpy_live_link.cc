/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "bpy_live_link.hh"

#include <Python.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ANIM_action.hh"

#include "BKE_action.hh"
#include "BKE_anim_data.hh"
#include "BKE_armature.hh"
#include "BKE_deform.hh"
#include "BKE_main.hh"
#include "BKE_image.hh"
#include "BKE_attribute.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_object.hh"
#include "BKE_scene.hh"

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"

#include "DEG_depsgraph_query.hh"

#include "DNA_action_types.h"
#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"

#include "BKE_node_legacy_types.hh"

#include "IMB_imbuf_types.hh"

#include "RNA_prototypes.hh"

#include "bpy_rna.hh"

#include "blender_live_link_generated.h"

namespace blender {
namespace {

namespace ll = Blender::LiveLink;

template<typename T> std::vector<T> reversed_copy(const std::vector<T> &values)
{
  return std::vector<T>(values.rbegin(), values.rend());
}

struct PyPtr {
  PyObject *value = nullptr;

  PyPtr() = default;
  explicit PyPtr(PyObject *value) : value(value) {}
  ~PyPtr()
  {
    Py_XDECREF(value);
  }

  PyPtr(const PyPtr &) = delete;
  PyPtr &operator=(const PyPtr &) = delete;

  PyPtr(PyPtr &&other) noexcept : value(other.value)
  {
    other.value = nullptr;
  }

  operator PyObject *() const
  {
    return value;
  }
};

const char *id_name(const ID &id)
{
  return id.name + 2;
}

bool py_bool_attr(PyObject *object, const char *name, const bool default_value)
{
  PyPtr value(PyObject_GetAttrString(object, name));
  if (!value) {
    PyErr_Clear();
    return default_value;
  }
  const int truth = PyObject_IsTrue(value);
  if (truth < 0) {
    PyErr_Clear();
    return default_value;
  }
  return truth != 0;
}

float py_float_attr(PyObject *object, const char *name, const float default_value)
{
  PyPtr value(PyObject_GetAttrString(object, name));
  if (!value) {
    PyErr_Clear();
    return default_value;
  }
  const double result = PyFloat_AsDouble(value);
  if (PyErr_Occurred()) {
    PyErr_Clear();
    return default_value;
  }
  return float(result);
}

std::string py_string_attr(PyObject *object, const char *name, const char *default_value = "")
{
  PyPtr value(PyObject_GetAttrString(object, name));
  if (!value) {
    PyErr_Clear();
    return default_value;
  }
  const char *utf8 = PyUnicode_AsUTF8(value);
  if (!utf8) {
    PyErr_Clear();
    return default_value;
  }
  return utf8;
}

bool object_live_link_enabled(PyObject *py_object)
{
  PyPtr settings(PyObject_GetAttrString(py_object, "live_link_settings"));
  if (!settings) {
    PyErr_Clear();
    return true;
  }
  return py_bool_attr(settings, "enable_live_link", true);
}

bool object_visible_get(const Object *object, Depsgraph *depsgraph)
{
  if (!depsgraph) {
    return (object->visibility_flag & OB_HIDE_VIEWPORT) == 0;
  }

  ViewLayer *view_layer = DEG_get_input_view_layer(depsgraph);
  if (!view_layer) {
    return (object->visibility_flag & OB_HIDE_VIEWPORT) == 0;
  }

  Base *base = BKE_view_layer_base_find(view_layer, const_cast<Object *>(object));
  if (!base) {
    return false;
  }

  return BKE_base_is_visible(nullptr, base);
}

Object *object_from_py(PyObject *py_object)
{
  const PointerRNA *ptr = pyrna_struct_as_ptr(py_object, RNA_Object);
  if (!ptr) {
    return nullptr;
  }
  return static_cast<Object *>(ptr->data);
}

Depsgraph *depsgraph_from_py(PyObject *py_depsgraph)
{
  const PointerRNA *ptr = pyrna_struct_as_ptr(py_depsgraph, RNA_Depsgraph);
  if (!ptr) {
    return nullptr;
  }
  return static_cast<Depsgraph *>(ptr->data);
}

std::vector<int32_t> deleted_ids_from_py(PyObject *deleted_object_uids)
{
  PyPtr sequence(PySequence_Fast(deleted_object_uids, "deleted_object_uids must be a sequence"));
  if (!sequence) {
    return {};
  }

  std::vector<int32_t> result;
  const Py_ssize_t size = PySequence_Fast_GET_SIZE(sequence);
  result.reserve(size);
  for (Py_ssize_t index = 0; index < size; index++) {
    PyObject *item = PySequence_Fast_GET_ITEM(sequence.value, index);
    const long value = PyLong_AsLong(item);
    if (PyErr_Occurred()) {
      return {};
    }
    result.push_back(int32_t(value));
  }
  return result;
}

struct MeshVertexKey {
  int vertex = 0;
  int uv_x = 0;
  int uv_y = 0;

  bool operator==(const MeshVertexKey &other) const
  {
    return vertex == other.vertex && uv_x == other.uv_x && uv_y == other.uv_y;
  }
};

struct MeshVertexKeyHash {
  size_t operator()(const MeshVertexKey &key) const
  {
    size_t hash = std::hash<int>()(key.vertex);
    hash ^= std::hash<int>()(key.uv_x + 0x9e3779b9 + (hash << 6) + (hash >> 2));
    hash ^= std::hash<int>()(key.uv_y + 0x9e3779b9 + (hash << 6) + (hash >> 2));
    return hash;
  }
};

struct VertexSkinWeights {
  std::array<int32_t, 4> indices = {0, 0, 0, 0};
  std::array<float, 4> weights = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct DisabledModifierState {
  ModifierData *modifier = nullptr;
  int old_mode = 0;
};

struct ReferencedMaterials {
  std::vector<Material *> materials;
  std::unordered_set<int32_t> ids;

  void add(Material *material)
  {
    if (!material) {
      return;
    }
    const int32_t material_id = int32_t(material->id.session_uid);
    if (ids.insert(material_id).second) {
      materials.push_back(material);
    }
  }

  size_t size() const
  {
    return materials.size();
  }
};

struct ImageReference {
  Image *image = nullptr;
  ImageUser image_user{};
};

struct ReferencedImages {
  std::vector<ImageReference> images;
  std::unordered_set<int32_t> ids;

  int32_t add(Image *image, const ImageUser *image_user)
  {
    if (!image) {
      return 0;
    }
    const int32_t image_id = int32_t(image->id.session_uid);
    if (ids.insert(image_id).second) {
      ImageReference reference;
      reference.image = image;
      if (image_user) {
        reference.image_user = *image_user;
      }
      else {
        BKE_imageuser_default(&reference.image_user);
      }
      images.push_back(reference);
    }
    return image_id;
  }

  size_t size() const
  {
    return images.size();
  }
};

std::vector<float> matrix_to_column_major(const float matrix[4][4])
{
  std::vector<float> result;
  result.reserve(16);
  for (int column = 0; column < 4; column++) {
    for (int row = 0; row < 4; row++) {
      result.push_back(matrix[column][row]);
    }
  }
  return result;
}

flatbuffers::Offset<ll::Matrix> create_matrix(flatbuffers::FlatBufferBuilder &builder,
                                              const float matrix[4][4])
{
  return ll::CreateMatrix(builder, builder.CreateVector(matrix_to_column_major(matrix)));
}

Object *mesh_armature_object(Object *object)
{
  if (!object || object->type != OB_MESH) {
    return nullptr;
  }

  if (object->parent && object->parent->type == OB_ARMATURE) {
    return object->parent;
  }

  for (ModifierData *modifier = static_cast<ModifierData *>(object->modifiers.first); modifier;
       modifier = modifier->next)
  {
    if (modifier->type != eModifierType_Armature) {
      continue;
    }
    ArmatureModifierData *armature_modifier = reinterpret_cast<ArmatureModifierData *>(modifier);
    if (armature_modifier->object && armature_modifier->object->type == OB_ARMATURE) {
      return armature_modifier->object;
    }
  }

  return nullptr;
}

void collect_bones_recursive(const ListBaseT<Bone> &bone_base, std::vector<const Bone *> &bones)
{
  for (const Bone *bone = static_cast<const Bone *>(bone_base.first); bone; bone = bone->next) {
    bones.push_back(bone);
    collect_bones_recursive(bone->childbase, bones);
  }
}

std::vector<const Bone *> armature_bones(const bArmature &armature)
{
  std::vector<const Bone *> bones;
  bones.reserve(BKE_armature_bonelist_count(&armature.bonebase));
  collect_bones_recursive(armature.bonebase, bones);
  return bones;
}

std::unordered_map<std::string, int32_t> bone_index_by_name(const std::vector<const Bone *> &bones)
{
  std::unordered_map<std::string, int32_t> indices;
  indices.reserve(bones.size());
  for (int32_t index = 0; index < int32_t(bones.size()); index++) {
    indices.emplace(bones[index]->name, index);
  }
  return indices;
}

std::unordered_map<int, int32_t> deform_group_to_bone_index(
    Object *object, const std::unordered_map<std::string, int32_t> &bone_indices)
{
  std::unordered_map<int, int32_t> result;
  const ListBaseT<bDeformGroup> *defbase = BKE_object_defgroup_list(object);
  if (!defbase) {
    return result;
  }

  int group_index = 0;
  for (const bDeformGroup *group = static_cast<const bDeformGroup *>(defbase->first);
       group;
       group = group->next)
  {
    const auto bone_index = bone_indices.find(group->name);
    if (bone_index != bone_indices.end()) {
      result.emplace(group_index, bone_index->second);
    }
    group_index++;
  }
  return result;
}

std::vector<VertexSkinWeights> skin_weights_for_mesh(const Mesh &mesh,
                                                     Object *object,
                                                     Object *armature_object)
{
  std::vector<VertexSkinWeights> result(mesh.verts_num);
  if (!object || !armature_object || armature_object->type != OB_ARMATURE ||
      !armature_object->data)
  {
    return result;
  }

  const bArmature *armature = reinterpret_cast<const bArmature *>(armature_object->data);
  const std::vector<const Bone *> bones = armature_bones(*armature);
  const std::unordered_map<std::string, int32_t> bone_indices = bone_index_by_name(bones);
  Object *weight_object = DEG_get_original(object);
  if (!weight_object) {
    weight_object = object;
  }
  const std::unordered_map<int, int32_t> group_to_bone = deform_group_to_bone_index(weight_object,
                                                                                   bone_indices);
  if (group_to_bone.empty()) {
    return result;
  }

  Span<MDeformVert> deform_verts = mesh.deform_verts();
  if (deform_verts.is_empty() && weight_object->type == OB_MESH && weight_object->data) {
    const Mesh *original_mesh = reinterpret_cast<const Mesh *>(weight_object->data);
    deform_verts = original_mesh->deform_verts();
  }

  const int max_vertex = std::min<int>(int(result.size()), int(deform_verts.size()));
  for (int vertex_index = 0; vertex_index < max_vertex; vertex_index++) {
    const MDeformVert &deform_vert = deform_verts[vertex_index];
    std::vector<std::pair<int32_t, float>> influences;
    influences.reserve(deform_vert.totweight);
    for (int weight_index = 0; weight_index < deform_vert.totweight; weight_index++) {
      const MDeformWeight &weight = deform_vert.dw[weight_index];
      const auto bone_index = group_to_bone.find(weight.def_nr);
      if (bone_index == group_to_bone.end() || weight.weight <= 0.0f) {
        continue;
      }
      influences.emplace_back(bone_index->second, weight.weight);
    }

    std::sort(influences.begin(),
              influences.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    if (influences.size() > 4) {
      influences.resize(4);
    }

    float total_weight = 0.0f;
    for (const auto &influence : influences) {
      total_weight += influence.second;
    }
    if (total_weight <= 0.0f) {
      continue;
    }

    VertexSkinWeights &vertex_weights = result[vertex_index];
    for (size_t index = 0; index < influences.size(); index++) {
      vertex_weights.indices[index] = influences[index].first;
      vertex_weights.weights[index] = influences[index].second / total_weight;
    }
  }

  return result;
}

void mesh_armature_matrices(Object *object,
                            Object *armature_object,
                            float mesh_to_armature[4][4],
                            float armature_to_mesh[4][4])
{
  float inverse_armature_world[4][4];
  float inverse_mesh_world[4][4];
  invert_m4_m4(inverse_armature_world, armature_object->object_to_world().ptr());
  invert_m4_m4(inverse_mesh_world, object->object_to_world().ptr());
  mul_m4_m4m4(mesh_to_armature, inverse_armature_world, object->object_to_world().ptr());
  mul_m4_m4m4(armature_to_mesh, inverse_mesh_world, armature_object->object_to_world().ptr());
}

std::vector<DisabledModifierState> disable_armature_modifiers(Object *object)
{
  std::vector<DisabledModifierState> disabled;
  for (ModifierData *modifier = static_cast<ModifierData *>(object->modifiers.first); modifier;
       modifier = modifier->next)
  {
    if (modifier->type != eModifierType_Armature ||
        (modifier->mode & eModifierMode_Realtime) == 0)
    {
      continue;
    }
    disabled.push_back({modifier, modifier->mode});
    modifier->mode &= ~eModifierMode_Realtime;
  }
  return disabled;
}

void restore_modifiers(const std::vector<DisabledModifierState> &disabled)
{
  for (const DisabledModifierState &state : disabled) {
    if (state.modifier) {
      state.modifier->mode = state.old_mode;
    }
  }
}

void tag_object_geometry_update(Main *bmain, Depsgraph *depsgraph, Object *object)
{
  if (!bmain || !depsgraph || !object) {
    return;
  }
  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
  BKE_scene_graph_update_tagged(depsgraph, bmain);
}

void add_object_materials(Object *object,
                          std::vector<int32_t> &material_ids,
                          ReferencedMaterials &referenced_materials)
{
  for (short slot = 1; slot <= object->totcol; slot++) {
    Material *material = BKE_object_material_get(object, slot);
    if (!material) {
      continue;
    }
    const int32_t material_id = int32_t(material->id.session_uid);
    material_ids.push_back(material_id);
    referenced_materials.add(material);
  }
}

template<typename Fn>
void with_active_uv_map(Mesh *mesh, Fn &&fn)
{
  const bke::AttributeAccessor attributes = mesh->attributes();
  const StringRefNull uv_name = mesh->active_or_default_uv_map_name();
  if (!uv_name.is_empty()) {
    const bke::AttributeReader<float2> uv_attribute = attributes.lookup<float2>(
        uv_name, bke::AttrDomain::Corner);
    if (uv_attribute) {
      const VArraySpan<float2> uv_map(*uv_attribute);
      if (!uv_map.is_empty()) {
        fn(&uv_map);
        return;
      }
    }
  }
  fn(nullptr);
}

bool legacy_action_targets_armature(const bAction &action,
                                    const std::unordered_set<std::string> &bone_names)
{
  for (const FCurve *fcurve = static_cast<const FCurve *>(action.curves.first); fcurve;
       fcurve = fcurve->next)
  {
    if (!fcurve->rna_path) {
      continue;
    }
    char bone_name[MAXBONENAME];
    if (!BLI_str_quoted_substr(fcurve->rna_path, "pose.bones[", bone_name, sizeof(bone_name)))
    {
      continue;
    }
    if (bone_names.find(bone_name) != bone_names.end()) {
      return true;
    }
  }
  return false;
}

bool layered_action_targets_armature(const bAction &action,
                                     const std::unordered_set<std::string> &bone_names)
{
  bool found = false;
  auto callback = [&](const FCurve *, const char *bone_name) {
    if (!found && bone_name && bone_names.find(bone_name) != bone_names.end()) {
      found = true;
    }
  };

  if (action.slot_array_num > 0) {
    for (int index = 0; index < action.slot_array_num && !found; index++) {
      const ActionSlot *slot = action.slot_array[index];
      if (!slot) {
        continue;
      }
      bke::BKE_action_find_fcurves_with_bones(&action, slot->handle, callback);
    }
  }
  else {
    bke::BKE_action_find_fcurves_with_bones(&action, animrig::Slot::unassigned, callback);
  }

  return found;
}

bool action_targets_armature(const bAction *action, Object *armature_object)
{
  if (!action || !armature_object || armature_object->type != OB_ARMATURE ||
      !armature_object->data)
  {
    return false;
  }

  const bArmature *armature = reinterpret_cast<const bArmature *>(armature_object->data);
  const std::vector<const Bone *> bones = armature_bones(*armature);
  std::unordered_set<std::string> bone_names;
  bone_names.reserve(bones.size());
  for (const Bone *bone : bones) {
    bone_names.insert(bone->name);
  }

  return legacy_action_targets_armature(*action, bone_names) ||
         layered_action_targets_armature(*action, bone_names);
}

std::vector<bAction *> armature_actions(Main *bmain, Object *armature_object)
{
  std::vector<bAction *> result;
  if (!bmain || !armature_object) {
    return result;
  }

  AnimData *anim_data = BKE_animdata_from_id(&armature_object->id);
  bAction *active_action = anim_data ? anim_data->action : nullptr;
  if (action_targets_armature(active_action, armature_object)) {
    result.push_back(active_action);
  }

  std::vector<bAction *> remaining_actions;
  for (bAction *action = static_cast<bAction *>(bmain->actions.first); action;
       action = reinterpret_cast<bAction *>(action->id.next))
  {
    if (action == active_action) {
      continue;
    }
    if (action_targets_armature(action, armature_object)) {
      remaining_actions.push_back(action);
    }
  }

  std::sort(remaining_actions.begin(), remaining_actions.end(), [](const bAction *a, const bAction *b) {
    return std::strcmp(id_name(a->id), id_name(b->id)) < 0;
  });
  result.insert(result.end(), remaining_actions.begin(), remaining_actions.end());
  return result;
}

struct AnimationState {
  bAction *action = nullptr;
  int32_t slot_handle = 0;
  char last_slot_identifier[MAX_ID_NAME] = "";
  float frame = 1.0f;
};

AnimationState capture_animation_state(Object *armature_object, Scene *scene)
{
  AnimationState state;
  state.frame = scene ? BKE_scene_frame_get(scene) : 1.0f;

  AnimData *anim_data = BKE_animdata_from_id(&armature_object->id);
  if (anim_data) {
    state.action = anim_data->action;
    state.slot_handle = anim_data->slot_handle;
    STRNCPY(state.last_slot_identifier, anim_data->last_slot_identifier);
  }
  return state;
}

void restore_animation_state(Object *armature_object,
                             Scene *scene,
                             Depsgraph *depsgraph,
                             const AnimationState &state)
{
  AnimData *anim_data = BKE_animdata_ensure_id(&armature_object->id);
  if (anim_data) {
    BKE_animdata_set_action(nullptr, &armature_object->id, state.action);
    anim_data = BKE_animdata_from_id(&armature_object->id);
    if (anim_data) {
      anim_data->slot_handle = state.slot_handle;
      STRNCPY(anim_data->last_slot_identifier, state.last_slot_identifier);
    }
  }

  if (scene && depsgraph) {
    BKE_scene_frame_set(scene, state.frame);
    BKE_scene_graph_update_for_newframe(depsgraph);
  }
}

flatbuffers::Offset<ll::Animation> export_animation(flatbuffers::FlatBufferBuilder &builder,
                                                    Object *armature_object,
                                                    bAction *action,
                                                    const std::vector<const Bone *> &bones,
                                                    Main *bmain,
                                                    Depsgraph *depsgraph)
{
  if (!action || !armature_object || !bmain || !depsgraph) {
    return 0;
  }

  Scene *scene = DEG_get_input_scene(depsgraph);
  if (!scene) {
    return 0;
  }

  const float frame_rate = scene->r.frs_sec_base > 0.0f ?
                               float(scene->r.frs_sec) / scene->r.frs_sec_base :
                               0.0f;
  const float2 frame_range = action->wrap().get_frame_range();
  const int frame_start = int(std::floor(frame_range.x));
  const int frame_end = int(std::ceil(frame_range.y));
  const int frame_count = std::max(1, frame_end - frame_start + 1);
  const int bone_count = int(bones.size());
  std::vector<float> skin_matrices;
  skin_matrices.reserve(size_t(frame_count) * size_t(bone_count) * 16);

  const AnimationState old_state = capture_animation_state(armature_object, scene);
  BKE_animdata_ensure_id(&armature_object->id);
  BKE_animdata_set_action(nullptr, &armature_object->id, action);

  for (int frame = frame_start; frame <= frame_end; frame++) {
    BKE_scene_frame_set(scene, float(frame));
    BKE_scene_graph_update_for_newframe(depsgraph);
    Object *armature_eval = DEG_get_evaluated(depsgraph, armature_object);

    for (const Bone *bone : bones) {
      float inverse_bind[4][4];
      invert_m4_m4(inverse_bind, bone->arm_mat);

      float skin_matrix[4][4];
      if (armature_eval && armature_eval->pose) {
        bPoseChannel *pose_channel = BKE_pose_channel_find_name(armature_eval->pose, bone->name);
        if (pose_channel) {
          mul_m4_m4m4(skin_matrix, pose_channel->pose_mat, inverse_bind);
        }
        else {
          unit_m4(skin_matrix);
        }
      }
      else {
        unit_m4(skin_matrix);
      }

      const std::vector<float> matrix_values = matrix_to_column_major(skin_matrix);
      skin_matrices.insert(skin_matrices.end(), matrix_values.begin(), matrix_values.end());
    }
  }

  restore_animation_state(armature_object, scene, depsgraph, old_state);

  const float duration_seconds = frame_rate > 0.0f ? float(frame_count) / frame_rate : 0.0f;
  return ll::CreateAnimation(builder,
                             builder.CreateString(id_name(action->id)),
                             frame_rate,
                             duration_seconds,
                             frame_count,
                             bone_count,
                             builder.CreateVector(skin_matrices));
}

flatbuffers::Offset<ll::Mesh> export_mesh(flatbuffers::FlatBufferBuilder &builder,
                                          Object *object,
                                          Object *mesh_armature,
                                          Main *bmain,
                                          Depsgraph *depsgraph,
                                          ReferencedMaterials &referenced_materials)
{
  if (!depsgraph || !object) {
    return 0;
  }

  std::vector<DisabledModifierState> disabled_armature_modifiers;
  if (mesh_armature) {
    disabled_armature_modifiers = disable_armature_modifiers(object);
    if (!disabled_armature_modifiers.empty()) {
      tag_object_geometry_update(bmain, depsgraph, object);
    }
  }

  Object *object_eval = DEG_get_evaluated(depsgraph, object);
  if (!object_eval) {
    restore_modifiers(disabled_armature_modifiers);
    if (!disabled_armature_modifiers.empty()) {
      tag_object_geometry_update(bmain, depsgraph, object);
    }
    return 0;
  }

  Mesh *mesh = BKE_object_to_mesh(depsgraph, object_eval, true);
  if (!mesh) {
    restore_modifiers(disabled_armature_modifiers);
    if (!disabled_armature_modifiers.empty()) {
      tag_object_geometry_update(bmain, depsgraph, object);
    }
    return 0;
  }

  std::vector<float> positions_out;
  std::vector<float> normals_out;
  std::vector<float> texcoords_out;
  std::vector<uint32_t> indices_out;
  std::vector<int32_t> joint_indices_out;
  std::vector<float> joint_weights_out;
  std::vector<int32_t> material_ids;
  add_object_materials(object, material_ids, referenced_materials);
  const std::vector<VertexSkinWeights> skin_weights = mesh_armature ?
                                                         skin_weights_for_mesh(
                                                             *mesh, object, mesh_armature) :
                                                         std::vector<VertexSkinWeights>();

  const Span<float3> positions = mesh->vert_positions();
  const Span<float3> normals = mesh->vert_normals();
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<int3> corner_tris = mesh->corner_tris();

  std::unordered_map<MeshVertexKey, uint32_t, MeshVertexKeyHash> key_to_index;
  key_to_index.reserve(corner_tris.size() * 3);
  positions_out.reserve(corner_tris.size() * 9);
  normals_out.reserve(corner_tris.size() * 9);
  texcoords_out.reserve(corner_tris.size() * 6);
  indices_out.reserve(corner_tris.size() * 3);
  if (mesh_armature) {
    joint_indices_out.reserve(corner_tris.size() * 12);
    joint_weights_out.reserve(corner_tris.size() * 12);
  }

  with_active_uv_map(mesh, [&](const VArraySpan<float2> *uv_map) {
    for (const int tri_index : corner_tris.index_range()) {
      const int3 &tri = corner_tris[tri_index];
      for (int tri_corner = 0; tri_corner < 3; tri_corner++) {
        const int corner = tri[tri_corner];
        const int vertex = corner_verts[corner];
        float2 uv = {0.0f, 0.0f};
        if (uv_map) {
          uv = (*uv_map)[corner];
        }

        const MeshVertexKey key = {vertex, int(uv.x * 1000000.0f), int(uv.y * 1000000.0f)};
        const auto [item, inserted] = key_to_index.emplace(key, uint32_t(key_to_index.size()));
        if (inserted) {
          const float3 position = positions[vertex];
          const float3 normal = normals[vertex];
          positions_out.insert(positions_out.end(), {position.x, position.y, position.z});
          normals_out.insert(normals_out.end(), {normal.x, normal.y, normal.z});
          texcoords_out.insert(texcoords_out.end(),
                               {float(key.uv_x) / 1000000.0f,
                                float(key.uv_y) / 1000000.0f});
          if (mesh_armature) {
            const VertexSkinWeights vertex_weights = vertex < int(skin_weights.size()) ?
                                                         skin_weights[vertex] :
                                                         VertexSkinWeights();
            joint_indices_out.insert(
                joint_indices_out.end(), vertex_weights.indices.begin(), vertex_weights.indices.end());
            joint_weights_out.insert(
                joint_weights_out.end(), vertex_weights.weights.begin(), vertex_weights.weights.end());
          }
        }

        indices_out.push_back(item->second);
      }
    }
  });
  BKE_object_to_mesh_clear(object_eval);
  restore_modifiers(disabled_armature_modifiers);
  if (!disabled_armature_modifiers.empty()) {
    tag_object_geometry_update(bmain, depsgraph, object);
  }

  int32_t armature_id = -1;
  flatbuffers::Offset<ll::Matrix> mesh_to_armature_fb = 0;
  flatbuffers::Offset<ll::Matrix> armature_to_mesh_fb = 0;
  if (mesh_armature) {
    armature_id = int32_t(mesh_armature->id.session_uid);
    float mesh_to_armature[4][4];
    float armature_to_mesh[4][4];
    mesh_armature_matrices(object, mesh_armature, mesh_to_armature, armature_to_mesh);
    mesh_to_armature_fb = create_matrix(builder, mesh_to_armature);
    armature_to_mesh_fb = create_matrix(builder, armature_to_mesh);
  }

  return ll::CreateMesh(builder,
                        builder.CreateVector(positions_out),
                        builder.CreateVector(normals_out),
                        builder.CreateVector(texcoords_out),
                        builder.CreateVector(indices_out),
                        mesh_armature ? builder.CreateVector(joint_indices_out) : 0,
                        mesh_armature ? builder.CreateVector(joint_weights_out) : 0,
                        builder.CreateVector(material_ids),
                        armature_id,
                        mesh_to_armature_fb,
                        armature_to_mesh_fb);
}

flatbuffers::Offset<ll::Light> export_light(flatbuffers::FlatBufferBuilder &builder, const Object *object)
{
  if (object->type != OB_LAMP || !object->data) {
    return 0;
  }

  const Light *light = reinterpret_cast<const Light *>(object->data);
  ll::LightType type = ll::LightType_Point;
  ll::PointLight point_light(light->energy);
  ll::SpotLight spot_light(light->energy, light->spotsize, light->spotblend);
  ll::SunLight sun_light(light->energy, (light->mode & LA_SHADOW) != 0);
  const void *point_ptr = nullptr;
  const void *spot_ptr = nullptr;
  const void *sun_ptr = nullptr;

  switch (light->type) {
    case LA_SUN:
      type = ll::LightType_Sun;
      sun_ptr = &sun_light;
      break;
    case LA_SPOT:
      type = ll::LightType_Spot;
      spot_ptr = &spot_light;
      break;
    case LA_AREA:
      type = ll::LightType_Area;
      break;
    case LA_LOCAL:
    default:
      type = ll::LightType_Point;
      point_ptr = &point_light;
      break;
  }

  const ll::Vec3 color(light->r, light->g, light->b);
  return ll::CreateLight(builder,
                         type,
                         &color,
                         (light->mode & LA_SHADOW) != 0,
                         static_cast<const ll::PointLight *>(point_ptr),
                         static_cast<const ll::SpotLight *>(spot_ptr),
                         static_cast<const ll::SunLight *>(sun_ptr));
}

std::vector<flatbuffers::Offset<ll::GameplayComponentContainer>> export_gameplay_components(
    flatbuffers::FlatBufferBuilder &builder, PyObject *py_object)
{
  std::vector<flatbuffers::Offset<ll::GameplayComponentContainer>> components_out;

  PyPtr settings(PyObject_GetAttrString(py_object, "live_link_settings"));
  if (!settings) {
    PyErr_Clear();
    return components_out;
  }

  PyPtr components(PyObject_GetAttrString(settings, "components"));
  if (!components) {
    PyErr_Clear();
    return components_out;
  }

  PyPtr sequence(PySequence_Fast(components, "live_link_settings.components must be a sequence"));
  if (!sequence) {
    PyErr_Clear();
    return components_out;
  }

  const Py_ssize_t size = PySequence_Fast_GET_SIZE(sequence);
  components_out.reserve(size);
  for (Py_ssize_t index = 0; index < size; index++) {
    PyObject *component = PySequence_Fast_GET_ITEM(sequence.value, index);
    const std::string type = py_string_attr(component, "type");
    if (type == "CHARACTER") {
      PyPtr player(PyObject_GetAttrString(component, "player"));
      if (!player) {
        PyErr_Clear();
        continue;
      }
      const auto value = ll::CreateGameplayComponentCharacter(
          builder,
          py_bool_attr(player, "player_controlled", false),
          py_float_attr(player, "move_speed", 20.0f),
          py_float_attr(player, "jump_speed", 10.0f));
      components_out.push_back(ll::CreateGameplayComponentContainer(
          builder, ll::GameplayComponent_GameplayComponentCharacter, value.Union()));
    }
    else if (type == "CAMERA_CONTROL") {
      PyPtr camera_control(PyObject_GetAttrString(component, "camera_control"));
      if (!camera_control) {
        PyErr_Clear();
        continue;
      }
      const auto value = ll::CreateGameplayComponentCameraControl(
          builder,
          py_float_attr(camera_control, "follow_distance", 5.0f),
          py_float_attr(camera_control, "follow_speed", 10.0f));
      components_out.push_back(ll::CreateGameplayComponentContainer(
          builder, ll::GameplayComponent_GameplayComponentCameraControl, value.Union()));
    }
  }

  return components_out;
}

flatbuffers::Offset<ll::Armature> export_armature(flatbuffers::FlatBufferBuilder &builder,
                                                  Object *armature_object,
                                                  Main *bmain,
                                                  Depsgraph *depsgraph)
{
  if (!armature_object || armature_object->type != OB_ARMATURE || !armature_object->data) {
    return 0;
  }

  const bArmature *armature = reinterpret_cast<const bArmature *>(armature_object->data);
  const std::vector<const Bone *> bones = armature_bones(*armature);
  const std::unordered_map<std::string, int32_t> bone_indices = bone_index_by_name(bones);

  std::vector<flatbuffers::Offset<ll::Bone>> bone_offsets;
  bone_offsets.reserve(bones.size());
  for (const Bone *bone : bones) {
    const char *parent_name = bone->parent ? bone->parent->name : nullptr;
    int32_t parent_index = -1;
    if (parent_name) {
      const auto item = bone_indices.find(parent_name);
      parent_index = item != bone_indices.end() ? item->second : -1;
    }

    float inverse_bind[4][4];
    invert_m4_m4(inverse_bind, bone->arm_mat);
    bone_offsets.push_back(ll::CreateBone(builder,
                                          builder.CreateString(bone->name),
                                          parent_name ? builder.CreateString(parent_name) : 0,
                                          parent_index,
                                          create_matrix(builder, inverse_bind)));
  }

  std::vector<flatbuffers::Offset<ll::Animation>> animation_offsets;
  const std::vector<bAction *> actions = armature_actions(bmain, armature_object);
  animation_offsets.reserve(actions.size());
  for (bAction *action : actions) {
    const auto animation = export_animation(builder, armature_object, action, bones, bmain, depsgraph);
    if (animation.o != 0) {
      animation_offsets.push_back(animation);
    }
  }

  return ll::CreateArmature(builder,
                            builder.CreateVector(bone_offsets),
                            builder.CreateVector(animation_offsets));
}

flatbuffers::Offset<ll::Object> export_object(flatbuffers::FlatBufferBuilder &builder,
                                              PyObject *py_object,
                                              Object *object,
                                              Main *bmain,
                                              Depsgraph *depsgraph,
                                              ReferencedMaterials &referenced_materials)
{
  Object *object_eval = depsgraph ? DEG_get_evaluated(depsgraph, object) : object;

  float location[3] = {0.0f, 0.0f, 0.0f};
  float quaternion[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  float scale[3] = {1.0f, 1.0f, 1.0f};
  mat4_to_loc_quat(location, quaternion, object_eval->object_to_world().ptr());
  mat4_to_size(scale, object_eval->object_to_world().ptr());

  const ll::Vec3 location_fb(location[0], location[1], location[2]);
  const ll::Vec3 scale_fb(scale[0], scale[1], scale[2]);
  const ll::Quat rotation_fb(quaternion[1], quaternion[2], quaternion[3], quaternion[0]);

  flatbuffers::Offset<ll::Mesh> mesh_fb = 0;
  if (object->type == OB_MESH) {
    mesh_fb = export_mesh(
        builder, object, mesh_armature_object(object), bmain, depsgraph, referenced_materials);
  }

  flatbuffers::Offset<ll::Armature> armature_fb = 0;
  if (object->type == OB_ARMATURE) {
    armature_fb = export_armature(builder, object, bmain, depsgraph);
  }

  flatbuffers::Offset<ll::Light> light_fb = export_light(builder, object_eval);

  ll::RigidBody rigid_body_fb(false, 0.0f);
  const ll::RigidBody *rigid_body_ptr = nullptr;
  if (object->rigidbody_object) {
    const RigidBodyOb *rigid_body = object->rigidbody_object;
    rigid_body_fb = ll::RigidBody(rigid_body->type == RBO_TYPE_ACTIVE &&
                                      (rigid_body->flag & RBO_FLAG_DISABLED) == 0 &&
                                      (rigid_body->flag & RBO_FLAG_KINEMATIC) == 0,
                                  object->rigidbody_object->mass);
    rigid_body_ptr = &rigid_body_fb;
  }

  std::vector<flatbuffers::Offset<ll::GameplayComponentContainer>> components =
      export_gameplay_components(builder, py_object);
  const auto components_fb = components.empty() ? 0 : builder.CreateVector(reversed_copy(components));

  return ll::CreateObject(builder,
                          builder.CreateString(id_name(object->id)),
                          int32_t(object->id.session_uid),
                          object_visible_get(object, depsgraph),
                          &location_fb,
                          &scale_fb,
                          &rotation_fb,
                          mesh_fb,
                          armature_fb,
                          rigid_body_ptr,
                          light_fb,
                          components_fb);
}

struct MaterialExportData {
  ll::Vec4 base_color = ll::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
  int32_t base_color_image_id = 0;
  float metallic = 0.0f;
  int32_t metallic_image_id = 0;
  float roughness = 0.0f;
  int32_t roughness_image_id = 0;
  ll::Vec4 emission_color = ll::Vec4(0.0f, 0.0f, 0.0f, 0.0f);
  int32_t emission_color_image_id = 0;
  float emission_strength = 0.0f;
};

const bNode *find_principled_bsdf(const Material &material)
{
  if (!material.nodetree) {
    return nullptr;
  }
  for (const bNode *node = static_cast<const bNode *>(material.nodetree->nodes.first); node;
       node = node->next)
  {
    if (node->type_legacy == SH_NODE_BSDF_PRINCIPLED ||
        std::strcmp(node->idname, "ShaderNodeBsdfPrincipled") == 0)
    {
      return node;
    }
  }
  return nullptr;
}

const bNodeSocket *find_input_socket(const bNode &node, const char *identifier)
{
  if (const bNodeSocket *socket = bke::node_find_socket(node, SOCK_IN, identifier)) {
    return socket;
  }
  return nullptr;
}

float socket_float_default(const bNodeSocket *socket, const float default_value)
{
  if (!socket || !socket->default_value) {
    return default_value;
  }
  const bNodeSocketValueFloat *value = static_cast<const bNodeSocketValueFloat *>(
      socket->default_value);
  return value->value;
}

ll::Vec4 socket_rgba_default(const bNodeSocket *socket, const ll::Vec4 &default_value)
{
  if (!socket || !socket->default_value) {
    return default_value;
  }
  const bNodeSocketValueRGBA *value = static_cast<const bNodeSocketValueRGBA *>(
      socket->default_value);
  return ll::Vec4(value->value[0], value->value[1], value->value[2], value->value[3]);
}

int32_t linked_image_id(const bNodeSocket *socket, ReferencedImages &referenced_images)
{
  if (!socket || !socket->link) {
    return 0;
  }
  const bNodeLink *link = socket->link;
  const bNode *from_node = link->fromnode;
  if (!from_node ||
      !(from_node->type_legacy == SH_NODE_TEX_IMAGE ||
        std::strcmp(from_node->idname, "ShaderNodeTexImage") == 0) ||
      !from_node->id)
  {
    return 0;
  }

  const NodeTexImage *texture_node = static_cast<const NodeTexImage *>(from_node->storage);
  ImageUser image_user{};
  const ImageUser *image_user_ptr = nullptr;
  if (texture_node) {
    image_user = texture_node->iuser;
    image_user_ptr = &image_user;
  }
  return referenced_images.add(reinterpret_cast<Image *>(from_node->id), image_user_ptr);
}

MaterialExportData extract_material_data(const Material &material,
                                         ReferencedImages &referenced_images)
{
  MaterialExportData data;
  const bNode *bsdf = find_principled_bsdf(material);
  if (!bsdf) {
    return data;
  }

  const bNodeSocket *base_color = find_input_socket(*bsdf, "Base Color");
  data.base_color = socket_rgba_default(base_color, data.base_color);
  data.base_color_image_id = linked_image_id(base_color, referenced_images);

  const bNodeSocket *metallic = find_input_socket(*bsdf, "Metallic");
  data.metallic = socket_float_default(metallic, data.metallic);
  data.metallic_image_id = linked_image_id(metallic, referenced_images);

  const bNodeSocket *roughness = find_input_socket(*bsdf, "Roughness");
  data.roughness = socket_float_default(roughness, data.roughness);
  data.roughness_image_id = linked_image_id(roughness, referenced_images);

  const bNodeSocket *emission_color = find_input_socket(*bsdf, "Emission Color");
  data.emission_color = socket_rgba_default(emission_color, data.emission_color);
  data.emission_color_image_id = linked_image_id(emission_color, referenced_images);

  const bNodeSocket *emission_strength = find_input_socket(*bsdf, "Emission Strength");
  data.emission_strength = socket_float_default(emission_strength, data.emission_strength);

  return data;
}

uint8_t float_to_rgba8(const float value)
{
  const float clamped = std::clamp(value, 0.0f, 1.0f) * 255.0f;
  return uint8_t(std::nearbyint(clamped));
}

std::vector<uint8_t> image_pixels_rgba8(const ImBuf &image_buffer)
{
  const int64_t pixel_count = int64_t(image_buffer.x) * int64_t(image_buffer.y);
  std::vector<uint8_t> pixels;
  if (pixel_count <= 0) {
    return pixels;
  }
  pixels.resize(size_t(pixel_count) * 4);

  if (image_buffer.float_buffer.data) {
    const float *source = image_buffer.float_buffer.data;
    const int channels = image_buffer.channels == 0 ? 4 : image_buffer.channels;
    for (int64_t pixel = 0; pixel < pixel_count; pixel++) {
      const int64_t source_index = pixel * channels;
      const int64_t dest_index = pixel * 4;
      pixels[dest_index + 0] = float_to_rgba8(source[source_index + 0]);
      pixels[dest_index + 1] = channels > 1 ? float_to_rgba8(source[source_index + 1]) : pixels[dest_index + 0];
      pixels[dest_index + 2] = channels > 2 ? float_to_rgba8(source[source_index + 2]) : pixels[dest_index + 0];
      pixels[dest_index + 3] = channels > 3 ? float_to_rgba8(source[source_index + 3]) : 255;
    }
    return pixels;
  }

  if (image_buffer.byte_buffer.data) {
    const uint8_t *source = image_buffer.byte_buffer.data;
    const int channels = image_buffer.channels == 0 ? 4 : image_buffer.channels;
    for (int64_t pixel = 0; pixel < pixel_count; pixel++) {
      const int64_t source_index = pixel * channels;
      const int64_t dest_index = pixel * 4;
      pixels[dest_index + 0] = source[source_index + 0];
      pixels[dest_index + 1] = channels > 1 ? source[source_index + 1] : pixels[dest_index + 0];
      pixels[dest_index + 2] = channels > 2 ? source[source_index + 2] : pixels[dest_index + 0];
      pixels[dest_index + 3] = channels > 3 ? source[source_index + 3] : 255;
    }
  }

  return pixels;
}

flatbuffers::Offset<ll::Image> export_image(flatbuffers::FlatBufferBuilder &builder,
                                            const ImageReference &reference)
{
  void *lock = nullptr;
  ImBuf *image_buffer = BKE_image_acquire_ibuf(reference.image,
                                              const_cast<ImageUser *>(&reference.image_user),
                                              &lock);
  if (!image_buffer) {
    return ll::CreateImage(builder, int32_t(reference.image->id.session_uid), 0, 0, 0);
  }

  std::vector<uint8_t> pixels = image_pixels_rgba8(*image_buffer);
  const int width = image_buffer->x;
  const int height = image_buffer->y;
  BKE_image_release_ibuf(reference.image, image_buffer, lock);

  return ll::CreateImage(builder,
                         int32_t(reference.image->id.session_uid),
                         width,
                         height,
                         builder.CreateVector(pixels));
}

flatbuffers::Offset<ll::Material> export_material(flatbuffers::FlatBufferBuilder &builder,
                                                  const Material &material,
                                                  ReferencedImages &referenced_images)
{
  const MaterialExportData data = extract_material_data(material, referenced_images);
  return ll::CreateMaterial(builder,
                            int32_t(material.id.session_uid),
                            builder.CreateString(id_name(material.id)),
                            &data.base_color,
                            data.base_color_image_id,
                            data.metallic,
                            data.metallic_image_id,
                            data.roughness,
                            data.roughness_image_id,
                            &data.emission_color,
                            data.emission_color_image_id,
                            data.emission_strength);
}

}  // namespace

PyObject *BPY_live_link_make_update(PyObject *objects,
                                    PyObject *deleted_object_uids,
                                    PyObject *dependency_graph,
                                    bool reset,
                                    const char *update_reason,
                                    int sequence)
{
  auto start_time = std::chrono::steady_clock::now();
  Depsgraph *depsgraph = depsgraph_from_py(dependency_graph);
  if (!depsgraph) {
    return nullptr;
  }
  Main *bmain = DEG_get_bmain(depsgraph);

  PyPtr object_sequence(PySequence_Fast(objects, "objects must be a sequence"));
  if (!object_sequence) {
    return nullptr;
  }

  std::vector<int32_t> deleted_ids = deleted_ids_from_py(deleted_object_uids);
  if (PyErr_Occurred()) {
    return nullptr;
  }

  flatbuffers::FlatBufferBuilder builder(1024);
  std::vector<flatbuffers::Offset<ll::Object>> object_offsets;
  ReferencedMaterials referenced_materials;
  ReferencedImages referenced_images;
  std::unordered_set<uint32_t> exported_object_ids;

  const Py_ssize_t object_count = PySequence_Fast_GET_SIZE(object_sequence);
  object_offsets.reserve(object_count);
  for (Py_ssize_t index = 0; index < object_count; index++) {
    PyObject *py_object = PySequence_Fast_GET_ITEM(object_sequence.value, index);
    Object *object = object_from_py(py_object);
    if (!object) {
      return nullptr;
    }
    if (!object_live_link_enabled(py_object)) {
      continue;
    }
    if (!exported_object_ids.insert(object->id.session_uid).second) {
      continue;
    }
    object_offsets.push_back(
        export_object(builder, py_object, object, bmain, depsgraph, referenced_materials));
  }

  std::vector<flatbuffers::Offset<ll::Material>> material_offsets;
  material_offsets.reserve(referenced_materials.size());
  for (const Material *material : referenced_materials.materials) {
    material_offsets.push_back(export_material(builder, *material, referenced_images));
  }

  std::vector<flatbuffers::Offset<ll::Image>> image_offsets;
  image_offsets.reserve(referenced_images.size());
  for (const ImageReference &reference : referenced_images.images) {
    image_offsets.push_back(export_image(builder, reference));
  }

  const auto elapsed = std::chrono::steady_clock::now() - start_time;
  const double generation_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();

  const auto update = ll::CreateUpdate(builder,
                                       builder.CreateVector(reversed_copy(object_offsets)),
                                       builder.CreateVector(reversed_copy(deleted_ids)),
                                       builder.CreateVector(material_offsets),
                                       builder.CreateVector(image_offsets),
                                       reset,
                                       generation_seconds);
  ll::FinishSizePrefixedUpdateBuffer(builder, update);

  PySys_WriteStdout(
      "Blender Live Link native make_update: sequence=%d reason=%s reset=%s objects=%zu "
      "deleted=%zu materials=%zu images=%zu bytes=%zu generation_seconds=%.6f\n",
      sequence,
      update_reason,
      reset ? "true" : "false",
      object_offsets.size(),
      deleted_ids.size(),
      material_offsets.size(),
      image_offsets.size(),
      size_t(builder.GetSize()),
      generation_seconds);

  return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(builder.GetBufferPointer()),
                                   Py_ssize_t(builder.GetSize()));
}

}  // namespace blender
