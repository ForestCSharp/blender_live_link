# Reminder to self: Need to zip up folder to install as add-on

# legacy bl_info Now using blender_manifest.toml
#bl_info = {
#    "name": "Blender Live Link",
#    "blender": (4, 00, 0),
#    "category": "Object",
#}

import bpy
from bpy.props import (StringProperty, FloatProperty, FloatVectorProperty, PointerProperty, CollectionProperty, EnumProperty)
from bpy.types import (Panel, Operator, PropertyGroup)
from bpy.app.handlers import persistent

import bmesh
import sys
import socket
import traceback

from os.path import dirname, realpath, basename, isfile, join
import glob

path_to_append = dirname(realpath(__file__)) + "/compiled_schemas/python"
if path_to_append not in sys.path:
    sys.path.append(path_to_append)

from .compiled_schemas.python import flatbuffers
from .compiled_schemas.python.Blender.LiveLink import Update
from .compiled_schemas.python.Blender.LiveLink import Object
from .compiled_schemas.python.Blender.LiveLink import Mesh
from .compiled_schemas.python.Blender.LiveLink import LightType
from .compiled_schemas.python.Blender.LiveLink import Light
from .compiled_schemas.python.Blender.LiveLink import PointLight
from .compiled_schemas.python.Blender.LiveLink import SpotLight
from .compiled_schemas.python.Blender.LiveLink import SunLight
from .compiled_schemas.python.Blender.LiveLink import RigidBody 
from .compiled_schemas.python.Blender.LiveLink import Vec4
from .compiled_schemas.python.Blender.LiveLink import Vec3
from .compiled_schemas.python.Blender.LiveLink import Quat
from .compiled_schemas.python.Blender.LiveLink import Vertex

# Class to manage our live link connection
class LiveLinkConnection():
    def __init__(self):
        self.create_socket()
        
    def __del__(self):
       self.close_socket() 

    def create_socket(self):
        # Create a new socket object 
        self.my_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.my_socket.settimeout(5)

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
            return;

        try:
            self.my_socket.send(data)
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

    def make_flatbuffer_object(self, builder, obj, dependency_graph):
        # Allocate string for object name
        object_name = builder.CreateString(obj.name)

        # Mesh Data
        mesh = None
        if obj.type == 'MESH': 
            mesh = self.get_mesh(obj, dependency_graph)

            # Export Vertices
            blender_vertices = mesh.vertices
            
            Mesh.MeshStartVerticesVector(builder, len(blender_vertices))
            for blender_vertex in reversed(blender_vertices):
                position = blender_vertex.co.to_4d()
                normal = blender_vertex.normal.to_4d()
                Vertex.CreateVertex(
                    builder,
                    position.x, position.y, position.z, position.w,
                    normal.x, normal.y, normal.z, normal.w
                )
            mesh_vertices = builder.EndVector()

            # Export Indices
            indices = []
            for blender_polygon in mesh.polygons:
                # Should be able to assume triangles here because of triangulate call above
                indices.append(blender_polygon.vertices[0])
                indices.append(blender_polygon.vertices[1])
                indices.append(blender_polygon.vertices[2])

            Mesh.MeshStartIndicesVector(builder, len(indices))
            for index in reversed(indices):
                builder.PrependUint32(index)
            mesh_indices = builder.EndVector()

            Mesh.Start(builder)
            Mesh.AddVertices(builder, mesh_vertices)
            Mesh.AddIndices(builder, mesh_indices)
            mesh = Mesh.End(builder)

        # Light Info
        light = None
        if obj.type == 'LIGHT':
            light_data = obj.data

            print(f"Light Type: {light_data.type}")
            print(f"Light Color: {light_data.color}")

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
                    print("Adding Point Light Data")
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

            Light.AddUseShadow(builder, light_data.use_shadow);

            light = Light.End(builder)
        
        # Begin New Object 
        Object.Start(builder)
        
        # Object Name
        Object.AddName(builder, object_name)

        # Session UID (note that this is a fairly new addition to the python API)
        session_uid = obj.session_uid
        Object.AddUniqueId(builder, session_uid)

        is_visible = obj.visible_get()
        Object.AddVisibility(builder, is_visible)

        # Object Location
        location_vec3 = Vec3.CreateVec3(builder, obj.location.x, obj.location.y, obj.location.z)
        Object.AddLocation(builder, location_vec3)

        # Object Scale
        scale_vec3 = Vec3.CreateVec3(builder, obj.scale.x, obj.scale.y, obj.scale.z)
        Object.AddScale(builder, scale_vec3)

        # Object Rotation
        rot = obj.rotation_euler.to_quaternion()
        rotation_quat = Quat.CreateQuat(builder, rot.x, rot.y, rot.z, rot.w)
        Object.AddRotation(builder, rotation_quat)

        # Add Object Mesh Data if it exists
        if mesh != None:
            Object.AddMesh(builder, mesh)

        # Add Object Light Data if it exists
        if light != None:
            Object.AddLight(builder, light)

        # Add Rigid Body Data if it exists
        if obj.rigid_body:
            Object.AddRigidBody(builder, RigidBody.CreateRigidBody(
                builder, 
                isDynamic = obj.rigid_body.enabled,
                mass = obj.rigid_body.mass
            ))

        # End New Object add add to array
        live_link_object = Object.End(builder)

        return live_link_object

    # Creates an update for objects in in_object_list
    def make_update(self, in_object_list, in_deleted_object_uids, reset=False):
        # Evaluate Depsgraph
        dependency_graph = bpy.context.evaluated_depsgraph_get()

        # init flatbuffers builder
        builder = flatbuffers.Builder(0)

        # Build up objects to be added to scene objects vector
        live_link_objects = []
        for blender_object in in_object_list: 
            # Only add if enable_live_link is set
            if (blender_object.live_link_settings.enable_live_link):
                live_link_objects.append(self.make_flatbuffer_object(builder, blender_object, dependency_graph))

        # actually create the scene objects vector
        Update.UpdateStartObjectsVector(builder, len(live_link_objects))
        for live_link_object in live_link_objects: 
            builder.PrependUOffsetTRelative(live_link_object)
        update_objects = builder.EndVector()

        Update.UpdateStartDeletedObjectUidsVector(builder, len(in_deleted_object_uids))
        for deleted_object_uid in in_deleted_object_uids:
            builder.PrependInt32(deleted_object_uid)
        update_deleted_object_uids = builder.EndVector()

        Update.Start(builder)

        # Add objects vector to scene
        Update.AddObjects(builder, update_objects)
        Update.AddDeletedObjectUids(builder, update_deleted_object_uids)
        Update.AddReset(builder, reset)

        # finalize scene flatbuffer
        live_link_scene = Update.End(builder)

        builder.FinishSizePrefixed(live_link_scene)
        
        return builder.Output() 

    def send_object_list(self, updated_objects, deleted_object_uids):
        self.send(self.make_update(updated_objects, deleted_object_uids))

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

    if len(updated_objects) > 0: 
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

