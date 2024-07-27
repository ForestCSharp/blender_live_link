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

class BlenderLiveLinkInit(bpy.types.Operator):
    """ Blender Live Link Init """          # Use this as a tooltip for menu items and buttons.
    bl_idname = "live_link.init"            # Unique identifier for buttons and menu items to reference.
    bl_label = "Live Link: Init"            # Display name in the interface.
    bl_options = {'REGISTER', 'UNDO'}       # Enable undo for the operator.

    def execute(self, context):        # execute() is called when running the operator.
        builder = flatbuffers.Builder(1024)

        name_string = builder.CreateString('test')

        Blender.LiveLink.Scene.Start(builder)
        Blender.LiveLink.Scene.AddName(builder, name_string)
        live_link_scene = Blender.LiveLink.Scene.End(builder)

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
