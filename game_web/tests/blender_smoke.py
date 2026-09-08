"""With bridge running: blender --background --factory-startup --python game_web/tests/blender_smoke.py
Writes /tmp/game_web_blender_snapshot.bin using the existing exporter and sends over LiveLinkTransport.
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
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
bpy.ops.mesh.primitive_cube_add(location=(2, -1, 0.5))
obj = bpy.context.object
obj.name = 'Web smoke asymmetric cube'
obj.scale = (2, 0.7, 1.3)
obj.rotation_euler = (0.2, 0.1, 0.5)
material = bpy.data.materials.new('Web smoke orange')
material.diffuse_color = (0.8, 0.25, 0.08, 1)
material.use_nodes = True
material.node_tree.nodes.get('Principled BSDF').inputs['Base Color'].default_value = material.diffuse_color
obj.data.materials.append(material)
bpy.context.view_layer.update()
connection = extension.live_link_connection
path = Path('/tmp/game_web_blender_snapshot.bin')
connection.save_to_file(list(bpy.context.scene.objects), str(path))
data = path.read_bytes()
transport = extension.LiveLinkTransport()
deadline = time.monotonic() + 10
while transport.state != 'connected':
    transport.poll()
    if time.monotonic() > deadline:
        raise RuntimeError('Bridge not listening')
    time.sleep(0.01)
assert transport.submit(data)
while transport.payload is not None:
    transport.poll()
    if time.monotonic() > deadline:
        raise RuntimeError('Export send timed out')
    time.sleep(0.01)
transport.close()
print('GAME_WEB_BLENDER_SMOKE_OK', path, len(data))
extension.unregister()
