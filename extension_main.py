try:
    import cython
    if cython.compiled:
        print("Running as compiled cython module.")
    else:
        print("Running as regular python module.")
except ImportError:
    print("Failed to import cython.")

import bpy
from bpy.props import (BoolProperty, StringProperty, FloatProperty, FloatVectorProperty, PointerProperty, CollectionProperty, EnumProperty)
from bpy.types import (Panel, Operator, PropertyGroup)
from bpy.app.handlers import persistent

import bmesh
import builtins
import numpy as np
import os
import socket
import sys
import traceback
import time

from io import StringIO

from os.path import dirname, realpath, basename, isfile, join
import glob

# ignore SIGPIPE so that writing to a closed socket raises a Python exception
import signal
try:
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)
except Exception:
    # Not all platforms expose SIGPIPE (Windows), ignore failures
    pass

path_to_append = dirname(realpath(__file__)) + "/compiled_schemas/python"
if path_to_append not in sys.path:
    sys.path.append(path_to_append)

from .compiled_schemas.python import flatbuffers
from .compiled_schemas.python.Blender.LiveLink import Armature
from .compiled_schemas.python.Blender.LiveLink import Animation
from .compiled_schemas.python.Blender.LiveLink import Bone 
from .compiled_schemas.python.Blender.LiveLink import GameplayComponent
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentCameraControl
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentCharacter
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentContainer
from .compiled_schemas.python.Blender.LiveLink import Image
from .compiled_schemas.python.Blender.LiveLink import Light
from .compiled_schemas.python.Blender.LiveLink import LightType
from .compiled_schemas.python.Blender.LiveLink import Material
from .compiled_schemas.python.Blender.LiveLink import Matrix
from .compiled_schemas.python.Blender.LiveLink import Mesh
from .compiled_schemas.python.Blender.LiveLink import Object
from .compiled_schemas.python.Blender.LiveLink import PointLight
from .compiled_schemas.python.Blender.LiveLink import Quat
from .compiled_schemas.python.Blender.LiveLink import RigidBody 
from .compiled_schemas.python.Blender.LiveLink import SpotLight
from .compiled_schemas.python.Blender.LiveLink import SunLight
from .compiled_schemas.python.Blender.LiveLink import Update
from .compiled_schemas.python.Blender.LiveLink import Vec3
from .compiled_schemas.python.Blender.LiveLink import Vec4

# Overridden print that prints to blender console windows
def print(*args, **kwargs):
    # Standard print to stdout
    builtins.print(*args, **kwargs)
    
    # Get the formatted string from print
    output = ' '.join(str(arg) for arg in args)
    if 'sep' in kwargs:
        output = kwargs['sep'].join(str(arg) for arg in args)
    
    # Find all CONSOLE windows and print to them
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'CONSOLE':
                for region in area.regions:
                    if region.type == 'WINDOW':
                        # Access the console and execute print command
                        with bpy.context.temp_override(
                            window=window,
                            area=area,
                            region=region
                        ):
                            bpy.ops.console.scrollback_append(
                                text=output,
                                type='OUTPUT'
                            )

