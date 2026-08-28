try:
    import cython
    if cython.compiled:
        print("Running as compiled cython module.")
    else:
        print("Running as regular python module.")
except ImportError:
    print("Failed to import cython.")

import bpy
from bpy.props import (BoolProperty, StringProperty, FloatProperty, FloatVectorProperty, IntProperty, PointerProperty, CollectionProperty, EnumProperty)
from bpy.types import (Panel, Operator, PropertyGroup)
from bpy.app.handlers import persistent

import bmesh
import builtins
import math
import numpy as np
import socket
import struct
import traceback
import time
import zlib

from contextlib import contextmanager, nullcontext
from dataclasses import dataclass, field
from io import StringIO
from mathutils import Matrix as MathMatrix, Vector

# ignore SIGPIPE so that writing to a closed socket raises a Python exception
import signal
try:
    signal.signal(signal.SIGPIPE, signal.SIG_IGN)
except Exception:
    # Not all platforms expose SIGPIPE (Windows), ignore failures
    pass

from .compiled_schemas.python import flatbuffers
from .compiled_schemas.python.Blender.LiveLink import Armature
from .compiled_schemas.python.Blender.LiveLink import Animation
from .compiled_schemas.python.Blender.LiveLink import AttachmentBindingType
from .compiled_schemas.python.Blender.LiveLink import Bone 
from .compiled_schemas.python.Blender.LiveLink import EditorCamera
from .compiled_schemas.python.Blender.LiveLink import GameplayComponent
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentAttachmentPoint
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentCameraControl
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentCharacter
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentContainer
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentFogController
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentSkyAtmosphere
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentCloudSystem
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentPart
from .compiled_schemas.python.Blender.LiveLink import CloudLayer
from .compiled_schemas.python.Blender.LiveLink import CloudLayerProfile
from .compiled_schemas.python.Blender.LiveLink import Image
from .compiled_schemas.python.Blender.LiveLink import Light
from .compiled_schemas.python.Blender.LiveLink import LightType
from .compiled_schemas.python.Blender.LiveLink import Material
from .compiled_schemas.python.Blender.LiveLink import Matrix
from .compiled_schemas.python.Blender.LiveLink import Mesh
from .compiled_schemas.python.Blender.LiveLink import Object
from .compiled_schemas.python.Blender.LiveLink import PointLight
from .compiled_schemas.python.Blender.LiveLink import PartType
from .compiled_schemas.python.Blender.LiveLink import Quat
from .compiled_schemas.python.Blender.LiveLink import RigidBody 
from .compiled_schemas.python.Blender.LiveLink import SpotLight
from .compiled_schemas.python.Blender.LiveLink import SunLight
from .compiled_schemas.python.Blender.LiveLink import Update
from .compiled_schemas.python.Blender.LiveLink import Vec2
from .compiled_schemas.python.Blender.LiveLink import Vec3
from .compiled_schemas.python.Blender.LiveLink import Vec4

EARTH_TOA_SOLAR_IRRADIANCE_W_M2 = 1361.0

EXPORT_TIMING_KEYS = (
    "mesh_eval",
    "triangulation",
    "vertex_dedupe",
    "skin_weights",
    "animation_sampling",
    "materials",
    "images",
    "image_pixels_read",
    "image_rgba8_convert",
    "image_flatbuffer_pack",
    "flatbuffer_finish",
)

INSTANCE_UID_MIN = 0x40000000
INSTANCE_UID_MAX = 0x7FFFFFFF

# Datablocks belonging to the temporary collection-evaluation scene. Their
# creation and teardown must never be mistaken for authored scene changes.
EVALUATION_SCENE_PREFIX = "__LiveLinkEvaluation__"
# Blender uses session_uid 0 for "unset"; no real datablock ever has it.
UNSET_SESSION_UID = 0


@dataclass
class ExportOccurrence:
    """One authored object as it appears in the exported scene."""

    source_object: object
    evaluation_object: object
    unique_id: int
    name: str
    matrix_world: object
    visibility: bool
    dependency_graph: object
    reference_uid_map: dict = field(default_factory=dict)
    occurrence_key: tuple = field(default_factory=tuple)
    instancer_path: tuple = field(default_factory=tuple)
    dependency_ids: frozenset = field(default_factory=frozenset)

    @property
    def matrix_world_values(self):
        return tuple(float(self.matrix_world[row][column]) for row in range(4) for column in range(4))

    def remap_uid(self, blender_object):
        if blender_object is None:
            return -1
        source_uid = blender_object.session_uid
        return self.reference_uid_map.get(source_uid, source_uid)


@dataclass(frozen=True)
class OccurrenceSnapshot:
    unique_id: int
    matrix_world_values: tuple
    visibility: bool
    dependency_ids: frozenset
    instancer_path: tuple

    @classmethod
    def from_occurrence(cls, occurrence):
        return cls(
            unique_id=occurrence.unique_id,
            matrix_world_values=occurrence.matrix_world_values,
            visibility=occurrence.visibility,
            dependency_ids=occurrence.dependency_ids,
            instancer_path=occurrence.instancer_path,
        )


class InstanceUidRegistry:
    """Stable positive int32 IDs for collection-expanded object occurrences."""

    def __init__(self):
        self.key_to_uid = {}
        self.uid_to_key = {}
        self.retired_uids = set()

    def clear(self):
        self.key_to_uid.clear()
        self.uid_to_key.clear()
        self.retired_uids.clear()

    @staticmethod
    def candidate_for_key(key):
        payload = b''.join(struct.pack('<I', int(value) & 0xFFFFFFFF) for value in key)
        return INSTANCE_UID_MIN | (zlib.crc32(payload) & (INSTANCE_UID_MIN - 1))

    @staticmethod
    def next_candidate(candidate):
        return INSTANCE_UID_MIN if candidate >= INSTANCE_UID_MAX else candidate + 1

    def prepare(self, keys, reserved_uids):
        reserved_uids = {int(uid) for uid in reserved_uids}

        # A direct Blender UID always wins. Reallocate an old occurrence if a
        # newly-created direct object claims its synthetic slot.
        for key, uid in list(self.key_to_uid.items()):
            if uid not in reserved_uids:
                continue
            self.retired_uids.add(uid)
            del self.key_to_uid[key]
            self.uid_to_key.pop(uid, None)

        for key in sorted(set(keys)):
            if key in self.key_to_uid:
                continue
            candidate = self.candidate_for_key(key)
            while candidate in reserved_uids or candidate in self.uid_to_key:
                candidate = self.next_candidate(candidate)
            self.key_to_uid[key] = candidate
            self.uid_to_key[candidate] = key

    def uid_for_key(self, key):
        return self.key_to_uid[key]

    def take_retired_uids(self):
        retired = set(self.retired_uids)
        self.retired_uids.clear()
        return retired


class ExportStats(dict):
    """Counters and coarse timings for one Python export."""

    def __init__(self, sequence, reason, input_object_count, deleted_object_count, reset):
        super().__init__(
            sequence=sequence,
            reason=reason,
            input_object_count=input_object_count,
            deleted_object_count=deleted_object_count,
            exported_object_count=0,
            mesh_count=0,
            mesh_vertex_count=0,
            mesh_index_count=0,
            skinned_mesh_count=0,
            light_count=0,
            armature_count=0,
            bone_count=0,
            animation_count=0,
            animation_matrix_count=0,
            material_slot_count=0,
            material_count=0,
            image_count=0,
            image_byte_count=0,
            byte_count=0,
            generation_seconds=0.0,
            timings={key: 0.0 for key in EXPORT_TIMING_KEYS},
            reset=reset,
        )

    def add_timing(self, key, elapsed_seconds):
        self["timings"][key] += elapsed_seconds

    @contextmanager
    def measure(self, key):
        start = time.perf_counter()
        try:
            yield
        finally:
            self.add_timing(key, time.perf_counter() - start)


def build_offset_vector(builder, decoded_values, start_vector):
    """Build an offset vector from values already arranged in decoded order."""
    decoded_values = list(decoded_values)
    start_vector(builder, len(decoded_values))
    for value in reversed(decoded_values):
        builder.PrependUOffsetTRelative(value)
    return builder.EndVector()


def build_int32_vector(builder, decoded_values, start_vector):
    """Build an int32 vector from values already arranged in decoded order."""
    decoded_values = list(decoded_values)
    start_vector(builder, len(decoded_values))
    for value in reversed(decoded_values):
        builder.PrependInt32(value)
    return builder.EndVector()

# Overridden print that prints to blender console windows
def print(*args, **kwargs):
    # Standard print to stdout
    builtins.print(*args, **kwargs)

    # Console operators can force a depsgraph refresh. Never invoke them from
    # background mode or while a temporary collection-evaluation scene is
    # active; doing so can invalidate evaluated data that is being exported.
    if bpy.app.background or bpy.context.scene.name.startswith(EVALUATION_SCENE_PREFIX):
        return
    
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

def native_live_link_available():
    return hasattr(bpy.app, "live_link_make_update")

def scene_uses_python_export_fallback(scene=None):
    if scene is None:
        scene = bpy.context.scene
    return bool(getattr(scene, "live_link_use_python_export_fallback", False))

def get_editor_camera_snapshot(context=None):
    """Return location/forward/up from the current Blender 3D viewport."""
    context = context or bpy.context
    area = getattr(context, "area", None)
    if area is None or area.type != 'VIEW_3D':
        window = getattr(context, "window", None)
        screen = getattr(window, "screen", None)
        if screen is None:
            return None
        view_areas = [
            candidate
            for candidate in screen.areas
            if candidate.type == 'VIEW_3D'
            and candidate.width > 0
            and candidate.height > 0
        ]
        if not view_areas:
            return None
        area = max(view_areas, key=lambda candidate: candidate.width * candidate.height)

    space = area.spaces.active
    region_3d = getattr(space, "region_3d", None)
    if region_3d is None:
        return None

    try:
        inverse_view = region_3d.view_matrix.inverted_safe()
        location = inverse_view.translation.copy()
        rotation = region_3d.view_rotation.copy()
        forward = rotation @ Vector((0.0, 0.0, -1.0))
        up = rotation @ Vector((0.0, 1.0, 0.0))
    except Exception:
        return None

    if forward.length_squared <= 1.0e-12 or up.length_squared <= 1.0e-12:
        return None
    forward.normalize()
    up.normalize()

    values = (*location, *forward, *up)
    if not all(math.isfinite(value) for value in values):
        return None
    if abs(forward.dot(up)) >= 0.999:
        return None
    return tuple(float(value) for value in values)

def is_mesh_export_object(obj):
    return obj is not None and obj.type in {'MESH', 'CURVE'}

def object_visible_in_game(obj, view_layer=None):
    if not obj.visible_get(view_layer=view_layer):
        return False

    settings = getattr(obj, "live_link_settings", None)
    if settings is None:
        return True

    for component in settings.components:
        if component.type != 'CHARACTER':
            continue
        character = getattr(component, "player", None)
        if character is not None and getattr(character, "hide_mesh_in_game", True):
            return False

    return True