# Begin LiveLinkView3DPanel
class LiveLinkView3DPanel(bpy.types.Panel):
    bl_label = "Blender Live Link"
    bl_idname = "LiveLink_View3D_Panel"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'BlenderLiveLink'
    
    def draw(self, context):
        layout = self.layout
        scene = context.scene
        layout.operator("live_link.send_full_update", text="Full Update")  
        layout.operator("live_link.send_reset", text="Send Reset")  
        layout.operator("live_link.reset_connection", text="Reset Connection")
# End LiveLinkView3DPanel

def menu_func(self, context):
    self.layout.operator(OpLiveLinkSendFullUpdate.bl_idname)
    self.layout.operator(OpLiveLinkSendReset.bl_idname)
    self.layout.operator(OpLiveLinkResetConnection.bl_idname)


# ------------------------------------------------------------
# Define Gameplay Components 
# ------------------------------------------------------------

class GameplayComponent(PropertyGroup):
    # Blender UI Info
    type_name = 'INVALID'
    label = 'INVALID'

    @classmethod
    def enum_info(cls):
        return (cls.type_name, cls.label, '')

class GameplayComponent_Player(GameplayComponent):
    # Blender UI Info
    type_name = 'PLAYER'
    label = 'Player'

    # Properties
    move_speed: FloatProperty(name="Move Speed", default=1.0)

class GameplayComponent_CameraControl(GameplayComponent):
    # Blender UI Info
    type_name = 'CAMERA_CONTROL'
    label = 'Camera Control'

    # Properties
    follow_distance: FloatProperty(name="Follow Distance", default=1.0)

gameplay_component_enum = [
    GameplayComponent_Player.enum_info(),
    GameplayComponent_CameraControl.enum_info(),
]

TYPE_TO_GROUP = {
    GameplayComponent_Player.type_name: 'player',
    GameplayComponent_CameraControl.type_name: 'camera_control'
}

# ------------------------------------------------------------
# Container for polymorphic data
# ------------------------------------------------------------

class GameplayComponentContainer(PropertyGroup):
    type: StringProperty()

    # Only one of these should be set, based on type
    player:         PointerProperty(type=GameplayComponent_Player)
    camera_control: PointerProperty(type=GameplayComponent_CameraControl)

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

    items: CollectionProperty(type=GameplayComponentContainer)

# ------------------------------------------------------------
# Operators to add/remove selected type
# ------------------------------------------------------------

class OBJECT_OT_add_custom_item(Operator):
    bl_idname = "object.add_custom_item"
    bl_label = "Add Custom Property Group"

    def execute(self, context):
        obj = context.object
        settings = obj.live_link_settings

        new_item = settings.items.add()
        new_item.type = settings.add_type

        # Initialize based on type
        if new_item.type == GameplayComponent_Player.type_name:
            new_item.player.move_speed = 20.0
        elif new_item.type == GameplayComponent_CameraControl.type_name:
            new_item.camera_control.follow_distance = 100.0

        return {'FINISHED'}

class OBJECT_OT_remove_custom_item(Operator):
    bl_idname = "object.remove_custom_item"
    bl_label = "Remove Custom Property Group"

    index: bpy.props.IntProperty()

    def execute(self, context):
        obj = context.object
        settings = obj.live_link_settings

        if 0 <= self.index < len(settings.items):
            settings.items.remove(self.index)
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
        layout.operator("object.add_custom_item", text="Add Property Group")

        for i, item in enumerate(settings.items):
            box = layout.box()
            row = box.row()
            row.label(text=f"Item {i+1} ({item.type})", icon='DOT')
            row.operator("object.remove_custom_item", text="", icon="X").index = i

            group_name = TYPE_TO_GROUP.get(item.type)
            if group_name:
                group = getattr(item, group_name, None)
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

    # View 3D Panel
    LiveLinkView3DPanel,

    # Custom Property Group System
    GameplayComponent_Player,
    GameplayComponent_CameraControl,
    GameplayComponentContainer,
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