# Class to manage our live link connection
class LiveLinkConnection():
    def __init__(self):
        self.update_sequence = 0
        self.create_socket()
        
    def __del__(self):
       self.close_socket() 

    def create_socket(self):
        # Create a new socket object 
        self.my_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.my_socket.settimeout(5.0)

        # Try to disable SIGPIPE on BSD / macOS (SO_NOSIGPIPE) if available
        # and otherwise rely on the global SIGPIPE ignore above.
        try:
            if hasattr(socket, "SO_NOSIGPIPE"):
                self.my_socket.setsockopt(socket.SOL_SOCKET, socket.SO_NOSIGPIPE, 1)
        except Exception:
            # best-effort: ignore if platform doesn't support it
            pass

        # Allow immediate reuse of address if you repeatedly restart game/server locally
        try:
            self.my_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        except Exception:
            pass

    def close_socket(self):
        try:
            self.my_socket.shutdown(socket.SHUT_RDWR)
        except:
            pass  # Socket might already be closed
        self.my_socket.close()

    def is_connected(self):
        try:
            self.my_socket.getpeername()
            return True
        except Exception as e:
            return False

    def connect(self):
        try:
            # Close old socket
            self.close_socket()

            # Create a new socket if attempting to reconnect
            self.create_socket()
            
            # FCS TODO: Store magic IP and Port numbers in some shared file
            HOST = '127.0.0.1'
            PORT = 65432
            self.my_socket.connect((HOST, PORT))
            return True
        except Exception as e:
            print("Failed to Connect to Running Game")
            return False

    def send(self, data):
        is_connected = self.is_connected()
        if not is_connected:
            print("Attempt to reconnect")
            is_connected = self.connect()

        if not is_connected:
            return

        try:
            self.my_socket.sendall(data)
        except Exception as e:
            print(traceback.format_exc())
            print("Error: LiveLinkConnection::send")

    def matrix_to_column_major_array(self, matrix):
        matrix_4x4 = matrix.to_4x4()
        return np.array(
            [matrix_4x4[row][col] for col in range(4) for row in range(4)],
            dtype=np.float32
        )

    def make_flatbuffer_matrix(self, builder, matrix):
        matrix_elements_fb = builder.CreateNumpyVector(self.matrix_to_column_major_array(matrix))
        Matrix.Start(builder)
        Matrix.AddElements(builder, matrix_elements_fb)
        return Matrix.End(builder)

    def add_export_timing(self, export_stats, key, elapsed_seconds):
        if export_stats is not None:
            export_stats["timings"][key] += elapsed_seconds

    def mesh_needs_triangulation(self, mesh):
        return any(polygon.loop_total != 3 for polygon in mesh.polygons)

    def get_mesh(self, obj, dependency_graph, use_rest_pose=False, export_stats=None):
        disabled_modifiers = []
        mesh_eval_start = time.perf_counter()
        if use_rest_pose:
            for modifier in obj.modifiers:
                if modifier.type == 'ARMATURE' and modifier.show_viewport:
                    disabled_modifiers.append(modifier)
                    modifier.show_viewport = False
            bpy.context.view_layer.update()

        try:
            if obj.mode == 'EDIT':
                # TODO: we shouldn't do this for meshes that are too complex
                bm = bmesh.from_edit_mesh(obj.data)
                mesh = bpy.data.meshes.new("Modified_Mesh")
                bm.to_mesh(mesh)
            else:
                # Evaluate non-armature modifiers, then copy into an owned mesh.
                obj_evaluated = obj.evaluated_get(dependency_graph)
                evaluated_mesh = obj_evaluated.to_mesh()
                try:
                    mesh = evaluated_mesh.copy()
                    mesh.name = "LiveLink_Mesh"
                finally:
                    obj_evaluated.to_mesh_clear()

            self.add_export_timing(export_stats, "mesh_eval", time.perf_counter() - mesh_eval_start)
            triangulation_start = time.perf_counter()
            if self.mesh_needs_triangulation(mesh):
                bm = bmesh.new()
                bm.from_mesh(mesh)
                bmesh.ops.triangulate(bm, faces=bm.faces[:])
                bm.to_mesh(mesh)
                bm.free()
            self.add_export_timing(export_stats, "triangulation", time.perf_counter() - triangulation_start)
            return mesh
        finally:
            for modifier in disabled_modifiers:
                modifier.show_viewport = True
            if disabled_modifiers:
                bpy.context.view_layer.update()

    def get_mesh_armature(self, in_object):
        if not in_object or in_object.type != 'MESH':
            # Not a mesh, return None
            return None
        
        # Check if the mesh is parented to an armature
        if in_object.parent and in_object.parent.type == 'ARMATURE':
            return in_object.parent
        
        # Check for an Armature Modifier
        for modifier in in_object.modifiers:
            if modifier.type == 'ARMATURE' and modifier.object:
                return modifier.object
    
        # No armature found, return None
        return None

    def iter_action_fcurves(self, action):
        if not action:
            return

        seen = set()

        def yield_fcurve(fcurve):
            fcurve_id = id(fcurve)
            if fcurve_id in seen:
                return
            seen.add(fcurve_id)
            yield fcurve

        try:
            for fcurve in action.fcurves:
                yield from yield_fcurve(fcurve)
        except Exception:
            pass

        # Blender's newer layered Action data can keep fcurves under strips and
        # channel bags instead of the legacy action.fcurves collection.
        for layer in getattr(action, "layers", []):
            for strip in getattr(layer, "strips", []):
                for channelbag in getattr(strip, "channelbags", []):
                    for fcurve in getattr(channelbag, "fcurves", []):
                        yield from yield_fcurve(fcurve)

    def action_targets_armature(self, action, armature_obj):
        if not action:
            return False

        bone_names = {bone.name for bone in armature_obj.data.bones}
        for fcurve in self.iter_action_fcurves(action):
            data_path = fcurve.data_path
            if not data_path.startswith('pose.bones["'):
                continue

            # data_path format is usually pose.bones["Bone"].location, etc.
            parts = data_path.split('"')
            if len(parts) >= 2 and parts[1] in bone_names:
                return True

        return False

    def get_armature_actions(self, armature_obj):
        active_action = None
        if armature_obj.animation_data:
            active_action = armature_obj.animation_data.action

        active_actions = []
        if self.action_targets_armature(active_action, armature_obj):
            active_actions.append(active_action)

        remaining_actions = []
        for action in bpy.data.actions:
            if action == active_action:
                continue
            if self.action_targets_armature(action, armature_obj):
                remaining_actions.append(action)

        remaining_actions.sort(key=lambda action: action.name)
        return active_actions + remaining_actions

    def make_flatbuffer_animation(self, builder, armature_obj, action, bones, export_stats=None):
        scene = bpy.context.scene
        frame_rate = scene.render.fps / scene.render.fps_base
        frame_start = int(np.floor(action.frame_range[0]))
        frame_end = int(np.ceil(action.frame_range[1]))
        frame_count = max(1, frame_end - frame_start + 1)
        bone_count = len(bones)
        if export_stats is not None:
            export_stats["animation_count"] += 1
            export_stats["animation_matrix_count"] += frame_count * bone_count

        old_frame = scene.frame_current
        old_subframe = scene.frame_subframe
        if armature_obj.animation_data is None:
            armature_obj.animation_data_create()
        old_action = armature_obj.animation_data.action

        skin_matrices = np.zeros(frame_count * bone_count * 16, dtype=np.float32)
        animation_sampling_start = time.perf_counter()
        try:
            armature_obj.animation_data.action = action
            for frame_idx, frame in enumerate(range(frame_start, frame_end + 1)):
                scene.frame_set(frame)
                bpy.context.view_layer.update()

                for bone_idx, bone in enumerate(bones):
                    pose_bone = armature_obj.pose.bones.get(bone.name)
                    if pose_bone:
                        skin_matrix = pose_bone.matrix @ bone.matrix_local.inverted()
                    else:
                        skin_matrix = bone.matrix_local @ bone.matrix_local.inverted()

                    matrix_offset = (frame_idx * bone_count + bone_idx) * 16
                    skin_matrices[matrix_offset:matrix_offset + 16] = self.matrix_to_column_major_array(skin_matrix)
        finally:
            armature_obj.animation_data.action = old_action
            scene.frame_set(old_frame, subframe=old_subframe)
            bpy.context.view_layer.update()
            self.add_export_timing(export_stats, "animation_sampling", time.perf_counter() - animation_sampling_start)

        animation_name_fb = builder.CreateString(action.name)
        skin_matrices_fb = builder.CreateNumpyVector(skin_matrices)
        duration_seconds = frame_count / frame_rate if frame_rate > 0.0 else 0.0

        Animation.Start(builder)
        Animation.AddName(builder, animation_name_fb)
        Animation.AddFrameRate(builder, frame_rate)
        Animation.AddDurationSeconds(builder, duration_seconds)
        Animation.AddFrameCount(builder, frame_count)
        Animation.AddBoneCount(builder, bone_count)
        Animation.AddSkinMatrices(builder, skin_matrices_fb)
        return Animation.End(builder)
 
    def make_flatbuffer_object(self, builder, obj, dependency_graph, referenced_materials, export_stats=None):
        # Allocate string for object name
        object_name = builder.CreateString(obj.name)

        # Mesh Data
        mesh_fb = None
        if obj.type == 'MESH': 
            mesh_armature = self.get_mesh_armature(obj)
            mesh = self.get_mesh(obj, dependency_graph, mesh_armature is not None, export_stats)

            vertex_count = len(mesh.vertices)
            loop_count = len(mesh.loops)
            poly_count = len(mesh.polygons)

            # --- Positions and normals per vertex ---
            positions = np.zeros(vertex_count * 3, dtype=np.float32)
            normals   = np.zeros(vertex_count * 3, dtype=np.float32)
            mesh.vertices.foreach_get("co", positions)
            mesh.vertices.foreach_get("normal", normals)
            positions = positions.reshape(vertex_count, 3)
            normals   = normals.reshape(vertex_count, 3)

            # --- Loop -> vertex index mapping ---
            loop_vertex_indices = np.zeros(loop_count, dtype=np.int32)
            mesh.loops.foreach_get("vertex_index", loop_vertex_indices)

            # --- Polygon loop ranges ---
            poly_loop_starts = np.zeros(poly_count, dtype=np.int32)
            poly_loop_totals = np.zeros(poly_count, dtype=np.int32)
            mesh.polygons.foreach_get("loop_start", poly_loop_starts)
            mesh.polygons.foreach_get("loop_total", poly_loop_totals)

            # --- UVs per loop (or zeros if none) ---
            if mesh.uv_layers.active:
                uv_layer = mesh.uv_layers.active.data
                uvs = np.zeros(loop_count * 2, dtype=np.float32)
                uv_layer.foreach_get("uv", uvs)
                uvs = uvs.reshape(loop_count, 2)
            else:
                uvs = np.zeros((loop_count, 2), dtype=np.float32)

            # --- Build unique (vertex, uv) keys ---
            vertex_dedupe_start = time.perf_counter()
            rounded_uvs = (uvs * 1e6).astype(np.int32)  # prevent float issues
            dtype = np.dtype([('v', np.int32), ('u', np.int32), ('v2', np.int32)])
            keys = np.zeros((loop_count,), dtype=dtype)
            keys['v']  = loop_vertex_indices
            keys['u']  = rounded_uvs[:, 0]
            keys['v2'] = rounded_uvs[:, 1]

            unique_keys, inverse_indices = np.unique(keys, return_inverse=True)
            new_indices = inverse_indices.astype(np.int32)
            new_vertex_count = len(unique_keys)
            self.add_export_timing(export_stats, "vertex_dedupe", time.perf_counter() - vertex_dedupe_start)
            if export_stats is not None:
                export_stats["mesh_count"] += 1
                export_stats["mesh_vertex_count"] += int(new_vertex_count)
                export_stats["mesh_index_count"] += int(loop_count)

            # --- Build new vertex buffers ---
            mesh_positions = positions[unique_keys['v']]
            mesh_normals   = normals[unique_keys['v']]
            mesh_uvs       = np.zeros((new_vertex_count, 2), dtype=np.float32)
            mesh_uvs[:, 0] = unique_keys['u'] / 1e6
            mesh_uvs[:, 1] = unique_keys['v2'] / 1e6

            # --- Build face index buffer ---
            indices = np.empty(loop_count, dtype=np.int32)
            for start, total in zip(poly_loop_starts, poly_loop_totals):
                indices[start:start+total] = new_indices[start:start+total]

            # --- Flatten arrays for FlatBuffers ---
            mesh_positions_fb = builder.CreateNumpyVector(mesh_positions.flatten())
            mesh_normals_fb   = builder.CreateNumpyVector(mesh_normals.flatten())
            mesh_uvs_fb       = builder.CreateNumpyVector(mesh_uvs.flatten())
            mesh_indices_fb   = builder.CreateNumpyVector(indices)

            # --- Optional skinning data prep ---
            mesh_joint_indices_fb = None
            mesh_joint_weights_fb = None
            mesh_to_armature_fb = None
            armature_to_mesh_fb = None
            if mesh_armature:
                skin_weights_start = time.perf_counter()
                if export_stats is not None:
                    export_stats["skinned_mesh_count"] += 1
                # Map bone -> index
                bone_index_map = {bone.name: i for i, bone in enumerate(mesh_armature.data.bones)}
                group_names = {vg.index: vg.name for vg in obj.vertex_groups}

                # Temporary arrays (per original vertex)
                joints_per_vert  = np.zeros((vertex_count, 4), dtype=np.int32)
                weights_per_vert = np.zeros((vertex_count, 4), dtype=np.float32)

                # Iterate over vertices and determine bone influences
                for v in mesh.vertices:
                    influences = []
                    for g in v.groups:
                        group_name = group_names.get(g.group)
                        if group_name in bone_index_map:
                            influences.append((bone_index_map[group_name], g.weight))

                    # Sort and take top 4
                    influences.sort(key=lambda x: x[1], reverse=True)
                    top = influences[:4]

                    # Normalize
                    total_w = sum(w for _, w in top)
                    if total_w > 0:
                        top = [(idx, w / total_w) for idx, w in top]

                    # Fill arrays
                    for i, (idx, w) in enumerate(top):
                        joints_per_vert[v.index, i]  = idx
                        weights_per_vert[v.index, i] = w

                # --- Remap into deduplicated vertex buffer ---
                mesh_joint_indices  = joints_per_vert[unique_keys['v']]
                mesh_joint_weights  = weights_per_vert[unique_keys['v']]
                mesh_joint_indices_fb = builder.CreateNumpyVector(mesh_joint_indices.flatten())
                mesh_joint_weights_fb = builder.CreateNumpyVector(mesh_joint_weights.flatten())

                mesh_to_armature_fb = self.make_flatbuffer_matrix(
                    builder,
                    mesh_armature.matrix_world.inverted() @ obj.matrix_world
                )
                armature_to_mesh_fb = self.make_flatbuffer_matrix(
                    builder,
                    obj.matrix_world.inverted() @ mesh_armature.matrix_world
                )
                self.add_export_timing(export_stats, "skin_weights", time.perf_counter() - skin_weights_start)

            # --- Material IDs (optional) ---
            # Get Materials
            material_ids = np.empty(0, dtype=np.int32)
            if obj.data.materials:
                for material in obj.data.materials:
                    if material is None:
                        continue
                    material_id = material.session_uid
                    # append to our material list
                    material_ids = np.append(material_ids, material_id)
                    # Add to referenced material dict
                    if referenced_materials.get(material_id) is None:
                        referenced_materials[material_id] = material
            material_ids_fb = builder.CreateNumpyVector(material_ids)
            if export_stats is not None:
                export_stats["material_slot_count"] += int(len(material_ids))

            # --- Build FlatBuffer Mesh ---
            Mesh.Start(builder)
            Mesh.AddPositions(builder, mesh_positions_fb)
            Mesh.AddNormals(builder, mesh_normals_fb)
            Mesh.AddTexcoords(builder, mesh_uvs_fb)
            Mesh.AddIndices(builder, mesh_indices_fb)
            Mesh.AddMaterialIds(builder, material_ids_fb)
            
            # Optional Skinning Data
            if mesh_joint_indices_fb is not None:
                Mesh.AddJointIndices(builder, mesh_joint_indices_fb)
            if mesh_joint_weights_fb is not None:
                Mesh.AddJointWeights(builder, mesh_joint_weights_fb)

            # Optional armature id
            if mesh_armature is not None:
                Mesh.AddArmatureId(builder, mesh_armature.session_uid)
                Mesh.AddMeshToArmature(builder, mesh_to_armature_fb)
                Mesh.AddArmatureToMesh(builder, armature_to_mesh_fb)
            else:
                Mesh.AddArmatureId(builder, -1)

            mesh_fb = Mesh.End(builder)
            bpy.data.meshes.remove(mesh)

        # Armature Data
        armature_fb = None
        if obj.type == 'ARMATURE':
            print("Found Armature!")
            if export_stats is not None:
                export_stats["armature_count"] += 1
                export_stats["bone_count"] += len(obj.data.bones)
            bone_index_map = {bone.name: i for i, bone in enumerate(obj.data.bones)}
            bones_fb = []
            for bone in obj.data.bones:
                bone_name_fb = builder.CreateString(bone.name)

                bone_parent_name = bone.parent.name if bone.parent else None
                bone_parent_name_fb = builder.CreateString(bone_parent_name) if bone_parent_name else None
                bone_parent_index = bone_index_map.get(bone_parent_name, -1)
                bone_inverse_bind_matrix_fb = self.make_flatbuffer_matrix(builder, bone.matrix_local.inverted())

                print(f"Bone: {bone.name} Parent: {bone_parent_name}")

                Bone.Start(builder)
                Bone.AddName(builder, bone_name_fb)
                if bone_parent_name_fb:
                    Bone.AddParentName(builder, bone_parent_name_fb)
                Bone.AddParentIndex(builder, bone_parent_index)
                Bone.AddInverseBindMatrix(builder, bone_inverse_bind_matrix_fb)
                bones_fb.append(Bone.End(builder))

            # Create flatbuffers vector of bones
            Armature.ArmatureStartBonesVector(builder, len(bones_fb))   
            for bone_fb in reversed(bones_fb):
                builder.PrependUOffsetTRelative(bone_fb)
            armature_bones_fb = builder.EndVector()

            actions = self.get_armature_actions(obj)
            if actions:
                print(f"Armature {obj.name}: exporting {len(actions)} animation(s): {', '.join(action.name for action in actions)}")
            else:
                print(f"Armature {obj.name}: no compatible pose-bone actions found")

            animations_fb = []
            for action in actions:
                animations_fb.append(self.make_flatbuffer_animation(builder, obj, action, obj.data.bones, export_stats))

            Armature.ArmatureStartAnimationsVector(builder, len(animations_fb))
            for animation_fb in reversed(animations_fb):
                builder.PrependUOffsetTRelative(animation_fb)
            armature_animations_fb = builder.EndVector()

            # Add Bones to armature object when creating it
            Armature.Start(builder)
            Armature.AddBones(builder, armature_bones_fb)
            Armature.AddAnimations(builder, armature_animations_fb)
            armature_fb = Armature.End(builder) 

        # Light Info
        light_fb = None
        if obj.type == 'LIGHT':
            if export_stats is not None:
                export_stats["light_count"] += 1
            light_data = obj.data

            Light.Start(builder)

            # Light Color
            light_color = Vec3.CreateVec3(builder, light_data.color.r, light_data.color.g, light_data.color.b)
            Light.AddColor(builder, light_color)

            # Light Type
            light_type_enum = LightType.LightType()
            def determine_light_type(in_light_data):
                match in_light_data.type:
                    case 'POINT': return light_type_enum.Point
                    case 'SPOT' : return light_type_enum.Spot
                    case 'SUN'  : return light_type_enum.Sun
                    case 'AREA' : return light_type_enum.Area
                    case '_': 
                        print("Unsupported Light Type")
                        return []
            
            light_type = determine_light_type(light_data)
            Light.AddType(builder, light_type)

            # Create Data Specific to Light Type
            match light_type:
                case light_type_enum.Point:
                    point_light = PointLight.CreatePointLight(
                        builder, 
                        power = light_data.energy
                    )
                    Light.AddPointLight(builder, point_light)
                case light_type_enum.Spot:
                    spot_light = SpotLight.CreateSpotLight(
                        builder,
                        power = light_data.energy,
                        beamAngle = light_data.spot_size,
                        edgeBlend = light_data.spot_blend
                    )
                    Light.AddSpotLight(builder, spot_light)
                case light_type_enum.Sun:
                    sun_light = SunLight.CreateSunLight(
                        builder,
                        power = light_data.energy,
                        castShadows = light_data.use_shadow
                    )
                    Light.AddSunLight(builder, sun_light)
                case light_type_enum.Area:
                    #TODO:
                    pass

            Light.AddUseShadow(builder, light_data.use_shadow)

            light_fb = Light.End(builder)

        # Add Object Gameplay Components
        gameplay_components = builder_create_gameplay_components(builder, obj.live_link_settings)
        
        # Begin New Object 
        Object.Start(builder)
        
        # Object Name
        Object.AddName(builder, object_name)

        # Session UID (note that this is a fairly new addition to the python API)
        session_uid = obj.session_uid
        Object.AddUniqueId(builder, session_uid)

        # Check object visibility flag
        is_visible = obj.visible_get()
        Object.AddVisibility(builder, is_visible)

        # Get world-space location, rotation, and scale
        obj_matrix_world = obj.matrix_world
        obj_location, obj_rotation, obj_scale = obj_matrix_world.decompose()

        # Object Location
        location_vec3 = Vec3.CreateVec3(builder, obj_location.x, obj_location.y, obj_location.z)
        Object.AddLocation(builder, location_vec3)

        # Object Scale
        scale_vec3 = Vec3.CreateVec3(builder, obj_scale.x, obj_scale.y, obj_scale.z)
        Object.AddScale(builder, scale_vec3)

        # Object Rotation
        rotation_quat = Quat.CreateQuat(builder, obj_rotation.x, obj_rotation.y, obj_rotation.z, obj_rotation.w)
        Object.AddRotation(builder, rotation_quat)

        # Add Object Mesh Data if it exists
        if mesh_fb is not None:
            Object.AddMesh(builder, mesh_fb)

        # Add Object Armature Data if it exists
        if armature_fb is not None:
            Object.AddArmature(builder, armature_fb)

        # Add Object Light Data if it exists
        if light_fb is not None:
            Object.AddLight(builder, light_fb)

        # Add Rigid Body Data if it exists
        if obj.rigid_body:
            Object.AddRigidBody(builder, RigidBody.CreateRigidBody(
                builder, 
                isDynamic = obj.rigid_body.enabled,
                mass = obj.rigid_body.mass
            ))

        # Add Gameplay Components data if it exists
        if gameplay_components is not None:
            Object.AddComponents(builder, gameplay_components)
            
        # End New Object add add to array
        live_link_object = Object.End(builder)

        return live_link_object

    # Creates an update for objects in in_object_list
    def make_update(self, in_object_list, in_deleted_object_uids, reset=False, update_reason="unknown"):
        export_generation_start = time.perf_counter()
        self.update_sequence += 1

        # Evaluate Depsgraph
        dependency_graph = bpy.context.evaluated_depsgraph_get()

        # init flatbuffers builder
        builder = flatbuffers.Builder(0)
        export_stats = {
            "sequence": self.update_sequence,
            "reason": update_reason,
            "input_object_count": len(in_object_list),
            "deleted_object_count": len(in_deleted_object_uids),
            "exported_object_count": 0,
            "mesh_count": 0,
            "mesh_vertex_count": 0,
            "mesh_index_count": 0,
            "skinned_mesh_count": 0,
            "light_count": 0,
            "armature_count": 0,
            "bone_count": 0,
            "animation_count": 0,
            "animation_matrix_count": 0,
            "material_slot_count": 0,
            "material_count": 0,
            "image_count": 0,
            "image_byte_count": 0,
            "byte_count": 0,
            "generation_seconds": 0.0,
            "timings": {
                "mesh_eval": 0.0,
                "triangulation": 0.0,
                "vertex_dedupe": 0.0,
                "skin_weights": 0.0,
                "animation_sampling": 0.0,
                "materials": 0.0,
                "images": 0.0,
                "flatbuffer_finish": 0.0,
            },
            "reset": reset,
        }

        # referenced materials, keyed by session_uid and updated in self.make_flatbuffer_object
        referenced_materials = {}

        # Build up objects to be added to scene objects vector. A live-linked
        # skinned mesh needs its armature data even when the armature object is
        # hidden or not explicitly live-linked in Blender.
        objects_to_export = []
        exported_object_ids = set()

        def queue_export_object(blender_object):
            if not blender_object:
                return
            object_id = blender_object.session_uid
            if object_id in exported_object_ids:
                return
            exported_object_ids.add(object_id)
            objects_to_export.append(blender_object)

        def armature_is_referenced_by_live_linked_mesh(armature_object):
            for scene_object in bpy.context.scene.objects:
                if scene_object.type != 'MESH':
                    continue
                if not scene_object.live_link_settings.enable_live_link:
                    continue
                if self.get_mesh_armature(scene_object) == armature_object:
                    return True
            return False

        for blender_object in in_object_list: 
            if blender_object.live_link_settings.enable_live_link:
                queue_export_object(blender_object)

                if blender_object.type == 'MESH':
                    mesh_armature = self.get_mesh_armature(blender_object)
                    if mesh_armature:
                        queue_export_object(mesh_armature)
            elif blender_object.type == 'ARMATURE' and armature_is_referenced_by_live_linked_mesh(blender_object):
                queue_export_object(blender_object)

        live_link_objects = []
        export_stats["exported_object_count"] = len(objects_to_export)
        for blender_object in objects_to_export:
            live_link_objects.append(
                self.make_flatbuffer_object(
                    builder,
                    blender_object,
                    dependency_graph,
                    referenced_materials,
                    export_stats
                )
            )

        # actually create the scene objects vector
        Update.UpdateStartObjectsVector(builder, len(live_link_objects))
        for live_link_object in live_link_objects: 
            builder.PrependUOffsetTRelative(live_link_object)
        update_objects = builder.EndVector()

        # create flatbuffers deleted objects
        Update.UpdateStartDeletedObjectUidsVector(builder, len(in_deleted_object_uids))
        for deleted_object_uid in in_deleted_object_uids:
            builder.PrependInt32(deleted_object_uid)
        update_deleted_object_uids = builder.EndVector()

        # referenced images, keyed by session_uid and updated when building up material list below 
        referenced_images = {}

        # create flatbuffers materials
        materials_start = time.perf_counter()
        flatbuffer_materials = []
        export_stats["material_count"] = len(referenced_materials)
        for material_id, material in referenced_materials.items():
            class MaterialData:
                def __init__(self):
                    self.base_color = (1.0,1.0,1.0,1.0)
                    self.base_color_image_id = None 
                    self.metallic = 0.0
                    self.metallic_image_id = None
                    self.roughness = 0.0
                    self.roughness_image_id = None
                    self.emission_color = (0.0,0.0,0.0,0.0)
                    self.emission_color_image_id = None
                    self.emission_strength = 0.0



            # Helper to register an image id for a material_node_input if it contains a valid image
            def extract_image_id(material_node_input):
                # Need to actually be linked to an image to extract it
                if not material_node_input or not material_node_input.is_linked:
                    return None

                # Take the first link only
                link = material_node_input.links[0]
                from_node = link.from_node

                # Check for Image Texture node
                if from_node.type != 'TEX_IMAGE' or not from_node.image:
                    return None

                image = from_node.image
                image_id = image.session_uid
                referenced_images[image_id] = image
                return image_id

            # Init material data
            material_data = MaterialData()

            # Add Material Properties to material data
            if material.use_nodes:
                # require "Principled BSDF" root node to grab relevant PBR data
                bsdf = material.node_tree.nodes.get("Principled BSDF")
                if bsdf:
                    # Base Color
                    base_color_input = bsdf.inputs["Base Color"]
                    material_data.base_color = base_color_input.default_value
                    material_data.base_color_image_id = extract_image_id(base_color_input)
                    # Metallic 
                    metallic_input = bsdf.inputs["Metallic"]
                    material_data.metallic = metallic_input.default_value
                    material_data.metallic_image_id = extract_image_id(metallic_input)
                    # Roughness 
                    roughness_input = bsdf.inputs["Roughness"]
                    material_data.roughness = roughness_input.default_value
                    material_data.roughness_image_id = extract_image_id(roughness_input)
                    # Emissive
                    emission_color_input = bsdf.inputs["Emission Color"]
                    emission_strength_input = bsdf.inputs["Emission Strength"]
                    material_data.emission_color = emission_color_input.default_value
                    material_data.emission_color_image_id = extract_image_id(emission_color_input)
                    material_data.emission_strength = emission_strength_input.default_value

            # End current flatbuffers Material and add to list

            # Need to create Material Name String before starting material table
            material_name = builder.CreateString(material.name_full)

            # Begin new flatbuffers Material 
            Material.Start(builder)

            # Set material unique id
            Material.AddUniqueId(builder, material_id)

            # Set material name
            Material.AddName(builder, material_name)
            
            # Base Color
            base_color_vec4 = Vec4.CreateVec4(builder, *material_data.base_color)
            Material.AddBaseColor(builder, base_color_vec4)
            if material_data.base_color_image_id is not None:
                Material.AddBaseColorImageId(builder, material_data.base_color_image_id)

            # Metallic
            Material.AddMetallic(builder, material_data.metallic)
            if material_data.metallic_image_id is not None:
                Material.AddMetallicImageId(builder, material_data.metallic_image_id)

            # Roughness
            Material.AddRoughness(builder, material_data.roughness)
            if material_data.roughness_image_id is not None:
                Material.AddRoughnessImageId(builder, material_data.roughness_image_id)

            # Emission
            emission_color_vec4 = Vec4.CreateVec4(builder, *material_data.emission_color)
            Material.AddEmissionColor(builder, emission_color_vec4)
            if material_data.emission_color_image_id is not None:
                Material.AddEmissionColorImageId(builder, material_data.emission_color_image_id)
            Material.AddEmissionStrength(builder, material_data.emission_strength)

            flatbuffer_material = Material.End(builder)
            flatbuffer_materials.append(flatbuffer_material)

        # Create the vector of materials for our top-level Update 
        Update.UpdateStartMaterialsVector(builder, len(flatbuffer_materials))   
        for material in reversed(flatbuffer_materials):
            builder.PrependUOffsetTRelative(material)
        update_materials = builder.EndVector()
        self.add_export_timing(export_stats, "materials", time.perf_counter() - materials_start)

        images_start = time.perf_counter()
        flatbuffer_images = []
        for image_id, image in referenced_images.items():
            
            # Get image width and height
            image_width, image_height = image.size

            # Ensure pixels are loaded
            _ = image.pixels[0]

            # Get float32 pixel array (RGBA)
            pixels_f32 = np.array(image.pixels[:], dtype=np.float32)

            # Reinterpret the float32 array as uint8 bytes
            pixels_bytes = pixels_f32.view(np.uint8)
            export_stats["image_count"] += 1
            export_stats["image_byte_count"] += int(pixels_bytes.nbytes)

            # Now pass to FlatBuffers
            flatbuffer_image_data = builder.CreateNumpyVector(pixels_bytes)

            # Build up flatbuffers image and add it to our list of images
            Image.Start(builder)
            Image.AddUniqueId(builder, image_id)
            Image.AddWidth(builder, image_width)
            Image.AddHeight(builder, image_height)
            Image.AddData(builder, flatbuffer_image_data) 
            flatbuffer_image = Image.End(builder)
            flatbuffer_images.append(flatbuffer_image)

        # Create the vector of images for our top-level Update 
        Update.UpdateStartImagesVector(builder, len(flatbuffer_images))   
        for image in reversed(flatbuffer_images):
            builder.PrependUOffsetTRelative(image)
        update_images = builder.EndVector()            
        self.add_export_timing(export_stats, "images", time.perf_counter() - images_start)

        flatbuffer_finish_start = time.perf_counter()
        # Begin writing top-level update table
        Update.Start(builder)

        # Add objects vector to scene
        Update.AddObjects(builder, update_objects)
        Update.AddDeletedObjectUids(builder, update_deleted_object_uids)
        Update.AddMaterials(builder, update_materials)
        Update.AddImages(builder, update_images)
        Update.AddReset(builder, reset)
        export_stats["generation_seconds"] = time.perf_counter() - export_generation_start
        Update.AddGenerationSeconds(builder, export_stats["generation_seconds"])

        # finalize scene flatbuffer
        live_link_scene = Update.End(builder)

        # finish and provide size information
        builder.FinishSizePrefixed(live_link_scene)
        
        # return flatbuffers binary output
        output = builder.Output()
        export_stats["byte_count"] = len(output)
        self.add_export_timing(export_stats, "flatbuffer_finish", time.perf_counter() - flatbuffer_finish_start)
        timing_stats = export_stats["timings"]
        print(
            "\nLive Link Export Stats: "
            f"seq={export_stats['sequence']} "
            f"reason={export_stats['reason']} "
            f"bytes={export_stats['byte_count']} "
            f"input_objects={export_stats['input_object_count']} "
            f"exported_objects={export_stats['exported_object_count']} "
            f"deleted={export_stats['deleted_object_count']} "
            f"meshes={export_stats['mesh_count']} "
            f"verts={export_stats['mesh_vertex_count']} "
            f"indices={export_stats['mesh_index_count']} "
            f"skinned={export_stats['skinned_mesh_count']} "
            f"lights={export_stats['light_count']} "
            f"armatures={export_stats['armature_count']} "
            f"bones={export_stats['bone_count']} "
            f"animations={export_stats['animation_count']} "
            f"animation_matrices={export_stats['animation_matrix_count']} "
            f"material_slots={export_stats['material_slot_count']} "
            f"materials={export_stats['material_count']} "
            f"images={export_stats['image_count']} "
            f"image_bytes={export_stats['image_byte_count']} "
            f"generation_seconds={export_stats['generation_seconds']:.6f} "
            f"reset={export_stats['reset']}"
        )
        print(
            "Live Link Export Timings: "
            f"seq={export_stats['sequence']} "
            f"mesh_eval={timing_stats['mesh_eval']:.6f}s "
            f"triangulation={timing_stats['triangulation']:.6f}s "
            f"vertex_dedupe={timing_stats['vertex_dedupe']:.6f}s "
            f"skin_weights={timing_stats['skin_weights']:.6f}s "
            f"animation_sampling={timing_stats['animation_sampling']:.6f}s "
            f"materials={timing_stats['materials']:.6f}s "
            f"images={timing_stats['images']:.6f}s "
            f"flatbuffer_finish={timing_stats['flatbuffer_finish']:.6f}s"
        )
        return output

    def send_object_list(self, updated_objects, deleted_object_uids, update_reason="object_list"):
        self.send(self.make_update(updated_objects, deleted_object_uids, update_reason=update_reason))

    def save_to_file(self, in_objects, in_filename, update_reason="save_to_file"):
        update = self.make_update(in_objects, [], update_reason=update_reason)
        with open(in_filename, 'wb') as f:
            f.write(update)

    def send_reset(self, update_reason="manual_reset"):
        self.send(self.make_update([], [], True, update_reason=update_reason))

