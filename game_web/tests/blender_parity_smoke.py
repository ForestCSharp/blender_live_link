"""Background Blender -> current bridge: textured mesh, three light types, delta edit.

Run with Blender --background --factory-startup --python this_file.
A deterministic editor pose substitutes for the absent background-mode viewport.
The exporter and transport remain unmodified in the repository.
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
extension.get_editor_camera_snapshot = lambda context=None: (6,-10,6, -0.4,0.8,-0.4, 0,0,1)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
bpy.ops.mesh.primitive_cube_add(location=(0,0,1))
cube = bpy.context.object
cube.name = 'Textured parity cube'
material = bpy.data.materials.new('Parity textured PBR')
material.use_nodes = True
bsdf = material.node_tree.nodes.get('Principled BSDF')
bsdf.inputs['Metallic'].default_value = 0.25
bsdf.inputs['Roughness'].default_value = 0.4
image = bpy.data.images.new('Parity RGBA corners',width=2,height=2,alpha=True)
image.pixels = [1,0,0,1, 0,1,0,1, 0,0,1,1, 1,1,0,1]
texture = material.node_tree.nodes.new('ShaderNodeTexImage')
texture.image = image
material.node_tree.links.new(texture.outputs['Color'],bsdf.inputs['Base Color'])
cube.data.materials.append(material)
bpy.ops.mesh.primitive_plane_add(size=12,location=(0,0,-0.05))
for kind, location, energy in [('POINT',(-3,-3,4),100),('SPOT',(3,-2,5),200),('SUN',(0,0,5),1)]:
    data = bpy.data.lights.new('Parity '+kind,kind)
    data.energy = energy
    light = bpy.data.objects.new(data.name,data)
    bpy.context.collection.objects.link(light)
    light.location = location
    light.rotation_euler = (0.4,-0.2,-0.2)
bpy.context.view_layer.update()
connection = extension.live_link_connection
path = Path('/tmp/game_web_parity_blender.bin')
connection.save_to_file(list(bpy.context.scene.objects),str(path))
transport = extension.LiveLinkTransport()
deadline = time.monotonic()+15
while transport.state != 'connected':
    transport.poll()
    if time.monotonic()>deadline: raise RuntimeError('Bridge did not connect')
    time.sleep(0.01)

def send(data):
    assert transport.submit(data)
    while transport.payload is not None:
        transport.poll()
        if time.monotonic()>deadline: raise RuntimeError('Send timed out')
        time.sleep(0.01)

send(path.read_bytes())
cube.location.x = 0.5
bpy.context.view_layer.update()
send(connection.make_update([cube],[],update_reason='web_parity_transform'))
transport.close()
extension.unregister()
print('GAME_WEB_PARITY_BLENDER_OK',path)
