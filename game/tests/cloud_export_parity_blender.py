"""Blender-background smoke test for native/Python Cloud System parity."""

import bpy
import os

bpy.ops.preferences.addon_enable(module="bl_ext.user_default.blender_live_link")


def add_component(settings, component_type):
    component = settings.components.add()
    component.type = component_type
    return component


bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)

light = bpy.data.lights.new("Cloud Sun", type='SUN')
light.use_shadow = True
sun = bpy.data.objects.new("Cloud Sun", light)
bpy.context.collection.objects.link(sun)
bpy.context.view_layer.objects.active = sun
sun.select_set(True)

settings = sun.live_link_settings
add_component(settings, 'SKY_ATMOSPHERE')
cloud_container = add_component(settings, 'CLOUD_SYSTEM')
cloud = cloud_container.cloud_system
cloud.enabled = True
cloud.seed = 0x1234567
cloud.weather_world_scale_m = 125000.0
cloud.wind_direction = (-0.6, 0.8)
cloud.wind_speed_m_s = 31.0
cloud.shadow_enabled = True
cloud.shadow_extent_m = 55000.0

for index, profile in enumerate(('STRATUS', 'CUMULUS', 'CUMULONIMBUS', 'CIRRUS')):
    layer = cloud.layers.add()
    layer.profile = profile
    layer.enabled = index != 2
    layer.seed_offset = 100 + index
    layer.base_altitude_m += index * 10.0
    layer.thickness_m += index * 20.0
    layer.coverage = index / 3.0
    layer.density = 0.5 + index * 0.25
    layer.shape_scale_m += index * 30.0
    layer.detail_scale_m += index * 40.0
    layer.erosion = 1.0 - index / 3.0
    layer.anvil_bias = index / 3.0
    layer.wind_multiplier = -1.0 + index
    layer.phase_forward = 0.2 + index * 0.1
    layer.phase_backward = -0.5 + index * 0.1
    layer.phase_blend = 0.25 + index * 0.2
    layer.ambient_scale = 0.5 + index * 0.1
    layer.multi_scattering_strength = index / 3.0

result = bpy.ops.live_link.compare_native_python_export()
if 'FINISHED' not in result:
    raise RuntimeError(f"Cloud native/Python parity failed: {result}")
print("CLOUD_EXPORT_PARITY_OK")

if os.environ.get("CLOUD_RUNTIME_SMOKE") == "1":
    from bl_ext.user_default.blender_live_link import extension_main
    if not extension_main.send_full_scene_update("cloud_runtime_smoke"):
        raise RuntimeError("Cloud runtime smoke could not send the scene to the game")
    print("CLOUD_RUNTIME_SCENE_SENT")