# Class to manage our live link connection
class LiveLinkConnection():
    def __init__(self):
        self.update_sequence = 0
        self.instance_uid_registry = InstanceUidRegistry()
        self.export_snapshot = {}
        # bpy.context is overridden to the temporary evaluation scene while a
        # collection instance is expanded, so the host scene and its view layer
        # are recorded here for the paths that must not follow that override.
        self.active_export_scene = None
        self.active_export_view_layer = None
        # Memo for one export pass; None outside a pass. See get_armature_actions.
        self.export_pass_armature_actions = None
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

    def connect(self, log_failure=True):
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
            if log_failure:
                print("Failed to Connect to Running Game")
            return False

    def send(self, data):
        is_connected = self.is_connected()
        if not is_connected:
            print("Attempt to reconnect")
            is_connected = self.connect()

        if not is_connected:
            return False

        try:
            self.my_socket.sendall(data)
            return True
        except Exception as e:
            print(traceback.format_exc())
            print("Error: LiveLinkConnection::send")
            self.close_socket()
            self.create_socket()
            return False

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
            export_stats.add_timing(key, elapsed_seconds)

    def mesh_needs_triangulation(self, mesh):
        return any(polygon.loop_total != 3 for polygon in mesh.polygons)

    def export_scene_for_object(self, blender_object):
        """Scene and view layer that own blender_object during this export.

        Frame stepping and depsgraph flushes only reach objects belonging to
        the scene they are issued against. Collection occurrences are sampled
        from temporary copies in the evaluation scene while direct objects are
        sampled from the host scene, so the overridden bpy.context cannot serve
        both.
        """
        host_scene = self.active_export_scene
        if host_scene is not None and host_scene.objects.get(blender_object.name) == blender_object:
            return host_scene, self.active_export_view_layer or bpy.context.view_layer
        return bpy.context.scene, bpy.context.view_layer

    def get_mesh(self, obj, dependency_graph, use_rest_pose=False, export_stats=None):
        disabled_modifiers = []
        mesh_eval_start = time.perf_counter()
        _, view_layer = self.export_scene_for_object(obj)
        if use_rest_pose:
            for modifier in obj.modifiers:
                if modifier.type == 'ARMATURE' and modifier.show_viewport:
                    disabled_modifiers.append(modifier)
                    modifier.show_viewport = False
            view_layer.update()

        try:
            if obj.type == 'MESH' and obj.mode == 'EDIT':
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
            with export_stats.measure("triangulation") if export_stats is not None else nullcontext():
                mesh.calc_loop_triangles()
            return mesh
        finally:
            for modifier in disabled_modifiers:
                modifier.show_viewport = True
            if disabled_modifiers:
                view_layer.update()

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

    def collect_export_objects(self, changed_objects, scene_objects):
        """Return the stable, UID-deduplicated mesh/armature export closure."""
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
            return any(
                scene_object.type == 'MESH'
                and scene_object.live_link_settings.enable_live_link
                and self.get_mesh_armature(scene_object) == armature_object
                for scene_object in scene_objects
            )

        for blender_object in changed_objects:
            if blender_object.live_link_settings.enable_live_link:
                queue_export_object(blender_object)
                if blender_object.type == 'MESH':
                    queue_export_object(self.get_mesh_armature(blender_object))
            elif (
                blender_object.type == 'ARMATURE'
                and armature_is_referenced_by_live_linked_mesh(blender_object)
            ):
                queue_export_object(blender_object)

        return objects_to_export

    @staticmethod
    def _object_enabled(blender_object):
        settings = getattr(blender_object, "live_link_settings", None)
        return settings is None or settings.enable_live_link

    @staticmethod
    def _collection_instance(blender_object):
        if blender_object is None or blender_object.instance_type != 'COLLECTION':
            return None
        return blender_object.instance_collection

    def _scope_export_objects(self, collection):
        all_objects = sorted(collection.all_objects, key=lambda obj: (obj.session_uid, obj.name))
        enabled = [obj for obj in all_objects if self._object_enabled(obj)]
        required_ids = {obj.session_uid for obj in enabled}
        for obj in enabled:
            if obj.type == 'MESH':
                armature = self.get_mesh_armature(obj)
                if armature is not None:
                    required_ids.add(armature.session_uid)
        return [obj for obj in all_objects if obj.session_uid in required_ids]

    @staticmethod
    def _collection_dependency_uids(collection):
        dependency_uids = set()

        def visit(current):
            uid = int(current.session_uid)
            if uid in dependency_uids:
                return
            dependency_uids.add(uid)
            for child in current.children:
                visit(child)

        visit(collection)
        return tuple(sorted(dependency_uids))

    def _walk_instance_collections(self, root_objects):
        collections = {}
        instance_keys = set()

        def walk(collection, path, ancestors):
            collection_uid = collection.session_uid
            if collection_uid in ancestors:
                print(
                    "Live Link collection instance cycle skipped: "
                    + " -> ".join(str(value) for value in (*ancestors, collection_uid))
                )
                return
            collections[collection_uid] = collection
            next_ancestors = (*ancestors, collection_uid)
            for source in self._scope_export_objects(collection):
                key = (*path, source.session_uid)
                instance_keys.add(key)
                nested_collection = self._collection_instance(source)
                if nested_collection is not None and self._object_enabled(source):
                    walk(nested_collection, key, next_ancestors)

        for root in root_objects:
            collection = self._collection_instance(root)
            if collection is not None and self._object_enabled(root):
                walk(collection, (root.session_uid,), ())

        return list(collections.values()), instance_keys

    @contextmanager
    def export_evaluation_context(self, root_objects):
        active_depsgraph = bpy.context.evaluated_depsgraph_get()
        active_view_layer = bpy.context.view_layer
        active_scene = bpy.context.scene
        collections, instance_keys = self._walk_instance_collections(root_objects)
        direct_uids = {obj.session_uid for obj in active_scene.objects}
        self.instance_uid_registry.prepare(instance_keys, direct_uids)

        self.active_export_scene = active_scene
        self.active_export_view_layer = active_view_layer
        self.export_pass_armature_actions = {}

        if not collections:
            try:
                yield active_depsgraph, active_depsgraph, active_view_layer, active_view_layer, {}
            finally:
                self.active_export_scene = None
                self.active_export_view_layer = None
                self.export_pass_armature_actions = None
            return

        source_scene = active_scene
        evaluation_scene = None
        evaluation_objects = {}
        temporary_objects = []
        with suspend_depsgraph_updates():
            try:
                evaluation_scene = bpy.data.scenes.new(EVALUATION_SCENE_PREFIX)
                evaluation_scene.frame_start = source_scene.frame_start
                evaluation_scene.frame_end = source_scene.frame_end
                evaluation_scene.render.fps = source_scene.render.fps
                evaluation_scene.render.fps_base = source_scene.render.fps_base
                evaluation_scene.frame_set(
                    source_scene.frame_current,
                    subframe=source_scene.frame_subframe,
                )
                for collection in sorted(collections, key=lambda item: (item.session_uid, item.name)):
                    try:
                        evaluation_scene.collection.children.link(collection)
                    except RuntimeError:
                        # A collection may already be reachable through another linked collection.
                        pass

                source_objects = {
                    source.session_uid: source
                    for collection in collections
                    for source in collection.all_objects
                }
                for source_uid, source in sorted(source_objects.items()):
                    needs_copy = source.type == 'ARMATURE' or (
                        is_mesh_export_object(source) and self.get_mesh_armature(source) is not None
                    )
                    if not needs_copy:
                        continue
                    evaluation_object = source.copy()
                    evaluation_object.name = f"{EVALUATION_SCENE_PREFIX}{source.name}"
                    evaluation_object.hide_viewport = False
                    evaluation_object.hide_render = False
                    evaluation_scene.collection.objects.link(evaluation_object)
                    evaluation_objects[source_uid] = evaluation_object
                    temporary_objects.append(evaluation_object)

                for source_uid, evaluation_object in evaluation_objects.items():
                    source = source_objects[source_uid]
                    if source.parent is not None:
                        evaluation_object.parent = evaluation_objects.get(
                            source.parent.session_uid,
                            source.parent,
                        )
                    for modifier in evaluation_object.modifiers:
                        if modifier.type == 'ARMATURE':
                            if modifier.object is not None:
                                modifier.object = evaluation_objects.get(
                                    modifier.object.session_uid,
                                    modifier.object,
                                )
                            modifier.show_viewport = False

                evaluation_view_layer = evaluation_scene.view_layers[0]
                with bpy.context.temp_override(
                    scene=evaluation_scene,
                    view_layer=evaluation_view_layer,
                ):
                    evaluation_depsgraph = bpy.context.evaluated_depsgraph_get()
                    yield (
                        active_depsgraph,
                        evaluation_depsgraph,
                        active_view_layer,
                        evaluation_view_layer,
                        evaluation_objects,
                    )
            finally:
                if evaluation_scene is not None:
                    bpy.data.scenes.remove(evaluation_scene)
                for temporary_object in temporary_objects:
                    if temporary_object.name in bpy.data.objects:
                        bpy.data.objects.remove(temporary_object)
                # Building and tearing down the evaluation scene tags the
                # depsgraph. Flush it here, while depsgraph callbacks are still
                # suspended, so the exporter never observes its own bookkeeping
                # as an authored change and reschedules itself indefinitely.
                try:
                    active_view_layer.update()
                except RuntimeError:
                    pass
                self.active_export_scene = None
                self.active_export_view_layer = None
                self.export_pass_armature_actions = None

    @staticmethod
    def _id_uid(blender_id):
        return getattr(blender_id, "session_uid", None) if blender_id is not None else None

    def _object_dependency_ids(self, blender_object, instancer_path=(), collection_path=()):
        dependency_ids = {int(value) for value in instancer_path}
        dependency_ids.update(int(value) for value in collection_path)

        def add(blender_id):
            uid = self._id_uid(blender_id)
            if uid is not None:
                dependency_ids.add(int(uid))

        add(blender_object)
        add(getattr(blender_object, "data", None))
        add(getattr(getattr(blender_object, "animation_data", None), "action", None))
        for material_slot in getattr(blender_object, "material_slots", []):
            material = material_slot.material
            add(material)
            node_tree = getattr(material, "node_tree", None) if material else None
            if node_tree:
                # The node tree is reported separately from its material, so it
                # has to be a tracked dependency in its own right.
                add(node_tree)
                for node in node_tree.nodes:
                    add(getattr(node, "image", None))
        armature = self.get_mesh_armature(blender_object)
        if armature is not None:
            add(armature)
            add(armature.data)
            for action in self.get_armature_actions(armature):
                add(action)
        return frozenset(dependency_ids)

    def _make_direct_occurrence(self, blender_object, dependency_graph, view_layer=None):
        uid = blender_object.session_uid
        # A direct object lives in the host scene, so it must be resolved
        # against the host view layer. visible_get() silently returns False for
        # an object with no base in the view layer it is given, which would
        # hide the whole scene while the evaluation-scene override is active.
        return ExportOccurrence(
            source_object=blender_object,
            evaluation_object=blender_object,
            unique_id=uid,
            name=blender_object.name,
            matrix_world=blender_object.matrix_world.copy(),
            visibility=object_visible_in_game(blender_object, view_layer=view_layer),
            dependency_graph=dependency_graph,
            reference_uid_map={},
            occurrence_key=("DIRECT", uid),
            instancer_path=(),
            dependency_ids=self._object_dependency_ids(blender_object),
        )

    def _expand_collection_occurrences(
        self,
        root,
        collection,
        parent_matrix,
        uid_path,
        name_path,
        inherited_visible,
        dependency_graph,
        evaluation_view_layer,
        evaluation_objects,
        ancestors=(),
        dependency_collection_path=(),
    ):
        collection_uid = collection.session_uid
        if collection_uid in ancestors:
            print(
                "Live Link collection instance cycle skipped: "
                + "/".join((*name_path, collection.name))
            )
            return []

        scope_objects = self._scope_export_objects(collection)
        scope_map = {
            source.session_uid: self.instance_uid_registry.uid_for_key((*uid_path, source.session_uid))
            for source in scope_objects
        }
        collection_basis = (
            parent_matrix
            @ MathMatrix.Translation(tuple(-value for value in collection.instance_offset))
        )
        collection_dependencies = tuple(sorted({
            *dependency_collection_path,
            *self._collection_dependency_uids(collection),
        }))
        next_ancestors = (*ancestors, collection_uid)
        occurrences = []

        for source in scope_objects:
            source_key = (*uid_path, source.session_uid)
            occurrence_uid = scope_map[source.session_uid]
            occurrence_matrix = collection_basis @ source.matrix_world
            source_visible = object_visible_in_game(source, view_layer=evaluation_view_layer)
            occurrence_visible = bool(inherited_visible and source_visible)
            occurrence_name_path = (*name_path, source.name)
            occurrences.append(ExportOccurrence(
                source_object=source,
                evaluation_object=evaluation_objects.get(source.session_uid, source),
                unique_id=occurrence_uid,
                name="/".join(occurrence_name_path),
                matrix_world=occurrence_matrix.copy(),
                visibility=occurrence_visible,
                dependency_graph=dependency_graph,
                reference_uid_map=dict(scope_map),
                occurrence_key=("INSTANCE", *source_key),
                instancer_path=uid_path,
                dependency_ids=self._object_dependency_ids(
                    source,
                    instancer_path=uid_path,
                    collection_path=collection_dependencies,
                ),
            ))

            nested_collection = self._collection_instance(source)
            if nested_collection is not None and self._object_enabled(source):
                nested_visible = bool(
                    inherited_visible
                    and object_visible_in_game(source, view_layer=evaluation_view_layer)
                )
                occurrences.extend(self._expand_collection_occurrences(
                    root=root,
                    collection=nested_collection,
                    parent_matrix=occurrence_matrix,
                    uid_path=source_key,
                    name_path=occurrence_name_path,
                    inherited_visible=nested_visible,
                    dependency_graph=dependency_graph,
                    evaluation_view_layer=evaluation_view_layer,
                    evaluation_objects=evaluation_objects,
                    ancestors=next_ancestors,
                    dependency_collection_path=collection_dependencies,
                ))

        return occurrences

    def collect_export_occurrences(
        self,
        changed_objects,
        active_depsgraph,
        instance_depsgraph,
        active_view_layer,
        instance_view_layer,
        evaluation_objects,
    ):
        changed_objects = list(changed_objects)
        # The export closure is scoped to the host scene, not to the temporary
        # evaluation scene bpy.context may currently point at.
        host_scene = self.active_export_scene or bpy.context.scene
        direct_objects = self.collect_export_objects(changed_objects, host_scene.objects)
        direct_ids = {obj.session_uid for obj in direct_objects}
        occurrences = [
            self._make_direct_occurrence(obj, active_depsgraph, view_layer=active_view_layer)
            for obj in direct_objects
        ]

        for root in changed_objects:
            collection = self._collection_instance(root)
            if collection is None or not self._object_enabled(root):
                continue
            if root.session_uid not in direct_ids:
                occurrences.append(
                    self._make_direct_occurrence(
                        root,
                        active_depsgraph,
                        view_layer=active_view_layer,
                    )
                )
            try:
                root_visible = object_visible_in_game(root, view_layer=active_view_layer)
            except RuntimeError:
                root_visible = not root.hide_viewport
            occurrences.extend(self._expand_collection_occurrences(
                root=root,
                collection=collection,
                parent_matrix=root.matrix_world.copy(),
                uid_path=(root.session_uid,),
                name_path=(root.name,),
                inherited_visible=root_visible,
                dependency_graph=instance_depsgraph,
                evaluation_view_layer=instance_view_layer,
                evaluation_objects=evaluation_objects,
            ))

        deduplicated = []
        seen_uids = set()
        for occurrence in occurrences:
            if occurrence.unique_id in seen_uids:
                continue
            seen_uids.add(occurrence.unique_id)
            deduplicated.append(occurrence)
        return deduplicated

    def resolve_attachment_point(self, marker, attachment, dependency_graph, export_occurrence=None):
        owner = attachment.owner_part
        owner_id = (
            export_occurrence.remap_uid(owner)
            if export_occurrence is not None
            else (owner.session_uid if owner else -1)
        )
        binding_type = AttachmentBindingType.AttachmentBindingType.Object
        armature_id = -1
        bone_name = ""
        local_transform = marker.matrix_world.copy()
        valid = False
        error = "owner is not set"

        if owner:
            owner_is_body = any(
                component.type == Component_Part.type_name
                and component.part.part_type == 'BODY'
                for component in owner.live_link_settings.components
            )
            if not owner_is_body:
                error = f"owner '{owner.name}' is not a Body part"
            elif marker.parent == owner and marker.parent_type == 'OBJECT':
                owner_eval = owner.evaluated_get(dependency_graph) if dependency_graph else owner
                marker_eval = marker.evaluated_get(dependency_graph) if dependency_graph else marker
                local_transform = owner_eval.matrix_world.inverted_safe() @ marker_eval.matrix_world
                valid = True
            elif marker.parent_type == 'BONE' and marker.parent:
                owner_armature = self.get_mesh_armature(owner)
                armature = marker.parent
                bone_name = marker.parent_bone or ""
                binding_type = AttachmentBindingType.AttachmentBindingType.Bone
                armature_id = (
                    export_occurrence.remap_uid(armature)
                    if export_occurrence is not None
                    else armature.session_uid
                )
                if owner_armature != armature:
                    error = "bone parent is not the Body part's armature"
                elif not bone_name:
                    error = "bone parent has no bone name"
                else:
                    armature_eval = armature.evaluated_get(dependency_graph) if dependency_graph else armature
                    marker_eval = marker.evaluated_get(dependency_graph) if dependency_graph else marker
                    pose_bone = armature_eval.pose.bones.get(bone_name) if armature_eval.pose else None
                    if not pose_bone:
                        error = f"bone '{bone_name}' was not found"
                    else:
                        bone_world = armature_eval.matrix_world @ pose_bone.matrix
                        local_transform = bone_world.inverted_safe() @ marker_eval.matrix_world
                        valid = True
            else:
                error = "marker must be parented to its Body or to the Body armature's bone"

        if not valid:
            print(f"Attachment Point '{marker.name}' is invalid: {error}")

        return {
            "owner_id": owner_id,
            "binding_type": binding_type,
            "armature_id": armature_id,
            "bone_name": bone_name,
            "local_transform": local_transform,
            "valid": valid,
        }

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
        # Resolving an armature's actions scans every fcurve of every action in
        # the file, and the occurrence index asks for it once per skinned mesh.
        # Memoize for the duration of one export pass only: caching across ticks
        # would hold bpy references that Blender can free underneath us.
        cache = self.export_pass_armature_actions
        cache_key = armature_obj.session_uid if cache is not None else None
        if cache is not None and cache_key in cache:
            return cache[cache_key]

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
        resolved = active_actions + remaining_actions
        if cache is not None:
            cache[cache_key] = resolved
        return resolved

    def make_flatbuffer_animation(self, builder, armature_obj, action, bones, export_stats=None):
        scene, view_layer = self.export_scene_for_object(armature_obj)
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
                view_layer.update()

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
            view_layer.update()
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
 
    def make_flatbuffer_object(self, builder, occurrence, dependency_graph, referenced_materials, export_stats=None):
        obj = occurrence.source_object
        # Allocate string for object name
        object_name = builder.CreateString(occurrence.name)

        # Mesh Data
        mesh_fb = None
        if is_mesh_export_object(obj):
            mesh_armature = self.get_mesh_armature(obj)
            evaluation_object = occurrence.evaluation_object
            mesh = self.get_mesh(
                evaluation_object,
                dependency_graph,
                mesh_armature is not None and evaluation_object == obj,
                export_stats,
            )
            vertex_count = len(mesh.vertices)
            loop_count = len(mesh.loops)

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
            loop_triangle_indices = np.asarray(
                [loop_index for triangle in mesh.loop_triangles for loop_index in triangle.loops],
                dtype=np.int32
            )
            if export_stats is not None:
                export_stats["mesh_count"] += 1
                export_stats["mesh_vertex_count"] += int(new_vertex_count)
                export_stats["mesh_index_count"] += int(len(loop_triangle_indices))

            # --- Build new vertex buffers ---
            mesh_positions = positions[unique_keys['v']]
            mesh_normals   = normals[unique_keys['v']]
            mesh_uvs       = np.zeros((new_vertex_count, 2), dtype=np.float32)
            mesh_uvs[:, 0] = unique_keys['u'] / 1e6
            mesh_uvs[:, 1] = unique_keys['v2'] / 1e6

            # --- Build triangle index buffer ---
            indices = new_indices[loop_triangle_indices].astype(np.int32)

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
            material_ids = []
            material_slots = list(getattr(obj, "material_slots", []))
            if material_slots:
                for material_slot in material_slots:
                    material = material_slot.material
                    if material is None:
                        continue
                    material_id = material.session_uid
                    # append to our material list
                    material_ids.append(material_id)
                    # Add to referenced material dict
                    if referenced_materials.get(material_id) is None:
                        referenced_materials[material_id] = material
            material_ids_fb = build_int32_vector(
                builder,
                material_ids,
                Mesh.MeshStartMaterialIdsVector,
            )
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
                Mesh.AddArmatureId(builder, occurrence.remap_uid(mesh_armature))
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
            armature_bones_fb = build_offset_vector(
                builder,
                bones_fb,
                Armature.ArmatureStartBonesVector,
            )

            actions = self.get_armature_actions(obj)
            if actions:
                print(f"Armature {obj.name}: exporting {len(actions)} animation(s): {', '.join(action.name for action in actions)}")
            else:
                print(f"Armature {obj.name}: no compatible pose-bone actions found")

            animations_fb = []
            for action in actions:
                animations_fb.append(self.make_flatbuffer_animation(
                    builder,
                    occurrence.evaluation_object,
                    action,
                    obj.data.bones,
                    export_stats,
                ))

            armature_animations_fb = build_offset_vector(
                builder,
                animations_fb,
                Armature.ArmatureStartAnimationsVector,
            )

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
                        # Keep Blender's familiar artistic Sun strength while
                        # sending physical irradiance to the game runtime.
                        power = light_data.energy * EARTH_TOA_SOLAR_IRRADIANCE_W_M2,
                        castShadows = light_data.use_shadow
                    )
                    Light.AddSunLight(builder, sun_light)
                case light_type_enum.Area:
                    #TODO:
                    pass

            Light.AddUseShadow(builder, light_data.use_shadow)

            light_fb = Light.End(builder)

        # Add Object Gameplay Components
        gameplay_components = builder_create_gameplay_components(
            builder,
            obj.live_link_settings,
            obj,
            dependency_graph,
            self,
            export_occurrence=occurrence,
        )
        
        # Begin New Object 
        Object.Start(builder)
        
        # Object Name
        Object.AddName(builder, object_name)

        # Session UID (note that this is a fairly new addition to the python API)
        Object.AddUniqueId(builder, occurrence.unique_id)

        # Character collision meshes can remain live-linked while opting out
        # of in-game rendering.
        Object.AddVisibility(builder, occurrence.visibility)

        # Get world-space location, rotation, and scale
        obj_matrix_world = occurrence.matrix_world
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
                isDynamic = (
                    obj.rigid_body.type == 'ACTIVE'
                    and obj.rigid_body.enabled
                    and not obj.rigid_body.kinematic
                ),
                mass = obj.rigid_body.mass
            ))

        # Add Gameplay Components data if it exists
        if gameplay_components is not None:
            Object.AddComponents(builder, gameplay_components)
            
        # End New Object add add to array
        live_link_object = Object.End(builder)

        return live_link_object

    def _serialize_occurrences(
        self,
        occurrences,
        deleted_uids,
        dependency_graph,
        reset,
        update_reason,
        editor_camera,
    ):
        if native_live_link_available() and not scene_uses_python_export_fallback():
            self.update_sequence += 1
            output = self.make_update_native(
                occurrences,
                deleted_uids,
                dependency_graph=dependency_graph,
                reset=reset,
                update_reason=update_reason,
                sequence=self.update_sequence,
                editor_camera=editor_camera,
            )
            print(
                "Live Link Native Export Returned: "
                f"seq={self.update_sequence} "
                f"bytes={len(output)}"
            )
            return output
        return self._make_update_python_occurrences(
            occurrences,
            deleted_uids,
            reset=reset,
            update_reason=update_reason,
            editor_camera=editor_camera,
        )

    def make_update(self, in_object_list, in_deleted_object_uids, reset=False, update_reason="unknown"):
        editor_camera = get_editor_camera_snapshot()
        objects = list(in_object_list)
        with self.export_evaluation_context(objects) as evaluation:
            active_dg, instance_dg, active_view_layer, instance_view_layer, evaluation_objects = evaluation
            occurrences = self.collect_export_occurrences(
                objects,
                active_dg,
                instance_dg,
                active_view_layer,
                instance_view_layer,
                evaluation_objects,
            )
            deleted_uids = set(int(uid) for uid in in_deleted_object_uids)
            deleted_uids.update(self.instance_uid_registry.take_retired_uids())
            deleted_uids = sorted(deleted_uids)

            return self._serialize_occurrences(
                occurrences,
                deleted_uids,
                dependency_graph=active_dg,
                reset=reset,
                update_reason=update_reason,
                editor_camera=editor_camera,
            )

    def send_scene_changes(self, dirty_ids, update_reason, force_full=False):
        all_scene_objects = list(bpy.context.scene.objects)
        editor_camera = get_editor_camera_snapshot()
        dirty_ids = {int(uid) for uid in dirty_ids}

        with self.export_evaluation_context(all_scene_objects) as evaluation:
            active_dg, instance_dg, active_view_layer, instance_view_layer, evaluation_objects = evaluation
            all_occurrences = self.collect_export_occurrences(
                all_scene_objects,
                active_dg,
                instance_dg,
                active_view_layer,
                instance_view_layer,
                evaluation_objects,
            )
            current_snapshot = {
                occurrence.occurrence_key: OccurrenceSnapshot.from_occurrence(occurrence)
                for occurrence in all_occurrences
            }
            previous_snapshot = self.export_snapshot
            deleted_uids = {
                snapshot.unique_id
                for key, snapshot in previous_snapshot.items()
                if (
                    key not in current_snapshot
                    or current_snapshot[key].unique_id != snapshot.unique_id
                )
            }
            deleted_uids.update(self.instance_uid_registry.take_retired_uids())

            selected = []
            for occurrence in all_occurrences:
                current = current_snapshot[occurrence.occurrence_key]
                previous = previous_snapshot.get(occurrence.occurrence_key)
                signature_changed = (
                    previous is None
                    or previous.unique_id != current.unique_id
                    or previous.matrix_world_values != current.matrix_world_values
                    or previous.visibility != current.visibility
                )
                dependency_changed = bool(current.dependency_ids.intersection(dirty_ids))
                if force_full or signature_changed or dependency_changed:
                    selected.append(occurrence)

            if not selected and not deleted_uids and not force_full:
                return True

            output = self._serialize_occurrences(
                selected,
                sorted(deleted_uids),
                dependency_graph=active_dg,
                reset=False,
                update_reason=update_reason,
                editor_camera=editor_camera,
            )
            sent = self.send(output)
            if sent:
                self.export_snapshot = current_snapshot
            return sent

    def make_update_native(
        self,
        in_object_list,
        in_deleted_object_uids,
        dependency_graph=None,
        reset=False,
        update_reason="unknown",
        sequence=0,
        editor_camera=None,
    ):
        if not native_live_link_available():
            raise RuntimeError("Native Live Link export is not available")
        if dependency_graph is None:
            dependency_graph = bpy.context.evaluated_depsgraph_get()

        print(
            "\nLive Link Native Export Requested: "
            f"seq={sequence} "
            f"reason={update_reason} "
            f"reset={reset} "
            f"objects={len(in_object_list)} "
            f"deleted={len(in_deleted_object_uids)}"
        )

        output = bpy.app.live_link_make_update(
            in_object_list,
            in_deleted_object_uids,
            dependency_graph,
            reset,
            update_reason,
            sequence,
            editor_camera,
        )
        if not isinstance(output, (bytes, bytearray)):
            raise TypeError("bpy.app.live_link_make_update must return bytes")
        return bytes(output)

    def make_full_update_object_list(self):
        return list(bpy.context.scene.objects)

    def compare_native_python_full_update(self):
        if not native_live_link_available():
            raise RuntimeError("Native Live Link export is not available")
        native_compare = getattr(bpy.app, "live_link_compare_updates", None)
        if native_compare is None:
            raise RuntimeError("Native Live Link compare is not available")

        objects = self.make_full_update_object_list()
        editor_camera = get_editor_camera_snapshot()
        old_sequence = self.update_sequence
        try:
            with self.export_evaluation_context(objects) as evaluation:
                active_dg, instance_dg, active_view_layer, instance_view_layer, evaluation_objects = evaluation
                occurrences = self.collect_export_occurrences(
                    objects,
                    active_dg,
                    instance_dg,
                    active_view_layer,
                    instance_view_layer,
                    evaluation_objects,
                )
                native_bytes = self.make_update_native(
                    occurrences,
                    [],
                    dependency_graph=active_dg,
                    reset=False,
                    update_reason="compare_native_python_native",
                    sequence=old_sequence + 1,
                    editor_camera=editor_camera,
                )
                python_bytes = self._make_update_python_occurrences(
                    occurrences,
                    [],
                    reset=False,
                    update_reason="compare_native_python_python",
                    increment_sequence=False,
                    editor_camera=editor_camera,
                )
        finally:
            self.update_sequence = old_sequence

        matched, message = native_compare(native_bytes, python_bytes, 100)
        if matched:
            print("\nLive Link Native/Python Compare: SEMANTIC MATCH " + message)
            return True, "Native/Python exports semantically match"

        print("\nLive Link Native/Python Compare: SEMANTIC MISMATCH " + message)
        return False, message.split("\n", 1)[0]

    def make_update_python(
        self,
        in_object_list,
        in_deleted_object_uids,
        reset=False,
        update_reason="unknown",
        increment_sequence=True,
        editor_camera=None,
    ):
        objects = list(in_object_list)
        with self.export_evaluation_context(objects) as evaluation:
            active_dg, instance_dg, active_view_layer, instance_view_layer, evaluation_objects = evaluation
            occurrences = self.collect_export_occurrences(
                objects,
                active_dg,
                instance_dg,
                active_view_layer,
                instance_view_layer,
                evaluation_objects,
            )
            deleted_uids = set(int(uid) for uid in in_deleted_object_uids)
            deleted_uids.update(self.instance_uid_registry.take_retired_uids())
            return self._make_update_python_occurrences(
                occurrences,
                sorted(deleted_uids),
                reset=reset,
                update_reason=update_reason,
                increment_sequence=increment_sequence,
                editor_camera=editor_camera,
            )

    # Creates an update for already-collected occurrences using Python FlatBuffers generation.
    def _make_update_python_occurrences(
        self,
        occurrences,
        in_deleted_object_uids,
        reset=False,
        update_reason="unknown",
        increment_sequence=True,
        editor_camera=None,
    ):
        export_generation_start = time.perf_counter()
        if increment_sequence:
            self.update_sequence += 1
            sequence = self.update_sequence
        else:
            sequence = self.update_sequence + 1

        # init flatbuffers builder
        builder = flatbuffers.Builder(0)
        export_stats = ExportStats(
            sequence=sequence,
            reason=update_reason,
            input_object_count=len(occurrences),
            deleted_object_count=len(in_deleted_object_uids),
            reset=reset,
        )

        # referenced materials, keyed by session_uid and updated in self.make_flatbuffer_object
        referenced_materials = {}

        live_link_objects = []
        export_stats["exported_object_count"] = len(occurrences)
        for occurrence in occurrences:
            live_link_objects.append(
                self.make_flatbuffer_object(
                    builder,
                    occurrence,
                    occurrence.dependency_graph,
                    referenced_materials,
                    export_stats
                )
            )

        # The historical exporter decoded these two vectors in reverse input
        # order. Pass that decoded order explicitly so this refactor preserves
        # payload compatibility.
        update_objects = build_offset_vector(
            builder,
            reversed(live_link_objects),
            Update.UpdateStartObjectsVector,
        )

        # create flatbuffers deleted objects
        update_deleted_object_uids = build_int32_vector(
            builder,
            reversed(in_deleted_object_uids),
            Update.UpdateStartDeletedObjectUidsVector,
        )

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
        update_materials = build_offset_vector(
            builder,
            flatbuffer_materials,
            Update.UpdateStartMaterialsVector,
        )
        self.add_export_timing(export_stats, "materials", time.perf_counter() - materials_start)

        images_start = time.perf_counter()
        flatbuffer_images = []
        for image_id, image in referenced_images.items():
            
            # Get image width and height
            image_width, image_height = image.size
            image_pixel_component_count = image_width * image_height * 4

            pixels_read_start = time.perf_counter()
            try:
                pixels_f32 = np.empty(image_pixel_component_count, dtype=np.float32)
                image.pixels.foreach_get(pixels_f32)
            except Exception:
                # Some Blender image types may not support foreach_get reliably.
                pixels_f32 = np.array(image.pixels[:], dtype=np.float32)
            self.add_export_timing(export_stats, "image_pixels_read", time.perf_counter() - pixels_read_start)

            rgba8_convert_start = time.perf_counter()
            np.clip(pixels_f32, 0.0, 1.0, out=pixels_f32)
            np.multiply(pixels_f32, 255.0, out=pixels_f32)
            np.rint(pixels_f32, out=pixels_f32)
            pixels_rgba8 = np.empty(pixels_f32.size, dtype=np.uint8)
            pixels_rgba8[:] = pixels_f32
            self.add_export_timing(export_stats, "image_rgba8_convert", time.perf_counter() - rgba8_convert_start)

            export_stats["image_count"] += 1
            export_stats["image_byte_count"] += int(pixels_rgba8.nbytes)

            # Now pass to FlatBuffers
            with export_stats.measure("image_flatbuffer_pack"):
                flatbuffer_image_data = builder.CreateNumpyVector(pixels_rgba8)

            # Build up flatbuffers image and add it to our list of images
            Image.Start(builder)
            Image.AddUniqueId(builder, image_id)
            Image.AddWidth(builder, image_width)
            Image.AddHeight(builder, image_height)
            Image.AddData(builder, flatbuffer_image_data) 
            flatbuffer_image = Image.End(builder)
            flatbuffer_images.append(flatbuffer_image)

        # Create the vector of images for our top-level Update 
        update_images = build_offset_vector(
            builder,
            flatbuffer_images,
            Update.UpdateStartImagesVector,
        )
        self.add_export_timing(export_stats, "images", time.perf_counter() - images_start)

        flatbuffer_finish_start = time.perf_counter()
        editor_camera_fb = None
        if editor_camera is not None:
            EditorCamera.Start(builder)
            location_fb = Vec3.CreateVec3(builder, *editor_camera[0:3])
            EditorCamera.AddLocation(builder, location_fb)
            forward_fb = Vec3.CreateVec3(builder, *editor_camera[3:6])
            EditorCamera.AddForward(builder, forward_fb)
            up_fb = Vec3.CreateVec3(builder, *editor_camera[6:9])
            EditorCamera.AddUp(builder, up_fb)
            editor_camera_fb = EditorCamera.End(builder)

        # Begin writing top-level update table
        Update.Start(builder)

        # Add objects vector to scene
        Update.AddObjects(builder, update_objects)
        Update.AddDeletedObjectUids(builder, update_deleted_object_uids)
        Update.AddMaterials(builder, update_materials)
        Update.AddImages(builder, update_images)
        Update.AddReset(builder, reset)
        if editor_camera_fb is not None:
            Update.AddEditorCamera(builder, editor_camera_fb)
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
            f"image_pixels_read={timing_stats['image_pixels_read']:.6f}s "
            f"image_rgba8_convert={timing_stats['image_rgba8_convert']:.6f}s "
            f"image_flatbuffer_pack={timing_stats['image_flatbuffer_pack']:.6f}s "
            f"flatbuffer_finish={timing_stats['flatbuffer_finish']:.6f}s"
        )
        return output

    def send_object_list(self, updated_objects, deleted_object_uids, update_reason="object_list"):
        return self.send(self.make_update(updated_objects, deleted_object_uids, update_reason=update_reason))

    def save_to_file(self, in_objects, in_filename, update_reason="save_to_file"):
        update = self.make_update(in_objects, [], update_reason=update_reason)
        with open(in_filename, 'wb') as f:
            f.write(update)

    def send_reset(self, update_reason="manual_reset"):
        sent = self.send(self.make_update([], [], True, update_reason=update_reason))
        if sent:
            self.export_snapshot.clear()
        return sent