live_link_connection = []

batched_updates = set()
batched_deleted = set()

def clear_batched_depsgraph_updates(update_reason="unknown"):
    if bpy.app.timers.is_registered(send_updates_timer):
        bpy.app.timers.unregister(send_updates_timer)

    if batched_updates or batched_deleted:
        print(
            "\nLive Link Clear Queued Depsgraph Updates: "
            f"reason={update_reason} "
            f"queued_updates={len(batched_updates)} "
            f"queued_deleted={len(batched_deleted)}"
        )
        batched_updates.clear()
        batched_deleted.clear()

# Actually sends batched updates
def send_updates_timer(): 
    global batched_updates, batched_deleted 

    # No new updates in SEND_DELAY seconds → send batched data
    if batched_updates or batched_deleted:
        update_reason = f"depsgraph_timer(updates={len(batched_updates)},deleted={len(batched_deleted)})"
        print(f"\nLive Link Timer Send: reason={update_reason}")

        depsgraph_update_post_callback.enabled = False

        live_link_connection.send_object_list(
            updated_objects=list(batched_updates),
            deleted_object_uids=list(batched_deleted),
            update_reason=update_reason,
        )

        depsgraph_update_post_callback.enabled = True

        # Clear batch
        batched_updates.clear()
        batched_deleted.clear()

    return None

