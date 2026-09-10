"""Run with Blender --background --factory-startup --python this_file.
Exports unmodified wire data to the running bridge and a /tmp snapshot.
"""
import importlib
from pathlib import Path
import sys
import time
import bpy

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root.parent))
extension = importlib.import_module(root.name + '.extension_main')
extension.register()
bpy.context.scene.live_link_use_python_export_fallback = True
extension.get_editor_camera_snapshot = lambda context=None: (8,-12,8, -.4,.8,-.4, 0,0,1)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
for name, location, scale, kind in [('Physics ground',(0,0,-.5),(6,6,.5),'PASSIVE'),
                                    ('Physics falling cube',(0,0,4),(.5,.5,.5),'ACTIVE')]:
    bpy.ops.mesh.primitive_cube_add(location=location)
    obj=bpy.context.object; obj.name=name; obj.scale=scale
    bpy.ops.rigidbody.object_add(); obj.rigid_body.type=kind; obj.rigid_body.mass=2
bpy.context.view_layer.update()
path=Path('/tmp/game_web_physics_blender.bin')
extension.live_link_connection.save_to_file(list(bpy.context.scene.objects),str(path))
transport=extension.LiveLinkTransport(); deadline=time.monotonic()+15
while transport.state!='connected':
    transport.poll()
    if time.monotonic()>deadline: raise RuntimeError('Bridge connection timed out')
    time.sleep(.01)
assert transport.submit(path.read_bytes())
while transport.payload is not None:
    transport.poll()
    if time.monotonic()>deadline: raise RuntimeError('Export timed out')
    time.sleep(.01)
transport.close(); extension.unregister()
print('GAME_WEB_PHYSICS_BLENDER_OK',path)