live_link_connection = []

batched_dirty_ids = set()
batched_force_full = False

@contextmanager
def suspend_depsgraph_updates():
    previous_enabled = depsgraph_update_post_callback.enabled
    depsgraph_update_post_callback.enabled = False
    try:
        yield
    finally:
        depsgraph_update_post_callback.enabled = previous_enabled

def queue_object_updates(objects, update_reason):
    for blender_object in objects:
        uid = getattr(blender_object, "session_uid", None)
        if uid is not None:
            batched_dirty_ids.add(int(uid))
    schedule_send(update_reason=update_reason)

def queue_object_update(obj, update_reason):
    queue_object_updates((obj,), update_reason)

def clear_batched_depsgraph_updates(update_reason="unknown"):
    global batched_force_full
    if bpy.app.timers.is_registered(send_updates_timer):
        bpy.app.timers.unregister(send_updates_timer)

    if batched_dirty_ids or batched_force_full:
        print(
            "\nLive Link Clear Queued Depsgraph Updates: "
            f"reason={update_reason} "
            f"dirty_ids={len(batched_dirty_ids)} "
            f"force_full={batched_force_full}"
        )
        batched_dirty_ids.clear()
        batched_force_full = False

def send_full_scene_update(update_reason="full_update"):
    clear_batched_depsgraph_updates(update_reason=f"{update_reason}_before_send")
    print(
        "\nLive Link Full Update Requested: "
        f"reason={update_reason} "
        f"scene_objects={len(bpy.context.scene.objects)}"
    )

    sent = False
    with suspend_depsgraph_updates():
        try:
            sent = live_link_connection.send_scene_changes(
                dirty_ids=set(),
                update_reason=update_reason,
                force_full=True,
            )
        finally:
            clear_batched_depsgraph_updates(update_reason=f"{update_reason}_after_send")

    if sent:
        automatic_initial_full_update_timer.pending = False
    return sent

