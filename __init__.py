# Reminder to self: Need to zip up folder to install as add-on

# legacy bl_info Now using blender_manifest.toml
#bl_info = {
#    "name": "Blender Live Link",
#    "blender": (4, 00, 0),
#    "category": "Object",
#}

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
from .compiled_schemas.python.Blender.LiveLink import GameplayComponent
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentCameraControl
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentCharacter
from .compiled_schemas.python.Blender.LiveLink import GameplayComponentContainer
from .compiled_schemas.python.Blender.LiveLink import Image
from .compiled_schemas.python.Blender.LiveLink import Light
from .compiled_schemas.python.Blender.LiveLink import LightType
from .compiled_schemas.python.Blender.LiveLink import Material
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
            # best-effort; ignore if platform doesn't support it
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

    def get_mesh(self, obj, dependency_graph):
        # Evaluate Modifiers
        obj_evaluated = obj.evaluated_get(dependency_graph)
        # Get a copy of the mesh data
        mesh = obj_evaluated.data 

        if obj.mode == 'EDIT':
            # TODO: we shouldn't do this for meshes that are too complex
            bm = bmesh.from_edit_mesh(mesh)
            new_mesh = bpy.data.meshes.new("Modified_Mesh")
            bm.to_mesh(new_mesh)

            bm = bmesh.new()
            bm.from_mesh(new_mesh)
            bmesh.ops.triangulate(bm, faces=bm.faces[:])
            bm.to_mesh(new_mesh)

            return new_mesh 
        else:
            # create a new bmesh and set its data from our mesh data
            bm = bmesh.new()
            bm.from_mesh(mesh)
            # triangulate the mesh
            bmesh.ops.triangulate(bm, faces=bm.faces[:])
            # assign back to mesh
            bm.to_mesh(mesh)
            return mesh

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
 
    def make_flatbuffer_object(self, builder, obj, dependency_graph, referenced_materials):
        # Allocate string for object name
        object_name = builder.CreateString(obj.name)

        # Mesh Data
        fb_mesh = None
        if obj.type == 'MESH': 
            mesh = self.get_mesh(obj, dependency_graph)

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
            rounded_uvs = (uvs * 1e6).astype(np.int32)  # prevent float issues
            dtype = np.dtype([('v', np.int32), ('u', np.int32), ('v2', np.int32)])
            keys = np.zeros((loop_count,), dtype=dtype)
            keys['v']  = loop_vertex_indices
            keys['u']  = rounded_uvs[:, 0]
            keys['v2'] = rounded_uvs[:, 1]

            unique_keys, inverse_indices = np.unique(keys, return_inverse=True)
            new_indices = inverse_indices.astype(np.int32)
            new_vertex_count = len(unique_keys)

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

            # TODO: if we find an armature build up skinned vertex info in new field
            # Check for armature
            mesh_armature = self.get_mesh_armature(obj)
            if mesh_armature is not None:
                print("Mesh is skinned!")

            # --- Build FlatBuffer Mesh ---
            Mesh.Start(builder)
            Mesh.AddPositions(builder, mesh_positions_fb)
            Mesh.AddNormals(builder, mesh_normals_fb)
            Mesh.AddTexcoords(builder, mesh_uvs_fb)
            Mesh.AddIndices(builder, mesh_indices_fb)
            Mesh.AddMaterialIds(builder, material_ids_fb)
            fb_mesh = Mesh.End(builder)

        # Light Info
        light = None
        if obj.type == 'LIGHT':
            light_data = obj.data

            Light.Start(builder)

            # Light Color
            light_color = Vec3.CreateVec3(builder, light_data.color.r, light_data.color.g, light_data.color.b)
            Light.AddColor(builder, light_color)

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
            
            # Light Type
            light_type = determine_light_type(light_data)
            Light.AddType(builder, light_type)

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

            light = Light.End(builder)

        # Armature Data
        armature = None
        if obj.type == 'ARMATURE':
            print("Found Armature!")

            # Dictionary to store inverse bind matrices
            inverse_bind_matrices = {}

            # Iterate through all bones in the armature
            for bone in obj.data.bones:
                break
                # Get the bone's world-space matrix in the bind pose
                # bone.matrix_local is the bone's transformation in armature space
                bone_matrix = obj.matrix_world @ bone.matrix_local
                
                # Compute the inverse bind matrix
                inverse_bind_matrix = bone_matrix.inverted()
                
                # Store the matrix (convert to list for easier export if needed)
                inverse_bind_matrices[bone.name] = inverse_bind_matrix
                
                # Print for verification
                print(f"Bone: {bone.name}")
                print(f"Inverse Bind Matrix:\n{inverse_bind_matrix}") 

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
        if fb_mesh is not None:
            Object.AddMesh(builder, fb_mesh)

        # Add Object Light Data if it exists
        if light is not None:
            Object.AddLight(builder, light)

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
    def make_update(self, in_object_list, in_deleted_object_uids, reset=False):
        # Evaluate Depsgraph
        dependency_graph = bpy.context.evaluated_depsgraph_get()

        # init flatbuffers builder
        builder = flatbuffers.Builder(0)

        # referenced materials, keyed by session_uid and updated in self.make_flatbuffer_object
        referenced_materials = {}

        # Build up objects to be added to scene objects vector
        live_link_objects = []
        for blender_object in in_object_list: 
            # Only add if enable_live_link is set
            if (blender_object.live_link_settings.enable_live_link):
                live_link_objects.append(
                    self.make_flatbuffer_object(
                        builder, 
                        blender_object, 
                        dependency_graph, 
                        referenced_materials
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
        flatbuffer_materials = []
        for material_id, material in referenced_materials.items():
            class MaterialData:
                def __init__(self):
                    self.base_color = (1,1,1,1)
                    self.base_color_image_id = None 
                    self.metallic = 0.0
                    self.metallic_image_id = None
                    self.roughness = 0.0
                    self.roughness_image_id = None


            # Helper to register an image id for a material_node_input if it contains a valid image
            def extract_image_id(material_node_input):
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

            flatbuffer_material = Material.End(builder)
            flatbuffer_materials.append(flatbuffer_material)


        Update.UpdateStartMaterialsVector(builder, len(flatbuffer_materials))   
        for material in reversed(flatbuffer_materials):
            builder.PrependUOffsetTRelative(material)
        update_materials = builder.EndVector()

        flatbuffer_images = []
        for image_id, image in referenced_images.items():
            print("Found Referenced Image with ID: " + str(image_id))
            
            image_width, image_height = image.size

            # Ensure pixels are loaded
            _ = image.pixels[0]

            # Get float32 pixel array (RGBA)
            pixels_f32 = np.array(image.pixels[:], dtype=np.float32)

            # Reinterpret the float32 array as uint8 bytes
            pixels_bytes = pixels_f32.view(np.uint8)

            # Now pass to FlatBuffers
            flatbuffer_image_data = builder.CreateNumpyVector(pixels_bytes)

            Image.Start(builder)
            Image.AddUniqueId(builder, image_id)
            Image.AddWidth(builder, image_width)
            Image.AddHeight(builder, image_height)
            Image.AddData(builder, flatbuffer_image_data) 

            flatbuffer_image = Image.End(builder)
            flatbuffer_images.append(flatbuffer_image)

        Update.UpdateStartImagesVector(builder, len(flatbuffer_images))   
        for image in reversed(flatbuffer_images):
            builder.PrependUOffsetTRelative(image)
        update_images = builder.EndVector()            

        Update.Start(builder)

        # Add objects vector to scene
        Update.AddObjects(builder, update_objects)
        Update.AddDeletedObjectUids(builder, update_deleted_object_uids)
        Update.AddMaterials(builder, update_materials)
        Update.AddImages(builder, update_images)
        Update.AddReset(builder, reset)

        # finalize scene flatbuffer
        live_link_scene = Update.End(builder)

        builder.FinishSizePrefixed(live_link_scene)
        
        return builder.Output() 

    def send_object_list(self, updated_objects, deleted_object_uids):
        self.send(self.make_update(updated_objects, deleted_object_uids))

    def save_to_file(self, in_objects, in_filename):
        update = self.make_update(in_objects, [])
        with open(in_filename, 'wb') as f:
            f.write(update)

    def send_reset(self):
        self.send(self.make_update([], [], True))

live_link_connection = []

# Callback when depsgraph has finished updating
@persistent
def depsgraph_update_post_callback(scene, depsgraph):
    if not depsgraph_update_post_callback.enabled:
        return

    updated_objects = []
    deleted_object_uids = []

    # Determine if any objects were deleted
    current_objects = set(obj.session_uid for obj in scene.objects)

    # Track the objects at the last update
    if hasattr(depsgraph_update_post_callback, "previous_objects"):
        previous_objects = depsgraph_update_post_callback.previous_objects
        # Find the difference (deleted objects)
        deleted_object_uids = list(previous_objects - current_objects)
        if len(deleted_object_uids) > 0:
            print(f"Deleted Objects UIDs: {', '.join(map(str, deleted_object_uids))}")
    
    # Store the current object names for the next update
    depsgraph_update_post_callback.previous_objects = current_objects

    for update in depsgraph.updates:
        update_id = update.id
        if isinstance(update_id, bpy.types.Object):
            if update.is_updated_transform or update.is_updated_geometry:
                # appending update_id won't work, need to look up object in scene.objects
                updated_object = scene.objects[update_id.name]
                updated_objects.append(updated_object)

    if len(updated_objects) > 0 or len(deleted_object_uids) > 0: 
        # Temporarily disable depsgraph_update_post_callback to prevent infinite recursion
        depsgraph_update_post_callback.enabled = False 

        live_link_connection.send_object_list(
            updated_objects = updated_objects, 
            deleted_object_uids = deleted_object_uids
        )

        # Re-Enable depsgraph_update_post_callback
        depsgraph_update_post_callback.enabled = True 

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
        live_link_connection.send_object_list(
            updated_objects = list(bpy.context.scene.objects), 
            deleted_object_uids = []
        ) 
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
        live_link_connection.send_reset()
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
            self.filepath
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

