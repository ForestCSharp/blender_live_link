"""Read Starter .blend assets without saving them; export through existing Live Link.
Run Blender --background --factory-startup --python this_file with bridge running.
"""
import importlib
from pathlib import Path
import sys
import time
import bpy
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root.parent))
extension=importlib.import_module(root.name+'.extension_main');extension.register()
bpy.context.scene.live_link_use_python_export_fallback=True
extension.get_editor_camera_snapshot=lambda context=None:(30,-45,25, -.5,.8,-.3, 0,0,1)
bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
for path in sorted((root/'blend_files/Mechs/Starter').glob('*.blend')):
    with bpy.data.libraries.load(str(path),link=False) as (source,target):target.objects=source.objects
    for obj in target.objects:
        if obj and obj.name not in bpy.context.scene.objects:bpy.context.collection.objects.link(obj)
for index in range(2):
    obj=bpy.data.objects.new('Web Pilot '+str(index+1),None);bpy.context.collection.objects.link(obj);obj.location=(index*15,0,12)
    component=obj.live_link_settings.components.add();component.type='CHARACTER';component.player.player_controlled=index==0
    component.player.height=6;component.player.radius=1
    if index==0:
        camera=obj.live_link_settings.components.add();camera.type='CAMERA_CONTROL';camera.camera_control.follow_distance=20;camera.camera_control.follow_speed=5
bpy.ops.mesh.primitive_cube_add(location=(0,0,-1));floor=bpy.context.object;floor.name='Web Ground';floor.scale=(50,50,1)
bpy.ops.rigidbody.object_add();floor.rigid_body.type='PASSIVE'
bpy.context.view_layer.update()
for obj in bpy.context.scene.objects:
    components=[c.type for c in obj.live_link_settings.components]
    if components:print('GAMEPLAY_OBJECT',obj.name,components)
path=Path('/tmp/game_web_starter_mechs.bin')
extension.live_link_connection.save_to_file(list(bpy.context.scene.objects),str(path))
transport=extension.LiveLinkTransport();deadline=time.monotonic()+30
while transport.state!='connected':
    transport.poll()
    if time.monotonic()>deadline:raise RuntimeError('Connection timeout')
    time.sleep(.01)
assert transport.submit(path.read_bytes())
while transport.payload is not None:
    transport.poll()
    if time.monotonic()>deadline:raise RuntimeError('Send timeout')
    time.sleep(.01)
transport.close();extension.unregister()
print('GAME_WEB_STARTER_MECHS_OK',path,path.stat().st_size)