AUTOMATIC_INITIAL_UPDATE_RETRY_SECONDS = 1.0

def automatic_initial_full_update_timer():
    if not automatic_initial_full_update_timer.pending:
        return None

    if not live_link_connection.is_connected():
        if not live_link_connection.connect(log_failure=False):
            if automatic_initial_full_update_timer.status != "waiting":
                print("\nLive Link Automatic Initial Update: waiting for game on 127.0.0.1:65432")
                automatic_initial_full_update_timer.status = "waiting"
            return AUTOMATIC_INITIAL_UPDATE_RETRY_SECONDS

        print("\nLive Link Automatic Initial Update: connected to game")
        automatic_initial_full_update_timer.status = "connected"

    if send_full_scene_update(update_reason="automatic_initial_full_update"):
        automatic_initial_full_update_timer.pending = False
        automatic_initial_full_update_timer.status = "sent"
        print("Live Link Automatic Initial Update: full scene sent")
        return None

    if automatic_initial_full_update_timer.status != "retrying":
        print("Live Link Automatic Initial Update: send failed; retrying")
        automatic_initial_full_update_timer.status = "retrying"
    return AUTOMATIC_INITIAL_UPDATE_RETRY_SECONDS

automatic_initial_full_update_timer.pending = False
automatic_initial_full_update_timer.status = "idle"

