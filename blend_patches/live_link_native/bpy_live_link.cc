/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "bpy_live_link.hh"

#include <Python.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "BKE_attribute.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"

#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector_types.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_ID.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"

#include "RNA_prototypes.hh"

#include "bpy_rna.hh"

#include "blender_live_link_generated.h"

namespace blender {
namespace {

namespace ll = Blender::LiveLink;

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

void add_object_materials(Object *object,
                          std::vector<int32_t> &material_ids,
                          std::map<int32_t, Material *> &referenced_materials)
{
  for (short slot = 1; slot <= object->totcol; slot++) {
    Material *material = BKE_object_material_get(object, slot);
    if (!material) {
      continue;
    }
    const int32_t material_id = int32_t(material->id.session_uid);
    material_ids.push_back(material_id);
    referenced_materials.emplace(material_id, material);
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

flatbuffers::Offset<ll::Mesh> export_mesh(flatbuffers::FlatBufferBuilder &builder,
                                          Object *object,
                                          Object *object_eval,
                                          Depsgraph *depsgraph,
                                          std::map<int32_t, Material *> &referenced_materials)
{
  if (!depsgraph || !object_eval) {
    return 0;
  }

  Mesh *mesh = BKE_object_to_mesh(depsgraph, object_eval, true);
  if (!mesh) {
    return 0;
  }

  std::vector<float> positions_out;
  std::vector<float> normals_out;
  std::vector<float> texcoords_out;
  std::vector<uint32_t> indices_out;
  std::vector<int32_t> material_ids;
  add_object_materials(object, material_ids, referenced_materials);

  const Span<float3> positions = mesh->vert_positions();
  const Span<float3> normals = mesh->vert_normals();
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<int3> corner_tris = mesh->corner_tris();

  std::unordered_map<MeshVertexKey, uint32_t, MeshVertexKeyHash> vertex_map;
  vertex_map.reserve(corner_tris.size() * 3);
  positions_out.reserve(corner_tris.size() * 9);
  normals_out.reserve(corner_tris.size() * 9);
  texcoords_out.reserve(corner_tris.size() * 6);
  indices_out.reserve(corner_tris.size() * 3);

  with_active_uv_map(mesh, [&](const VArraySpan<float2> *uv_map) {
    for (const int3 &tri : corner_tris) {
      for (int tri_corner = 0; tri_corner < 3; tri_corner++) {
        const int corner = tri[tri_corner];
        const int vertex = corner_verts[corner];
        float2 uv = {0.0f, 0.0f};
        if (uv_map) {
          uv = (*uv_map)[corner];
        }

        const MeshVertexKey key = {
            vertex, int(std::round(uv.x * 1000000.0f)), int(std::round(uv.y * 1000000.0f))};
        auto found = vertex_map.find(key);
        if (found != vertex_map.end()) {
          indices_out.push_back(found->second);
          continue;
        }

        const uint32_t new_index = uint32_t(vertex_map.size());
        vertex_map.emplace(key, new_index);
        indices_out.push_back(new_index);

        const float3 position = positions[vertex];
        const float3 normal = normals[vertex];
        positions_out.insert(positions_out.end(), {position.x, position.y, position.z});
        normals_out.insert(normals_out.end(), {normal.x, normal.y, normal.z});
        texcoords_out.insert(texcoords_out.end(), {uv.x, uv.y});
      }
    }
  });

  BKE_object_to_mesh_clear(object_eval);

  return ll::CreateMesh(builder,
                        builder.CreateVector(positions_out),
                        builder.CreateVector(normals_out),
                        builder.CreateVector(texcoords_out),
                        builder.CreateVector(indices_out),
                        0,
                        0,
                        builder.CreateVector(material_ids),
                        -1,
                        0,
                        0);
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

flatbuffers::Offset<ll::Object> export_object(flatbuffers::FlatBufferBuilder &builder,
                                              PyObject *py_object,
                                              Object *object,
                                              Depsgraph *depsgraph,
                                              std::map<int32_t, Material *> &referenced_materials)
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
    mesh_fb = export_mesh(builder, object, object_eval, depsgraph, referenced_materials);
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
  const auto components_fb = components.empty() ? 0 : builder.CreateVector(components);

  return ll::CreateObject(builder,
                          builder.CreateString(id_name(object->id)),
                          int32_t(object->id.session_uid),
                          (object->visibility_flag & OB_HIDE_VIEWPORT) == 0,
                          &location_fb,
                          &scale_fb,
                          &rotation_fb,
                          mesh_fb,
                          0,
                          rigid_body_ptr,
                          light_fb,
                          components_fb);
}

flatbuffers::Offset<ll::Material> export_material(flatbuffers::FlatBufferBuilder &builder,
                                                  const Material &material)
{
  const ll::Vec4 base_color(material.r, material.g, material.b, material.a);
  const ll::Vec4 emission_color(0.0f, 0.0f, 0.0f, 0.0f);
  return ll::CreateMaterial(builder,
                            int32_t(material.id.session_uid),
                            builder.CreateString(id_name(material.id)),
                            &base_color,
                            0,
                            material.metallic,
                            0,
                            material.roughness,
                            0,
                            &emission_color,
                            0,
                            0.0f);
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
  std::map<int32_t, Material *> referenced_materials;
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
        export_object(builder, py_object, object, depsgraph, referenced_materials));
  }

  std::vector<flatbuffers::Offset<ll::Material>> material_offsets;
  material_offsets.reserve(referenced_materials.size());
  for (const auto &item : referenced_materials) {
    material_offsets.push_back(export_material(builder, *item.second));
  }

  const auto elapsed = std::chrono::steady_clock::now() - start_time;
  const double generation_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();

  const auto update = ll::CreateUpdate(builder,
                                       builder.CreateVector(object_offsets),
                                       builder.CreateVector(deleted_ids),
                                       builder.CreateVector(material_offsets),
                                       0,
                                       reset,
                                       generation_seconds);
  ll::FinishSizePrefixedUpdateBuffer(builder, update);

  PySys_WriteStdout(
      "Blender Live Link native make_update: sequence=%d reason=%s reset=%s objects=%zu "
      "deleted=%zu materials=%zu bytes=%zu generation_seconds=%.6f\n",
      sequence,
      update_reason,
      reset ? "true" : "false",
      object_offsets.size(),
      deleted_ids.size(),
      material_offsets.size(),
      size_t(builder.GetSize()),
      generation_seconds);

  return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(builder.GetBufferPointer()),
                                   Py_ssize_t(builder.GetSize()));
}

}  // namespace blender
