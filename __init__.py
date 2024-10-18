# Reminder to self: Need to zip up folder to install as add-on

# legacy bl_info Now using blender_manifest.toml
#bl_info = {
#    "name": "Blender Live Link",
#    "blender": (4, 00, 0),
#    "category": "Object",
#}

import bpy
import bmesh
import sys
import socket

from os.path import dirname, realpath, basename, isfile, join
import glob

path_to_append = dirname(realpath(__file__)) + "/compiled_schemas/python"
if path_to_append not in sys.path:
    sys.path.append(path_to_append)

from .compiled_schemas.python import flatbuffers
from .compiled_schemas.python.Blender.LiveLink import Update
from .compiled_schemas.python.Blender.LiveLink import Object
from .compiled_schemas.python.Blender.LiveLink import Mesh
from .compiled_schemas.python.Blender.LiveLink import Vec4
from .compiled_schemas.python.Blender.LiveLink import Vec3
from .compiled_schemas.python.Blender.LiveLink import Quat
from .compiled_schemas.python.Blender.LiveLink import Vertex

def is_socket_connected(socket):
    try:
        socket.getpeername()
        return True
    except OSError:
        return False

class BlenderLiveLinkInit(bpy.types.Operator):
    """ Blender Live Link Send Update """       # Use this as a tooltip for menu items and buttons.
    bl_idname = "live_link.send_update"         # Unique identifier for buttons and menu items to reference.
    bl_label = "Live Link: Send Update"         # Display name in the interface.
    bl_options = {'REGISTER', 'UNDO'}           # Enable undo for the operator.

    #FCS TODO: Store magic IP and Port numbers in some shared file
    HOST = '127.0.0.1'
    PORT = 65432

    my_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    def make_flatbuffer_object(self, builder, obj, dependency_graph):
        # Allocate string for object name
        object_name = builder.CreateString(obj.name)

        # If we're a mesh object, setup mesh data 
        mesh = None

        if (obj.type == 'MESH'): 
            # Evaluate Modifiers
            obj_evaluated = obj.evaluated_get(dependency_graph)
            # Get Mesh Data
            mesh = obj_evaluated.data

            # Make sure Mesh is triangulated
            bm = bmesh.new()
            bm.from_mesh(mesh)
            bmesh.ops.triangulate(bm, faces=bm.faces[:])
            bm.to_mesh(mesh)

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

        # Object Mesh Data
        if (mesh != None):
            Object.AddMesh(builder, mesh)

        # End New Object add add to array
        live_link_object = Object.End(builder)

        return live_link_object

    # Creates an update for objects in in_object_list
    def make_update(self, in_object_list):
        # Evaluate Depsgraph
        dependency_graph = bpy.context.evaluated_depsgraph_get()

        # init flatbuffers builder
        builder = flatbuffers.Builder(0)

        # Build up objects to be added to scene objects vector
        live_link_objects = []
        for blender_object in in_object_list: 
            live_link_objects.append(self.make_flatbuffer_object(builder, blender_object, dependency_graph))

        # actually create the scene objects vector
        Update.UpdateStartObjectsVector(builder, len(live_link_objects))
        for live_link_object in live_link_objects: 
            builder.PrependUOffsetTRelative(live_link_object)

        scene_objects = builder.EndVector()

        # create string for scene name
        scene_name = builder.CreateString(bpy.data.filepath)
        print("filepath: " + bpy.data.filepath)

        Update.Start(builder)

        # Add objects vector to scene
        Update.AddObjects(builder, scene_objects)

        # finalize scene flatbuffer
        live_link_scene = Update.End(builder)

        builder.FinishSizePrefixed(live_link_scene)
        
        return builder.Output()


    # Called when operator is run
    def execute(self, context):

        if not is_socket_connected(self.my_socket):
            self.my_socket.connect((self.HOST,self.PORT))

        #FCS TODO: Smarter way to iterate collection hierarchy to maintain parentage?
        # Create Initial flatbuffer update message
        flatbuffer_update = self.make_update(bpy.context.scene.objects);
        self.my_socket.send(flatbuffer_update)

        # Testing sending one object at a time
        #for blender_object in bpy.context.scene.objects:
        #    flatbuffer_update = self.make_update([blender_object]);
        #    my_socket.send(flatbuffer_update)

        # FCS TODO: Remove Shutdown
        #my_socket.shutdown(socket.SHUT_RDWR)

        return {'FINISHED'}

def menu_func(self, context):
    self.layout.operator(BlenderLiveLinkInit.bl_idname)

def register():
    bpy.utils.register_class(BlenderLiveLinkInit)
    bpy.types.VIEW3D_MT_object.append(menu_func)  # Adds the new operator to an existing menu.

def unregister():
    bpy.utils.unregister_class(BlenderLiveLinkInit)

# This allows you to run the script directly from Blender's Text editor
# to test the add-on without having to install it.
if __name__ == "__main__":
    register()


#TODO: Helper function to uninstall, build, and reinstall add-on during dev