def schedule_automatic_initial_full_update(update_reason="startup"):
    if (automatic_initial_full_update_timer.pending
        and bpy.app.timers.is_registered(automatic_initial_full_update_timer)):
        return

    automatic_initial_full_update_timer.pending = True
    automatic_initial_full_update_timer.status = "scheduled"

    if bpy.app.timers.is_registered(automatic_initial_full_update_timer):
        bpy.app.timers.unregister(automatic_initial_full_update_timer)

    print(f"\nLive Link Automatic Initial Update Scheduled: reason={update_reason}")
    bpy.app.timers.register(automatic_initial_full_update_timer, first_interval=0.25)

@persistent
def automatic_initial_full_update_load_post(_):
    live_link_connection.instance_uid_registry.clear()
    live_link_connection.export_snapshot.clear()
    clear_batched_depsgraph_updates(update_reason="blend_file_loaded")
    schedule_automatic_initial_full_update(update_reason="blend_file_loaded")

# Actually sends batched updates
def send_updates_timer(): 
    global batched_force_full

    # No new updates in SEND_DELAY seconds → send batched data
    if batched_dirty_ids or batched_force_full:
        update_reason = (
            f"depsgraph_timer(dirty_ids={len(batched_dirty_ids)},"
            f"force_full={batched_force_full})"
        )
        print(f"\nLive Link Timer Send: reason={update_reason}")

        with suspend_depsgraph_updates():
            sent = live_link_connection.send_scene_changes(
                dirty_ids=set(batched_dirty_ids),
                update_reason=update_reason,
                force_full=batched_force_full,
            )

        if sent:
            batched_dirty_ids.clear()
            batched_force_full = False
        else:
            return 0.25

    return None

# Schedules send but doesn't actually send the objects to the game
def schedule_send(update_reason="depsgraph_update"):
    # unregister timer if currently active:
    if bpy.app.timers.is_registered(send_updates_timer):
        bpy.app.timers.unregister(send_updates_timer)
    # Schedule new timer
    SEND_DELAY = 0.25
    bpy.app.timers.register(send_updates_timer, first_interval=SEND_DELAY)
    if batched_dirty_ids or batched_force_full:
        print(
            "\nLive Link Schedule Send: "
            f"reason={update_reason} "
            f"dirty_ids={len(batched_dirty_ids)} "
            f"force_full={batched_force_full}"
        )

# Callback when depsgraph has finished updating
@persistent
def depsgraph_update_post_callback(scene, depsgraph):
    if not depsgraph_update_post_callback.enabled:
        return

    if depsgraph_update_post_callback.suppress_next:
        depsgraph_update_post_callback.suppress_next = False
        clear_batched_depsgraph_updates(update_reason="python_export_fallback_toggled")
        return

    global batched_force_full
    saw_update = False
    relevant_types = tuple(
        blender_type
        for blender_type in (
            bpy.types.Object,
            bpy.types.Collection,
            bpy.types.Mesh,
            bpy.types.Curve,
            getattr(bpy.types, "Curves", None),
            bpy.types.Armature,
            bpy.types.Material,
            bpy.types.Image,
            bpy.types.Action,
            # Shading edits report the node tree alongside its material. Without
            # it every material tweak falls into the conservative branch below
            # and re-sends the whole scene, images included.
            bpy.types.NodeTree,
        )
        if blender_type is not None
    )
    ignored_types = (bpy.types.Scene, bpy.types.World)
    for update in depsgraph.updates:
        saw_update = True
        # depsgraph.updates reports evaluated copies, and every evaluated
        # datablock carries session_uid 0. Resolve back to the original so the
        # queued ids are the ones occurrence dependency sets are built from.
        update_id = getattr(update.id, "original", None) or update.id
        if isinstance(update_id, ignored_types):
            continue
        # The temporary evaluation scene and its object copies are exporter
        # bookkeeping, not authored changes.
        if getattr(update_id, "name", "").startswith(EVALUATION_SCENE_PREFIX):
            continue
        if not isinstance(update_id, relevant_types):
            # A node tree or another dependency not represented in occurrence
            # dependency sets can still affect evaluated output.
            batched_force_full = True
            continue
        uid = getattr(update_id, "session_uid", None)
        if uid == UNSET_SESSION_UID:
            # Unset: the removed temporary evaluation datablocks report this.
            continue
        if uid is None:
            batched_force_full = True
        else:
            batched_dirty_ids.add(int(uid))

    if saw_update:
        schedule_send(update_reason="depsgraph_update_post")

# Enable depsgraph_update_post_callback. Will be disabled to prevent recursion within depsgraph_update_post_callback
depsgraph_update_post_callback.enabled = True
depsgraph_update_post_callback.suppress_next = False

def live_link_python_export_fallback_update(self, context):
    depsgraph_update_post_callback.suppress_next = True
    clear_batched_depsgraph_updates(update_reason="python_export_fallback_toggled")

# Begin OpLiveLinkSendFullUpdate
class OpLiveLinkSendFullUpdate(bpy.types.Operator):
    """Live Link: Send Full Update """
    bl_idname = "live_link.send_full_update"
    bl_label = "Live Link: Send Full Update"
    bl_options = {'REGISTER'} 

    # Called when operator is run
    def execute(self, context):
        send_full_scene_update(update_reason="manual_full_update")
        return {'FINISHED'}
# End OpLiveLinkSendFullUpdate 

class OpLiveLinkCompareNativePythonExport(bpy.types.Operator):
    """Live Link: Compare Native and Python Full Update Exports"""
    bl_idname = "live_link.compare_native_python_export"
    bl_label = "Live Link: Compare Native/Python Export"
    bl_options = {'REGISTER'}

    def execute(self, context):
        if not native_live_link_available():
            self.report({'ERROR'}, "Native Live Link export is not available")
            return {'CANCELLED'}

        try:
            with suspend_depsgraph_updates():
                matched, message = live_link_connection.compare_native_python_full_update()
        except Exception as exc:
            print(traceback.format_exc())
            self.report({'ERROR'}, f"Native/Python compare failed: {exc}")
            return {'CANCELLED'}

        if matched:
            self.report({'INFO'}, message)
            return {'FINISHED'}

        self.report({'WARNING'}, message)
        return {'CANCELLED'}

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
        if native_live_link_available():
            layout.operator("live_link.compare_native_python_export", text="Compare Native/Python Export")
            layout.prop(scene, "live_link_use_python_export_fallback")
# End LiveLinkView3DPanel

def menu_func(self, context):
    self.layout.operator(OpLiveLinkSendFullUpdate.bl_idname)
    self.layout.operator(OpLiveLinkSendReset.bl_idname)
    self.layout.operator(OpLiveLinkResetConnection.bl_idname)

# ------------------------------------------------------------
# Define Gameplay Components 
# ------------------------------------------------------------

def gameplay_component_property_update(self, _context):
    if not depsgraph_update_post_callback.enabled:
        return
    source_object = getattr(self, "id_data", None)
    if isinstance(source_object, bpy.types.Object):
        queue_object_update(source_object, update_reason="gameplay_component_property_update")

def sky_atmosphere_property_update(self, context):
    gameplay_component_property_update(self, context)

class Component(PropertyGroup):
    # Blender UI Info
    type_name = 'INVALID'
    label = 'INVALID'

    @classmethod
    def enum_info(cls):
        return (cls.type_name, cls.label, '')

    # Adds component to flatbuffers component list
    def create_flatbuffers_object(
        self,
        builder,
        source_object=None,
        dependency_graph=None,
        exporter=None,
        export_occurrence=None,
    ):
        # Functions to generate value_type and value (implemneted by child classes) 
        value_type = self.get_flatbuffers_value_type()
        value = self.create_flatbuffers_value(
            builder,
            source_object=source_object,
            dependency_graph=dependency_graph,
            exporter=exporter,
            export_occurrence=export_occurrence,
        )

        # Create the container that contains our union and return that
        GameplayComponentContainer.Start(builder)
        if value is not None:
            GameplayComponentContainer.AddValueType(builder, value_type)
            GameplayComponentContainer.AddValue(builder, value)
        return GameplayComponentContainer.End(builder)

    def create_flatbuffers_value(self, builder, **_kwargs):
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
    height: FloatProperty(name="Height", default=6.0, min=0.0)
    radius: FloatProperty(name="Radius", default=1.0, min=0.0)
    hide_mesh_in_game: BoolProperty(
        name="Hide Mesh in Game",
        description="Hide this object's mesh in-game while keeping character collision and gameplay active",
        default=True,
        update=gameplay_component_property_update,
    )

    # Adds component to flatbuffers component list
    def create_flatbuffers_value(self, builder, **_kwargs):
        GameplayComponentCharacter.Start(builder)
        GameplayComponentCharacter.AddPlayerControlled(builder, self.player_controlled)
        GameplayComponentCharacter.AddMoveSpeed(builder, self.move_speed)
        GameplayComponentCharacter.AddJumpSpeed(builder, self.jump_speed)
        GameplayComponentCharacter.AddHeight(builder, self.height)
        GameplayComponentCharacter.AddRadius(builder, self.radius)
        return GameplayComponentCharacter.End(builder)

    def get_flatbuffers_value_type(self):
        return GameplayComponent.GameplayComponent().GameplayComponentCharacter