# Schedules send but doesn't actually send the objects to the game
def schedule_send(update_reason="depsgraph_update"):
    # unregister timer if currently active:
    if bpy.app.timers.is_registered(send_updates_timer):
        bpy.app.timers.unregister(send_updates_timer)
    # Schedule new timer
    SEND_DELAY = 0.25
    bpy.app.timers.register(send_updates_timer, first_interval=SEND_DELAY)
    if batched_updates or batched_deleted:
        print(
            "\nLive Link Schedule Send: "
            f"reason={update_reason} "
            f"queued_updates={len(batched_updates)} "
            f"queued_deleted={len(batched_deleted)}"
        )

# Callback when depsgraph has finished updating
@persistent
def depsgraph_update_post_callback(scene, depsgraph):
    if not depsgraph_update_post_callback.enabled:
        return

    updated_objects = []

    # Determine if any objects were deleted
    current_objects = set(obj.session_uid for obj in scene.objects)

    # Track the objects at the last update
    if hasattr(depsgraph_update_post_callback, "previous_objects"):
        previous_objects = depsgraph_update_post_callback.previous_objects
        # Find the difference (deleted objects)
        deleted_object_uids = []
        deleted_object_uids = list(previous_objects - current_objects)
        batched_deleted.update(deleted_object_uids)
    
    # Store the current object names for the next update
    depsgraph_update_post_callback.previous_objects = current_objects

    # Accumulate updated objects
    for update in depsgraph.updates:
        update_id = update.id
        if isinstance(update_id, bpy.types.Object):
            if update.is_updated_transform or update.is_updated_geometry:
                batched_updates.add(scene.objects[update_id.name])

    # Start/refresh the timer
    schedule_send(update_reason="depsgraph_update_post")

