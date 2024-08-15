# Reminder to self: Need to zip up folder to install as add-on

bl_info = {
    "name": "Live Link Init",
    "blender": (4, 00, 0),
    "category": "Object",
}

import bpy
import sys

from os.path import dirname, realpath
import sys
sys.path.append(dirname(realpath(__file__)) + "/compiled_schemas/python") 

import flatbuffers
import Blender.LiveLink.Scene
import Blender.LiveLink.Object
import Blender.LiveLink.Vec3

import socket

class BlenderLiveLinkInit(bpy.types.Operator):
    """ Blender Live Link Init """          # Use this as a tooltip for menu items and buttons.
    bl_idname = "live_link.init"            # Unique identifier for buttons and menu items to reference.
    bl_label = "Live Link: Init"            # Display name in the interface.
    bl_options = {'REGISTER', 'UNDO'}       # Enable undo for the operator.

    def execute(self, context):

        # init flatbuffers builder
        builder = flatbuffers.Builder(0)

        #FCS TODO: Smarter way to iterate collection hierarchy to maintain parentage?
        # Build up objects to be added to scene objects vector
        live_link_objects = []
        for obj in bpy.context.scene.objects: 
            object_name = builder.CreateString(obj.name)
            Blender.LiveLink.Object.Start(builder)
            Blender.LiveLink.Object.AddName(builder, object_name)
            location_vec3 = Blender.LiveLink.Vec3.CreateVec3(builder,obj.location.x, obj.location.y, obj.location.z);
            Blender.LiveLink.Object.AddLocation(builder, location_vec3)
            live_link_object = Blender.LiveLink.Object.End(builder)
            live_link_objects.append(live_link_object)

        # actually create the scene objects vector
        Blender.LiveLink.Scene.SceneStartObjectsVector(builder, len(live_link_objects))
        for live_link_object in live_link_objects: 
            builder.PrependUOffsetTRelative(live_link_object)
        scene_objects = builder.EndVector()

        # create string for scene name
        scene_name = builder.CreateString(bpy.data.filepath)
        print("filepath: " + bpy.data.filepath)

        Blender.LiveLink.Scene.Start(builder)

        # set scene name
        Blender.LiveLink.Scene.AddName(builder, scene_name)

        # Add objects vector to scene
        Blender.LiveLink.Scene.AddObjects(builder, scene_objects)

        # finalize scene flatbuffer
        live_link_scene = Blender.LiveLink.Scene.End(builder)

        builder.Finish(live_link_scene)
        
        flatbuffer_data = builder.Output()

	    #FCS TODO: Store magic IP and Port numbers in some shared file
        HOST = '127.0.0.1'
        PORT = 65432

        my_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        my_socket.connect((HOST,PORT))
        my_socket.send(flatbuffer_data)
        my_socket.shutdown(socket.SHUT_RDWR)

        print(flatbuffer_data)
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