class Component_CameraControl(Component):
    # Blender UI Info
    type_name = 'CAMERA_CONTROL'
    label = 'Camera Control'

    # Properties
    follow_distance: FloatProperty(name="Follow Distance", default=10.0)
    follow_speed: FloatProperty(name="Follow Speed", default=10.0)

    # Adds component to flatbuffers component list
    def create_flatbuffers_value(self, builder, **_kwargs):
        GameplayComponentCameraControl.Start(builder)
        GameplayComponentCameraControl.AddFollowDistance(builder, self.follow_distance)
        GameplayComponentCameraControl.AddFollowSpeed(builder, self.follow_speed)
        return GameplayComponentCameraControl.End(builder)

    def get_flatbuffers_value_type(self):
        return GameplayComponent.GameplayComponent().GameplayComponentCameraControl

class Component_FogController(Component):
    # Blender UI Info
    type_name = 'FOG_CONTROLLER'
    label = 'Fog Controller'

    # Properties
    enabled: BoolProperty(name="Enabled", default=True)
    density: FloatProperty(name="Density", default=0.015, min=0.0)
    base_height: FloatProperty(name="Base Height", default=0.0)
    scale_height: FloatProperty(name="Scale Height", default=25.0, min=0.001)
    max_distance: FloatProperty(name="Max Distance", default=500.0, min=0.0)
    ceiling_enabled: BoolProperty(name="Ceiling Enabled", default=False)
    ceiling_height: FloatProperty(name="Ceiling Height", default=100.0)
    ceiling_fade: FloatProperty(name="Ceiling Fade", default=25.0, min=0.0)
    fog_color: FloatVectorProperty(
        name="Fog Color",
        subtype='COLOR',
        size=3,
        default=(0.55, 0.65, 0.75),
        min=0.0,
        max=1.0
    )
    ambient_intensity: FloatProperty(name="Ambient Intensity", default=0.4, min=0.0)
    sun_intensity: FloatProperty(name="Sun Intensity", default=1.0, min=0.0)
    anisotropy: FloatProperty(name="Anisotropy", default=0.2, min=-0.95, max=0.95)

    # Adds component to flatbuffers component list
    def create_flatbuffers_value(self, builder, **_kwargs):
        GameplayComponentFogController.Start(builder)
        fog_color = Vec3.CreateVec3(builder, self.fog_color[0], self.fog_color[1], self.fog_color[2])
        GameplayComponentFogController.AddFogColor(builder, fog_color)
        GameplayComponentFogController.AddEnabled(builder, self.enabled)
        GameplayComponentFogController.AddDensity(builder, self.density)
        GameplayComponentFogController.AddBaseHeight(builder, self.base_height)
        GameplayComponentFogController.AddScaleHeight(builder, self.scale_height)
        GameplayComponentFogController.AddMaxDistance(builder, self.max_distance)
        GameplayComponentFogController.AddCeilingEnabled(builder, self.ceiling_enabled)
        GameplayComponentFogController.AddCeilingHeight(builder, self.ceiling_height)
        GameplayComponentFogController.AddCeilingFade(builder, self.ceiling_fade)
        GameplayComponentFogController.AddAmbientIntensity(builder, self.ambient_intensity)
        GameplayComponentFogController.AddSunIntensity(builder, self.sun_intensity)
        GameplayComponentFogController.AddAnisotropy(builder, self.anisotropy)
        return GameplayComponentFogController.End(builder)

    def get_flatbuffers_value_type(self):
        return GameplayComponent.GameplayComponent().GameplayComponentFogController

class Component_SkyAtmosphere(Component):
    type_name = 'SKY_ATMOSPHERE'
    label = 'Sky Atmosphere'

    enabled: BoolProperty(name="Enabled", default=True, update=sky_atmosphere_property_update)
    planet_center_z_m: FloatProperty(
        name="Planet Center Z", description="World-space Z coordinate of the fixed Earth center, in meters",
        default=-6360000.0, min=-1.0e9, max=1.0e9,
        update=sky_atmosphere_property_update)
    air_density: FloatProperty(
        name="Air Density", default=1.0, min=0.0, max=4.0,
        update=sky_atmosphere_property_update)
    aerosol_density: FloatProperty(
        name="Aerosol / Dust Density", default=1.0, min=0.0, max=10.0,
        update=sky_atmosphere_property_update)
    ozone_density: FloatProperty(
        name="Ozone Density", default=1.0, min=0.0, max=4.0,
        update=sky_atmosphere_property_update)
    ground_albedo: FloatVectorProperty(
        name="Ground Albedo", subtype='COLOR', size=3,
        default=(0.1, 0.1, 0.1), min=0.0, max=1.0,
        update=sky_atmosphere_property_update)
    sky_intensity: FloatProperty(
        name="Sky Intensity", default=1.0, min=0.0, max=20.0,
        update=sky_atmosphere_property_update)
    sun_disc_angular_diameter_degrees: FloatProperty(
        name="Sun Disc Diameter", description="Angular diameter in degrees",
        default=0.5357, min=0.01, max=10.0,
        update=sky_atmosphere_property_update)
    sun_disc_intensity: FloatProperty(
        name="Sun Disc Intensity", default=1.0, min=0.0, max=20.0,
        update=sky_atmosphere_property_update)
    atmosphere_height_m: FloatProperty(
        name="Atmosphere Height", default=60000.0, min=10000.0, max=200000.0,
        update=sky_atmosphere_property_update)
    rayleigh_scale_height_m: FloatProperty(
        name="Rayleigh Scale Height", default=8000.0, min=1000.0, max=30000.0,
        update=sky_atmosphere_property_update)
    mie_scale_height_m: FloatProperty(
        name="Mie Scale Height", default=1200.0, min=100.0, max=10000.0,
        update=sky_atmosphere_property_update)
    mie_anisotropy: FloatProperty(
        name="Mie Anisotropy", default=0.8, min=0.0, max=0.95,
        update=sky_atmosphere_property_update)
    max_sun_zenith_angle_degrees: FloatProperty(
        name="Maximum Sun Zenith", default=102.0, min=90.0, max=120.0,
        update=sky_atmosphere_property_update)

    def create_flatbuffers_value(self, builder, **_kwargs):
        GameplayComponentSkyAtmosphere.Start(builder)
        GameplayComponentSkyAtmosphere.AddEnabled(builder, self.enabled)
        GameplayComponentSkyAtmosphere.AddPlanetCenterZM(builder, self.planet_center_z_m)
        GameplayComponentSkyAtmosphere.AddAirDensity(builder, self.air_density)
        GameplayComponentSkyAtmosphere.AddAerosolDensity(builder, self.aerosol_density)
        GameplayComponentSkyAtmosphere.AddOzoneDensity(builder, self.ozone_density)
        ground_albedo = Vec3.CreateVec3(builder, *self.ground_albedo)
        GameplayComponentSkyAtmosphere.AddGroundAlbedo(builder, ground_albedo)
        GameplayComponentSkyAtmosphere.AddSkyIntensity(builder, self.sky_intensity)
        GameplayComponentSkyAtmosphere.AddSunDiscAngularDiameterDegrees(
            builder, self.sun_disc_angular_diameter_degrees)
        GameplayComponentSkyAtmosphere.AddSunDiscIntensity(builder, self.sun_disc_intensity)
        GameplayComponentSkyAtmosphere.AddAtmosphereHeightM(builder, self.atmosphere_height_m)
        GameplayComponentSkyAtmosphere.AddRayleighScaleHeightM(builder, self.rayleigh_scale_height_m)
        GameplayComponentSkyAtmosphere.AddMieScaleHeightM(builder, self.mie_scale_height_m)
        GameplayComponentSkyAtmosphere.AddMieAnisotropy(builder, self.mie_anisotropy)
        GameplayComponentSkyAtmosphere.AddMaxSunZenithAngleDegrees(
            builder, self.max_sun_zenith_angle_degrees)
        return GameplayComponentSkyAtmosphere.End(builder)

    def get_flatbuffers_value_type(self):
        return GameplayComponent.GameplayComponent().GameplayComponentSkyAtmosphere

CLOUD_PROFILE_ITEMS = [
    ('STRATUS', 'Stratus', 'Low, broad, softly eroded sheet clouds'),
    ('CUMULUS', 'Cumulus', 'Puffy low clouds with cauliflower-like detail'),
    ('CUMULONIMBUS', 'Cumulonimbus', 'Deep storm towers with anvil shaping'),
    ('CIRRUS', 'Cirrus', 'High, thin, strongly eroded clouds'),
]

CLOUD_PROFILE_DEFAULTS = {
    'STRATUS': dict(base_altitude_m=1500.0, thickness_m=800.0, coverage=0.75,
                    density=0.65, shape_scale_m=20000.0, detail_scale_m=2500.0,
                    erosion=0.25, anvil_bias=0.0, phase_forward=0.65,
                    phase_backward=-0.2, phase_blend=0.8, ambient_scale=0.75,
                    multi_scattering_strength=0.6),
    'CUMULUS': dict(base_altitude_m=1800.0, thickness_m=3000.0, coverage=0.5,
                    density=1.0, shape_scale_m=8000.0, detail_scale_m=1000.0,
                    erosion=0.65, anvil_bias=0.1, phase_forward=0.75,
                    phase_backward=-0.25, phase_blend=0.8, ambient_scale=0.6,
                    multi_scattering_strength=0.8),
    'CUMULONIMBUS': dict(base_altitude_m=1200.0, thickness_m=9000.0, coverage=0.35,
                         density=1.2, shape_scale_m=10000.0, detail_scale_m=1200.0,
                         erosion=0.55, anvil_bias=0.8, phase_forward=0.8,
                         phase_backward=-0.25, phase_blend=0.85, ambient_scale=0.5,
                         multi_scattering_strength=0.85),
    'CIRRUS': dict(base_altitude_m=8000.0, thickness_m=1500.0, coverage=0.25,
                   density=0.3, shape_scale_m=24000.0, detail_scale_m=3000.0,
                   erosion=0.8, anvil_bias=0.0, phase_forward=0.6,
                   phase_backward=-0.15, phase_blend=0.75, ambient_scale=0.9,
                   multi_scattering_strength=0.35),
}

CLOUD_PROFILE_TO_FLATBUFFER = {
    'STRATUS': CloudLayerProfile.CloudLayerProfile.Stratus,
    'CUMULUS': CloudLayerProfile.CloudLayerProfile.Cumulus,
    'CUMULONIMBUS': CloudLayerProfile.CloudLayerProfile.Cumulonimbus,
    'CIRRUS': CloudLayerProfile.CloudLayerProfile.Cirrus,
}

def apply_cloud_profile_defaults(layer, profile=None):
    for name, value in CLOUD_PROFILE_DEFAULTS[profile or layer.profile].items():
        setattr(layer, name, value)

def cloud_layer_profile_update(self, context):
    apply_cloud_profile_defaults(self)
    gameplay_component_property_update(self, context)