# Enable depsgraph_update_post_callback. Will be disabled to prevent recursion within depsgraph_update_post_callback
depsgraph_update_post_callback.enabled = True

# Begin OpLiveLinkSendFullUpdate
class OpLiveLinkSendFullUpdate(bpy.types.Operator):
    """Live Link: Send Full Update """
    bl_idname = "live_link.send_full_update"
    bl_label = "Live Link: Send Full Update"
    bl_options = {'REGISTER'} 

    # Called when operator is run
    def execute(self, context):
        clear_batched_depsgraph_updates(update_reason="manual_full_update_before_send")
        print(
            "\nLive Link Manual Send Requested: "
            f"reason=manual_full_update "
            f"scene_objects={len(bpy.context.scene.objects)} "
            f"queued_depsgraph_updates={len(batched_updates)} "
            f"queued_deleted={len(batched_deleted)}"
        )
        depsgraph_update_post_callback.enabled = False
        try:
            live_link_connection.send_object_list(
                updated_objects = list(bpy.context.scene.objects),
                deleted_object_uids = [],
                update_reason = "manual_full_update",
            )
        finally:
            clear_batched_depsgraph_updates(update_reason="manual_full_update_after_send")
            depsgraph_update_post_callback.enabled = True
        return {'FINISHED'}