class CloudLayerSettings(PropertyGroup):
    enabled: BoolProperty(name="Enabled", default=True, update=gameplay_component_property_update)
    profile: EnumProperty(name="Profile", items=CLOUD_PROFILE_ITEMS, default='CUMULUS',
                          update=cloud_layer_profile_update)
    seed_offset: IntProperty(name="Seed Offset", default=0, min=0, max=2147483647,
                             update=gameplay_component_property_update)
    base_altitude_m: FloatProperty(name="Base Altitude", default=1800.0, min=0.0,
                                   soft_max=20000.0, subtype='DISTANCE', unit='LENGTH',
                                   update=gameplay_component_property_update)
    thickness_m: FloatProperty(name="Thickness", default=3000.0, min=10.0,
                               soft_max=20000.0, subtype='DISTANCE', unit='LENGTH',
                               update=gameplay_component_property_update)
    coverage: FloatProperty(name="Coverage", default=0.5, min=0.0, max=1.0,
                            update=gameplay_component_property_update)
    density: FloatProperty(name="Density", default=1.0, min=0.0, max=4.0,
                           update=gameplay_component_property_update)
    shape_scale_m: FloatProperty(name="Shape Scale", default=8000.0, min=100.0,
                                 soft_max=50000.0, subtype='DISTANCE', unit='LENGTH',
                                 update=gameplay_component_property_update)
    detail_scale_m: FloatProperty(name="Detail Scale", default=1000.0, min=10.0,
                                  soft_max=10000.0, subtype='DISTANCE', unit='LENGTH',
                                  update=gameplay_component_property_update)
    erosion: FloatProperty(name="Erosion", default=0.65, min=0.0, max=1.0,
                           update=gameplay_component_property_update)
    anvil_bias: FloatProperty(name="Anvil Bias", default=0.1, min=0.0, max=1.0,
                              update=gameplay_component_property_update)
    wind_multiplier: FloatProperty(name="Wind Multiplier", default=1.0, min=-4.0, max=4.0,
                                   update=gameplay_component_property_update)
    phase_forward: FloatProperty(name="Forward Phase", default=0.75, min=-0.95, max=0.95,
                                 update=gameplay_component_property_update)
    phase_backward: FloatProperty(name="Backward Phase", default=-0.25, min=-0.95, max=0.95,
                                  update=gameplay_component_property_update)
    phase_blend: FloatProperty(name="Forward Phase Blend", default=0.8, min=0.0, max=1.0,
                               update=gameplay_component_property_update)
    ambient_scale: FloatProperty(name="Ambient Scale", default=0.6, min=0.0, max=2.0,
                                 update=gameplay_component_property_update)
    multi_scattering_strength: FloatProperty(name="Multiple Scattering", default=0.8,
                                              min=0.0, max=1.0,
                                              update=gameplay_component_property_update)

    def create_flatbuffers_value(self, builder):
        CloudLayer.Start(builder)
        CloudLayer.AddEnabled(builder, self.enabled)
        CloudLayer.AddProfile(builder, CLOUD_PROFILE_TO_FLATBUFFER[self.profile])
        CloudLayer.AddSeedOffset(builder, self.seed_offset)
        CloudLayer.AddBaseAltitudeM(builder, self.base_altitude_m)
        CloudLayer.AddThicknessM(builder, self.thickness_m)
        CloudLayer.AddCoverage(builder, self.coverage)
        CloudLayer.AddDensity(builder, self.density)
        CloudLayer.AddShapeScaleM(builder, self.shape_scale_m)
        CloudLayer.AddDetailScaleM(builder, self.detail_scale_m)
        CloudLayer.AddErosion(builder, self.erosion)
        CloudLayer.AddAnvilBias(builder, self.anvil_bias)
        CloudLayer.AddWindMultiplier(builder, self.wind_multiplier)
        CloudLayer.AddPhaseForward(builder, self.phase_forward)
        CloudLayer.AddPhaseBackward(builder, self.phase_backward)
        CloudLayer.AddPhaseBlend(builder, self.phase_blend)
        CloudLayer.AddAmbientScale(builder, self.ambient_scale)
        CloudLayer.AddMultiScatteringStrength(builder, self.multi_scattering_strength)
        return CloudLayer.End(builder)

class Component_CloudSystem(Component):
    type_name = 'CLOUD_SYSTEM'
    label = 'Cloud System'

    enabled: BoolProperty(name="Enabled", default=True, update=gameplay_component_property_update)
    seed: IntProperty(name="Seed", default=1, min=0, max=2147483647,
                      update=gameplay_component_property_update)
    weather_world_scale_m: FloatProperty(name="Weather World Scale", default=100000.0,
                                         min=1000.0, soft_max=500000.0,
                                         subtype='DISTANCE', unit='LENGTH',
                                         update=gameplay_component_property_update)
    wind_direction: FloatVectorProperty(name="Wind Direction", size=2, default=(1.0, 0.0),
                                        min=-1.0, max=1.0,
                                        update=gameplay_component_property_update)
    wind_speed_m_s: FloatProperty(name="Wind Speed", default=20.0, min=0.0, max=200.0,
                                  update=gameplay_component_property_update)
    shadow_enabled: BoolProperty(name="Cast Cloud Shadows", default=True,
                                 update=gameplay_component_property_update)
    shadow_extent_m: FloatProperty(name="Shadow Extent", default=8000.0, min=1000.0,
                                   soft_max=200000.0, subtype='DISTANCE', unit='LENGTH',
                                   update=gameplay_component_property_update)
    layers: CollectionProperty(type=CloudLayerSettings)

    def create_flatbuffers_value(self, builder, **_kwargs):
        layer_offsets = [self.layers[index].create_flatbuffers_value(builder)
                         for index in range(min(len(self.layers), 4))]
        layers = build_offset_vector(
            builder, layer_offsets, GameplayComponentCloudSystem.StartLayersVector)
        GameplayComponentCloudSystem.Start(builder)
        GameplayComponentCloudSystem.AddEnabled(builder, self.enabled)
        GameplayComponentCloudSystem.AddSeed(builder, self.seed)
        GameplayComponentCloudSystem.AddWeatherWorldScaleM(builder, self.weather_world_scale_m)
        wind_direction = Vec2.CreateVec2(builder, self.wind_direction[0], self.wind_direction[1])
        GameplayComponentCloudSystem.AddWindDirection(builder, wind_direction)
        GameplayComponentCloudSystem.AddWindSpeedMS(builder, self.wind_speed_m_s)
        GameplayComponentCloudSystem.AddShadowEnabled(builder, self.shadow_enabled)
        GameplayComponentCloudSystem.AddShadowExtentM(builder, self.shadow_extent_m)
        if layers is not None:
            GameplayComponentCloudSystem.AddLayers(builder, layers)
        return GameplayComponentCloudSystem.End(builder)

    def get_flatbuffers_value_type(self):
        return GameplayComponent.GameplayComponent().GameplayComponentCloudSystem

PART_TYPE_SPECS = [
    ('BODY', 'Body', PartType.PartType.Body, False),
    ('LEGS', 'Legs', PartType.PartType.Legs, True),
    ('LEFT_ARM', 'Left Arm', PartType.PartType.LeftArm, True),
    ('RIGHT_ARM', 'Right Arm', PartType.PartType.RightArm, True),
    ('HEAD', 'Head', PartType.PartType.Head, True),
]

PART_TYPE_ITEMS = [
    (identifier, label, '')
    for identifier, label, _flatbuffer_enum, _attachment_eligible in PART_TYPE_SPECS
]
ATTACHMENT_PART_TYPE_ITEMS = [
    (identifier, label, '')
    for identifier, label, _flatbuffer_enum, attachment_eligible in PART_TYPE_SPECS
    if attachment_eligible
]
PART_TYPE_TO_FLATBUFFER = {
    identifier: flatbuffer_enum
    for identifier, _label, flatbuffer_enum, _attachment_eligible in PART_TYPE_SPECS
}

class Component_Part(Component):
    type_name = 'PART'
    label = 'Part'

    part_type: EnumProperty(
        name="Part Type",
        items=PART_TYPE_ITEMS,
        default='BODY',
        update=gameplay_component_property_update,
    )

    def create_flatbuffers_value(self, builder, **_kwargs):
        GameplayComponentPart.Start(builder)
        GameplayComponentPart.AddPartType(builder, PART_TYPE_TO_FLATBUFFER[self.part_type])
        return GameplayComponentPart.End(builder)

    def get_flatbuffers_value_type(self):
        return GameplayComponent.GameplayComponent().GameplayComponentPart

class Component_AttachmentPoint(Component):
    type_name = 'ATTACHMENT_POINT'
    label = 'Attachment Point'

    owner_part: PointerProperty(
        name="Owner Body",
        type=bpy.types.Object,
        update=gameplay_component_property_update,
    )
    part_type: EnumProperty(
        name="Accepted Part",
        items=ATTACHMENT_PART_TYPE_ITEMS,
        default='LEGS',
        update=gameplay_component_property_update,
    )

    def create_flatbuffers_value(
        self,
        builder,
        source_object=None,
        dependency_graph=None,
        exporter=None,
        export_occurrence=None,
    ):
        resolved = exporter.resolve_attachment_point(
            source_object,
            self,
            dependency_graph,
            export_occurrence=export_occurrence,
        )
        bone_name = builder.CreateString(resolved["bone_name"]) if resolved["bone_name"] else None
        local_transform = exporter.make_flatbuffer_matrix(builder, resolved["local_transform"])

        GameplayComponentAttachmentPoint.Start(builder)
        GameplayComponentAttachmentPoint.AddOwnerPartId(builder, resolved["owner_id"])
        GameplayComponentAttachmentPoint.AddPartType(builder, PART_TYPE_TO_FLATBUFFER[self.part_type])
        GameplayComponentAttachmentPoint.AddBindingType(builder, resolved["binding_type"])
        GameplayComponentAttachmentPoint.AddArmatureId(builder, resolved["armature_id"])
        if bone_name is not None:
            GameplayComponentAttachmentPoint.AddBoneName(builder, bone_name)
        GameplayComponentAttachmentPoint.AddLocalTransform(builder, local_transform)
        GameplayComponentAttachmentPoint.AddValid(builder, resolved["valid"])
        return GameplayComponentAttachmentPoint.End(builder)

    def get_flatbuffers_value_type(self):
        return GameplayComponent.GameplayComponent().GameplayComponentAttachmentPoint

COMPONENT_SPECS = [
    (Component_Character, 'player'),
    (Component_CameraControl, 'camera_control'),
    (Component_FogController, 'fog_controller'),
    (Component_Part, 'part'),
    (Component_AttachmentPoint, 'attachment_point'),
    (Component_SkyAtmosphere, 'sky_atmosphere'),
    (Component_CloudSystem, 'cloud_system'),
]