# End OpLiveLinkSendFullUpdate 

# Begin OpLiveLinkSendReset
class OpLiveLinkSendReset(bpy.types.Operator):
    """Live Link: Send Reset """
    bl_idname = "live_link.send_reset"
    bl_label = "Live Link: Send Reset"
    bl_options = {'REGISTER'} 

    # Called when operator is run
    def execute(self, context):
        live_link_connection.send_reset(update_reason="manual_reset")
        return {'FINISHED'}
# End OpLiveLinkSendReset

# Begin OpLiveLinkResetConnection
class OpLiveLinkResetConnection(bpy.types.Operator):
    """Live Link: Reset Connection """
    bl_idname = "live_link.reset_connection"
    bl_label = "Live Link: Reset Connection"
    bl_options = {'REGISTER'} 

    # Called when operator is run
    def execute(self, context): 
        #global live_link_connection  
        live_link_connection = LiveLinkConnection()
        return {'FINISHED'}
# End OpLiveLinkResetConnection

# Begin OpLiveLinkSaveToFile
class OpLiveLinkSaveToFile(bpy.types.Operator):
    bl_idname = "live_link.save_to_file"
    bl_label = "Live Link: Save To File"

    filepath: bpy.props.StringProperty(subtype="FILE_PATH")

    def execute(self, context):
        print("Selected file path:", self.filepath)
        live_link_connection.save_to_file(
            list(bpy.context.scene.objects),
            self.filepath,
            update_reason="manual_save_to_file",
        )
        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