COMPONENT_CLASSES = [component_class for component_class, _group_name in COMPONENT_SPECS]
gameplay_component_enum = [component_class.enum_info() for component_class in COMPONENT_CLASSES]
TYPE_TO_GROUP = {
    component_class.type_name: group_name
    for component_class, group_name in COMPONENT_SPECS
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
    fog_controller: PointerProperty(type=Component_FogController)
    part:           PointerProperty(type=Component_Part)
    attachment_point: PointerProperty(type=Component_AttachmentPoint)
    sky_atmosphere: PointerProperty(type=Component_SkyAtmosphere)
    cloud_system:   PointerProperty(type=Component_CloudSystem)

    # Simply forwards to relevant component data to create flatbuffer object
    def create_flatbuffers_object(
        self,
        builder,
        source_object=None,
        dependency_graph=None,
        exporter=None,
        export_occurrence=None,
    ):
       component_data = getattr(self, TYPE_TO_GROUP[self.type])
       return component_data.create_flatbuffers_object(
           builder,
           source_object=source_object,
           dependency_graph=dependency_graph,
           exporter=exporter,
           export_occurrence=export_occurrence,
       )

# ------------------------------------------------------------
# Property Group for Live-Link Specific Data 
# ------------------------------------------------------------

class LiveLinkObjectSettings(bpy.types.PropertyGroup):
    enable_live_link: bpy.props.BoolProperty(
        name="Enable Live Link",
        default=True,
        update=gameplay_component_property_update,
    )

    add_type: EnumProperty(
        name="Add Type",
        description="Type of property to add",
        items=gameplay_component_enum
    )

    components: CollectionProperty(type=ComponentContainer)

#  Creates the flatbuffer array for the components under an object's live_link_settings
def builder_create_gameplay_components(
    builder,
    live_link_settings,
    source_object,
    dependency_graph,
    exporter,
    export_occurrence=None,
):
    out_flatbuffers_object = None

    if len(live_link_settings.components) > 0:
        flatbuffer_components = []
        for component in live_link_settings.components:
            flatbuffer_components.append(component.create_flatbuffers_object(
                builder,
                source_object=source_object,
                dependency_graph=dependency_graph,
                exporter=exporter,
                export_occurrence=export_occurrence,
            ))

        out_flatbuffers_object = build_offset_vector(
            builder,
            flatbuffer_components,
            Object.ObjectStartComponentsVector,
        )
    
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

        if settings.add_type == Component_SkyAtmosphere.type_name:
            if obj.type != 'LIGHT' or obj.data.type != 'SUN':
                self.report({'WARNING'}, "Sky Atmosphere can only be added to a Sun light")
                return {'CANCELLED'}
            if any(component.type == settings.add_type for component in settings.components):
                self.report({'WARNING'}, "Sky Atmosphere already exists on this object")
                return {'CANCELLED'}

        if settings.add_type == Component_CloudSystem.type_name:
            if obj.type != 'LIGHT' or obj.data.type != 'SUN':
                self.report({'WARNING'}, "Cloud System can only be added to a Sun light")
                return {'CANCELLED'}
            if not any(component.type == Component_SkyAtmosphere.type_name
                       for component in settings.components):
                self.report({'WARNING'}, "Add Sky Atmosphere to this Sun before Cloud System")
                return {'CANCELLED'}
            if any(component.type == settings.add_type for component in settings.components):
                self.report({'WARNING'}, "Cloud System already exists on this object")
                return {'CANCELLED'}

        if settings.add_type in {Component_Part.type_name, Component_AttachmentPoint.type_name}:
            if any(component.type == settings.add_type for component in settings.components):
                self.report({'WARNING'}, f"{settings.add_type.replace('_', ' ').title()} already exists on this object")
                return {'CANCELLED'}

        new_component = settings.components.add()
        new_component.type = settings.add_type

        if settings.add_type == Component_CloudSystem.type_name:
            cumulus = new_component.cloud_system.layers.add()
            cumulus.profile = 'CUMULUS'
            cumulus.seed_offset = 0
            apply_cloud_profile_defaults(cumulus)
            cirrus = new_component.cloud_system.layers.add()
            cirrus.profile = 'CIRRUS'
            cirrus.seed_offset = 1
            apply_cloud_profile_defaults(cirrus)

        queue_object_update(obj, update_reason="gameplay_component_added")

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
            queue_object_update(obj, update_reason="gameplay_component_removed")
            return {'FINISHED'}
        else:
            self.report({'WARNING'}, "Invalid index")
            return {'CANCELLED'}

def get_cloud_system_component(context, component_index):
    obj = context.object
    if not obj or component_index < 0 or component_index >= len(obj.live_link_settings.components):
        return None
    component = obj.live_link_settings.components[component_index]
    return component.cloud_system if component.type == Component_CloudSystem.type_name else None

class OBJECT_OT_cloud_layer_add(Operator):
    bl_idname = "object.live_link_cloud_layer_add"
    bl_label = "Add Cloud Layer"
    component_index: IntProperty()

    def execute(self, context):
        cloud = get_cloud_system_component(context, self.component_index)
        if cloud is None:
            return {'CANCELLED'}
        if len(cloud.layers) >= 4:
            self.report({'WARNING'}, "Cloud System supports at most four layers")
            return {'CANCELLED'}
        layer = cloud.layers.add()
        layer.profile = 'CUMULUS'
        layer.seed_offset = len(cloud.layers) - 1
        apply_cloud_profile_defaults(layer)
        queue_object_update(context.object, update_reason="cloud_layer_added")
        return {'FINISHED'}

class OBJECT_OT_cloud_layer_remove(Operator):
    bl_idname = "object.live_link_cloud_layer_remove"
    bl_label = "Remove Cloud Layer"
    component_index: IntProperty()
    layer_index: IntProperty()

    def execute(self, context):
        cloud = get_cloud_system_component(context, self.component_index)
        if cloud is None or not 0 <= self.layer_index < len(cloud.layers):
            return {'CANCELLED'}
        cloud.layers.remove(self.layer_index)
        queue_object_update(context.object, update_reason="cloud_layer_removed")
        return {'FINISHED'}

class OBJECT_OT_cloud_layer_move(Operator):
    bl_idname = "object.live_link_cloud_layer_move"
    bl_label = "Move Cloud Layer"
    component_index: IntProperty()
    layer_index: IntProperty()
    direction: IntProperty(default=1, min=-1, max=1)

    def execute(self, context):
        cloud = get_cloud_system_component(context, self.component_index)
        target = self.layer_index + self.direction
        if cloud is None or not 0 <= self.layer_index < len(cloud.layers) or not 0 <= target < len(cloud.layers):
            return {'CANCELLED'}
        cloud.layers.move(self.layer_index, target)
        queue_object_update(context.object, update_reason="cloud_layer_reordered")
        return {'FINISHED'}

class OBJECT_OT_cloud_layer_reset_profile(Operator):
    bl_idname = "object.live_link_cloud_layer_reset_profile"
    bl_label = "Reset Cloud Layer to Profile"
    component_index: IntProperty()
    layer_index: IntProperty()

    def execute(self, context):
        cloud = get_cloud_system_component(context, self.component_index)
        if cloud is None or not 0 <= self.layer_index < len(cloud.layers):
            return {'CANCELLED'}
        apply_cloud_profile_defaults(cloud.layers[self.layer_index])
        queue_object_update(context.object, update_reason="cloud_layer_profile_reset")
        return {'FINISHED'}

# ------------------------------------------------------------
# Utility function to auto-draw groups
# ------------------------------------------------------------

def draw_property_group(layout, group):
    for prop in group.bl_rna.properties:
        if prop.identifier != "rna_type":
            layout.prop(group, prop.identifier)

def draw_cloud_system(layout, cloud, component_index):
    layout.prop(cloud, "enabled")
    layout.prop(cloud, "seed")
    layout.prop(cloud, "weather_world_scale_m")
    layout.prop(cloud, "wind_direction")
    layout.prop(cloud, "wind_speed_m_s")
    layout.prop(cloud, "shadow_enabled")
    if cloud.shadow_enabled:
        layout.prop(cloud, "shadow_extent_m")

    if len(cloud.layers) > 2:
        warning = layout.box()
        warning.label(text="Three or four layers may exceed the 4 ms target", icon='ERROR')
    for layer_index, layer in enumerate(cloud.layers):
        layer_box = layout.box()
        header = layer_box.row(align=True)
        header.prop(layer, "enabled", text="")
        header.prop(layer, "profile", text=f"Layer {layer_index + 1}")
        up = header.operator(OBJECT_OT_cloud_layer_move.bl_idname, text="", icon='TRIA_UP')
        up.component_index, up.layer_index, up.direction = component_index, layer_index, -1
        down = header.operator(OBJECT_OT_cloud_layer_move.bl_idname, text="", icon='TRIA_DOWN')
        down.component_index, down.layer_index, down.direction = component_index, layer_index, 1
        remove = header.operator(OBJECT_OT_cloud_layer_remove.bl_idname, text="", icon='X')
        remove.component_index, remove.layer_index = component_index, layer_index
        if not layer.enabled:
            continue
        primary = layer_box.column(align=True)
        primary.prop(layer, "base_altitude_m")
        primary.prop(layer, "thickness_m")
        primary.prop(layer, "coverage")
        primary.prop(layer, "density")
        advanced = layer_box.box()
        advanced.label(text="Advanced")
        for property_name in (
            "seed_offset", "shape_scale_m", "detail_scale_m", "erosion", "anvil_bias",
            "wind_multiplier", "phase_forward", "phase_backward", "phase_blend",
            "ambient_scale", "multi_scattering_strength"):
            advanced.prop(layer, property_name)
        reset = advanced.operator(OBJECT_OT_cloud_layer_reset_profile.bl_idname,
                                  text="Reset to Profile", icon='FILE_REFRESH')
        reset.component_index, reset.layer_index = component_index, layer_index

    add = layout.operator(OBJECT_OT_cloud_layer_add.bl_idname, text="Add Cloud Layer", icon='ADD')
    add.component_index = component_index
    if len(cloud.layers) >= 4:
        layout.label(text="Four-layer maximum reached")

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
                    if component.type == Component_CloudSystem.type_name:
                        draw_cloud_system(box, group, i)
                    else:
                        draw_property_group(box, group)


# ------------------------------------------------------------
#  Classes we register with blender
# ------------------------------------------------------------

classes_to_register = [ 
    # Main Live Link Operators
    OpLiveLinkSendFullUpdate,
    OpLiveLinkCompareNativePythonExport,
    OpLiveLinkSendReset,
    OpLiveLinkResetConnection,
    OpLiveLinkSaveToFile,

    # View 3D Panel
    LiveLinkView3DPanel,

    # Custom Property Group System
    CloudLayerSettings,
    *COMPONENT_CLASSES,
    ComponentContainer,
    LiveLinkObjectSettings,
    OBJECT_OT_add_custom_item,
    OBJECT_OT_remove_custom_item,
    OBJECT_OT_cloud_layer_add,
    OBJECT_OT_cloud_layer_remove,
    OBJECT_OT_cloud_layer_move,
    OBJECT_OT_cloud_layer_reset_profile,
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
    if depsgraph_update_post_callback not in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.append(depsgraph_update_post_callback)
    if automatic_initial_full_update_load_post not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(automatic_initial_full_update_load_post)

    # add to searchable menu
    bpy.types.VIEW3D_MT_object.append(menu_func)

    # Setup live link settings on type Object
    bpy.types.Object.live_link_settings = bpy.props.PointerProperty(type=LiveLinkObjectSettings)
    bpy.types.Scene.live_link_use_python_export_fallback = bpy.props.BoolProperty(
        name="Use Python Export Fallback",
        description="Use the Python FlatBuffers exporter even when native Live Link export is available",
        default=False,
        update=live_link_python_export_fallback_update,
    )

    # Enabled add-ons normally register before the startup file is loaded and
    # are scheduled by load_post. This also covers enabling the add-on after a
    # file has already been opened.
    if getattr(bpy.data, "filepath", ""):
        schedule_automatic_initial_full_update(update_reason="addon_registered_with_open_file")

def unregister():
    automatic_initial_full_update_timer.pending = False
    automatic_initial_full_update_timer.status = "idle"
    if bpy.app.timers.is_registered(automatic_initial_full_update_timer):
        bpy.app.timers.unregister(automatic_initial_full_update_timer)

    # clean up live link connection
    global live_link_connection
    del live_link_connection

    # Unregister classes
    for cls in reversed(classes_to_register):
        bpy.utils.unregister_class(cls)

    # Remove depsgraph update post callback
    if depsgraph_update_post_callback in bpy.app.handlers.depsgraph_update_post:
        bpy.app.handlers.depsgraph_update_post.remove(depsgraph_update_post_callback)
    if automatic_initial_full_update_load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(automatic_initial_full_update_load_post)

    # remove from searchable menu
    bpy.types.VIEW3D_MT_object.remove(menu_func)

    # Delete Live Link Settings
    del bpy.types.Object.live_link_settings
    if hasattr(bpy.types.Scene, "live_link_use_python_export_fallback"):
        del bpy.types.Scene.live_link_use_python_export_fallback

# This allows you to run the script directly from Blender's Text editor
if __name__ == "__main__":
    register()