# Begin LiveLinkView3DPanel
class LiveLinkView3DPanel(bpy.types.Panel):
    bl_idname = "OBJECT_PT_LiveLink_View3D_Panel"
    bl_label = "Blender Live Link"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'BlenderLiveLink'
    
    def draw(self, context):
        layout = self.layout
        scene = context.scene
        layout.operator("live_link.send_full_update", text="Full Update")  
        layout.operator("live_link.send_reset", text="Send Reset")  
        layout.operator("live_link.reset_connection", text="Reset Connection")
        layout.operator("live_link.save_to_file", text="Save To File")
# End LiveLinkView3DPanel

def menu_func(self, context):
    self.layout.operator(OpLiveLinkSendFullUpdate.bl_idname)
    self.layout.operator(OpLiveLinkSendReset.bl_idname)
    self.layout.operator(OpLiveLinkResetConnection.bl_idname)

# ------------------------------------------------------------
# Define Gameplay Components 
# ------------------------------------------------------------

class Component(PropertyGroup):
    # Blender UI Info
    type_name = 'INVALID'
    label = 'INVALID'

    @classmethod
    def enum_info(cls):
        return (cls.type_name, cls.label, '')

    # Adds component to flatbuffers component list
    def create_flatbuffers_object(self, builder):
        # Functions to generate value_type and value (implemneted by child classes) 
        value_type = self.get_flatbuffers_value_type()
        value = self.create_flatbuffers_value(builder)

        # Create the container that contains our union and return that
        GameplayComponentContainer.Start(builder)
        if value is not None:
            GameplayComponentContainer.AddValueType(builder, value_type)
            GameplayComponentContainer.AddValue(builder, value)
        return GameplayComponentContainer.End(builder)

    def create_flatbuffers_value(self, builder):
        return None

    def get_flatbuffers_value_type(self):
        return None

class Component_Character(Component):
    # Blender UI Info
    type_name = 'CHARACTER'
    label = 'Character'

    # Properties
    player_controlled: BoolProperty(name="Player Controlled", default=False)
    move_speed: FloatProperty(name="Move Speed", default=20.0)
    jump_speed: FloatProperty(name="Jump Speed", default=10.0)

    # Adds component to flatbuffers component list
    def create_flatbuffers_value(self, builder):
        GameplayComponentCharacter.Start(builder)
        GameplayComponentCharacter.AddPlayerControlled(builder, self.player_controlled)
        GameplayComponentCharacter.AddMoveSpeed(builder, self.move_speed)
        GameplayComponentCharacter.AddJumpSpeed(builder, self.jump_speed)
        return GameplayComponentCharacter.End(builder)

    def get_flatbuffers_value_type(self):
        return GameplayComponent.GameplayComponent().GameplayComponentCharacter

class Component_CameraControl(Component):
    # Blender UI Info
    type_name = 'CAMERA_CONTROL'
    label = 'Camera Control'

    # Properties
    follow_distance: FloatProperty(name="Follow Distance", default=5.0)
    follow_speed: FloatProperty(name="Follow Speed", default=10.0)

    # Adds component to flatbuffers component list
    def create_flatbuffers_value(self, builder):
        GameplayComponentCameraControl.Start(builder)
        GameplayComponentCameraControl.AddFollowDistance(builder, self.follow_distance)
        GameplayComponentCameraControl.AddFollowSpeed(builder, self.follow_speed)
        return GameplayComponentCameraControl.End(builder)

    def get_flatbuffers_value_type(self):
        return GameplayComponent.GameplayComponent().GameplayComponentCameraControl

gameplay_component_enum = [
    Component_Character.enum_info(),
    Component_CameraControl.enum_info(),
]

TYPE_TO_GROUP = {
    Component_Character.type_name: 'player',
    Component_CameraControl.type_name: 'camera_control'
}

#GROUP_TO_TYPE = {v: k for k, v in TYPE_TO_GROUP.items()}

# ------------------------------------------------------------
# Container for polymorphic data
# ------------------------------------------------------------

class ComponentContainer(PropertyGroup):
    type: StringProperty()

    # Only one of these should be set, based on type
    player:         PointerProperty(type=Component_Character)
    camera_control: PointerProperty(type=Component_CameraControl)

    # Simply forwards to relevant component data to create flatbuffer object
    def create_flatbuffers_object(self, builder):
       component_data = getattr(self, TYPE_TO_GROUP[self.type])
       return component_data.create_flatbuffers_object(builder)

# ------------------------------------------------------------
# Property Group for Live-Link Specific Data 
# ------------------------------------------------------------

class LiveLinkObjectSettings(bpy.types.PropertyGroup):
    enable_live_link: bpy.props.BoolProperty(name="Enable Live Link", default=True)

    add_type: EnumProperty(
        name="Add Type",
        description="Type of property to add",
        items=gameplay_component_enum
    )

    components: CollectionProperty(type=ComponentContainer)

#  Creates the flatbuffer array for the components under an object's live_link_settings
def builder_create_gameplay_components(builder, live_link_settings):
    out_flatbuffers_object = None

    if len(live_link_settings.components) > 0:
        flatbuffer_components = []
        for component in live_link_settings.components:
            flatbuffer_components.append(component.create_flatbuffers_object(builder))

        Object.ObjectStartComponentsVector(builder, len(flatbuffer_components))
        for flatbuffer_component in reversed(flatbuffer_components): 
            builder.PrependUOffsetTRelative(flatbuffer_component)
        out_flatbuffers_object = builder.EndVector()
    
    return out_flatbuffers_object

# ------------------------------------------------------------
# Operators to add/remove selected type
# ------------------------------------------------------------

class OBJECT_OT_add_custom_item(Operator):
    bl_idname = "object.add_custom_item"
    bl_label = "Add Custom Property Group"

    def execute(self, context):
        obj = context.object
        settings = obj.live_link_settings

        new_component = settings.components.add()
        new_component.type = settings.add_type

        # Initialize based on type
        if new_component.type == Component_Character.type_name:
            new_component.player.move_speed = 20.0
        elif new_component.type == Component_CameraControl.type_name:
            new_component.camera_control.follow_distance = 100.0

        return {'FINISHED'}

class OBJECT_OT_remove_custom_item(Operator):
    bl_idname = "object.remove_custom_item"
    bl_label = "Remove Custom Property Group"

    index: bpy.props.IntProperty()

    def execute(self, context):
        obj = context.object
        settings = obj.live_link_settings

        if 0 <= self.index < len(settings.components):
            settings.components.remove(self.index)
            return {'FINISHED'}
        else:
            self.report({'WARNING'}, "Invalid index")
            return {'CANCELLED'}

# ------------------------------------------------------------
# Utility function to auto-draw groups
# ------------------------------------------------------------

def draw_property_group(layout, group):
    for prop in group.bl_rna.properties:
        if prop.identifier != "rna_type":
            layout.prop(group, prop.identifier)

# ------------------------------------------------------------
# Panel UI
# ------------------------------------------------------------

class OBJECT_PT_custom_object_panel(Panel):
    bl_label = "Live Link Properties"
    bl_idname = "OBJECT_PT_custom_object_panel"
    bl_space_type = 'PROPERTIES'  # ← This is key
    bl_region_type = 'WINDOW'
    bl_context = "object"         # ← This puts it in the Object tab
    bl_options = {'DEFAULT_CLOSED'}  # Optional: collapsed by default
    bl_icon = 'MODIFIER'          # ← Custom icon (Blender built-in icons)

    def draw(self, context):
        layout = self.layout
        obj = context.object

        settings = obj.live_link_settings
        layout.prop(settings, "enable_live_link")
        layout.prop(settings, "add_type")
        layout.operator("object.add_custom_item", text="Add Component")

        for i, component in enumerate(settings.components):
            box = layout.box()
            row = box.row()
            row.label(text=f"Component {i+1} ({component.type})", icon='DOT')
            row.operator("object.remove_custom_item", text="", icon="X").index = i

            group_name = TYPE_TO_GROUP.get(component.type)
            if group_name:
                group = getattr(component, group_name, None)
                if group:
                    draw_property_group(box, group)


# ------------------------------------------------------------
#  Classes we register with blender
# ------------------------------------------------------------

classes_to_register = [ 
    # Main Live Link Operators
    OpLiveLinkSendFullUpdate,
    OpLiveLinkSendReset,
    OpLiveLinkResetConnection,
    OpLiveLinkSaveToFile,

    # View 3D Panel
    LiveLinkView3DPanel,

    # Custom Property Group System
    Component_Character,
    Component_CameraControl,
    ComponentContainer,
    LiveLinkObjectSettings,
    OBJECT_OT_add_custom_item,
    OBJECT_OT_remove_custom_item,
    OBJECT_PT_custom_object_panel,
]

# ------------------------------------------------------------
# Blender Extension Register/Unregister functions
# ------------------------------------------------------------
def register():
    # init live link connection
    global live_link_connection  
    live_link_connection = LiveLinkConnection()

    # Register classes
    for cls in classes_to_register:
        bpy.utils.register_class(cls)

    # Register depsgraph update post callback
    bpy.app.handlers.depsgraph_update_post.append(depsgraph_update_post_callback)

    # add to searchable menu
    bpy.types.VIEW3D_MT_object.append(menu_func)

    # Setup live link settings on type Object
    bpy.types.Object.live_link_settings = bpy.props.PointerProperty(type=LiveLinkObjectSettings)

def unregister():
    # clean up live link connection
    global live_link_connection
    del live_link_connection

    # Unregister classes
    for cls in reversed(classes_to_register):
        bpy.utils.unregister_class(cls)

    # Remove depsgraph update post callback
    bpy.app.handlers.depsgraph_update_post.remove(depsgraph_update_post_callback)

    # remove from searchable menu
    bpy.types.VIEW3D_MT_object.remove(menu_func)

    # Delete Live Link Settings
    del bpy.types.Object.live_link_settings

# This allows you to run the script directly from Blender's Text editor
if __name__ == "__main__":
    register()
