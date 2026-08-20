#!/usr/bin/env python3
"""Send a synthetic cloud-and-ground scene to a running Live Link game."""

import os
import pathlib
import socket
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from compiled_schemas.python import flatbuffers
from compiled_schemas.python.Blender.LiveLink import CloudLayer
from compiled_schemas.python.Blender.LiveLink import CloudLayerProfile
from compiled_schemas.python.Blender.LiveLink import GameplayComponent
from compiled_schemas.python.Blender.LiveLink import GameplayComponentCloudSystem as CloudSystem
from compiled_schemas.python.Blender.LiveLink import GameplayComponentContainer as Container
from compiled_schemas.python.Blender.LiveLink import GameplayComponentSkyAtmosphere as Sky
from compiled_schemas.python.Blender.LiveLink import Light
from compiled_schemas.python.Blender.LiveLink import LightType
from compiled_schemas.python.Blender.LiveLink import Material
from compiled_schemas.python.Blender.LiveLink import Mesh
from compiled_schemas.python.Blender.LiveLink import Object
from compiled_schemas.python.Blender.LiveLink import Quat
from compiled_schemas.python.Blender.LiveLink import SunLight
from compiled_schemas.python.Blender.LiveLink import Update
from compiled_schemas.python.Blender.LiveLink import Vec2
from compiled_schemas.python.Blender.LiveLink import Vec3
from compiled_schemas.python.Blender.LiveLink import Vec4
from compiled_schemas.python.Blender.LiveLink import EditorCamera


SUN_ID = 9001
GROUND_ID = 9002
GROUND_MATERIAL_ID = 9101


def float_vector(builder, start_vector, values):
    start_vector(builder, len(values))
    for value in reversed(values):
        builder.PrependFloat32(value)
    return builder.EndVector()


def uint_vector(builder, start_vector, values):
    start_vector(builder, len(values))
    for value in reversed(values):
        builder.PrependUint32(value)
    return builder.EndVector()


def int_vector(builder, start_vector, values):
    start_vector(builder, len(values))
    for value in reversed(values):
        builder.PrependInt32(value)
    return builder.EndVector()


def layer(builder, profile, seed, base, thickness, coverage, density,
          shape_scale, detail_scale, erosion, anvil, phase_forward,
          phase_backward, phase_blend, ambient, multi_scattering):
    CloudLayer.Start(builder)
    CloudLayer.AddProfile(builder, profile)
    CloudLayer.AddSeedOffset(builder, seed)
    CloudLayer.AddBaseAltitudeM(builder, base)
    CloudLayer.AddThicknessM(builder, thickness)
    CloudLayer.AddCoverage(builder, coverage)
    CloudLayer.AddDensity(builder, density)
    CloudLayer.AddShapeScaleM(builder, shape_scale)
    CloudLayer.AddDetailScaleM(builder, detail_scale)
    CloudLayer.AddErosion(builder, erosion)
    CloudLayer.AddAnvilBias(builder, anvil)
    CloudLayer.AddPhaseForward(builder, phase_forward)
    CloudLayer.AddPhaseBackward(builder, phase_backward)
    CloudLayer.AddPhaseBlend(builder, phase_blend)
    CloudLayer.AddAmbientScale(builder, ambient)
    CloudLayer.AddMultiScatteringStrength(builder, multi_scattering)
    return CloudLayer.End(builder)


builder = flatbuffers.Builder(4096)
cumulus = layer(builder, CloudLayerProfile.CloudLayerProfile.Cumulus,
                0, 1800.0, 3000.0, 0.5, 1.0,
                8000.0, 1000.0, 0.65, 0.1, 0.75, -0.25, 0.8, 0.6, 0.8)
cirrus = layer(builder, CloudLayerProfile.CloudLayerProfile.Cirrus,
               1, 8000.0, 1500.0, 0.25, 0.3,
               24000.0, 3000.0, 0.8, 0.0, 0.6, -0.15, 0.75, 0.9, 0.35)
CloudSystem.StartLayersVector(builder, 2)
builder.PrependUOffsetTRelative(cirrus)
builder.PrependUOffsetTRelative(cumulus)
layers = builder.EndVector()
CloudSystem.Start(builder)
CloudSystem.AddEnabled(builder, True)
CloudSystem.AddSeed(builder, 1337)
wind = Vec2.CreateVec2(builder, 1.0, 0.2)
CloudSystem.AddWindDirection(builder, wind)
CloudSystem.AddWindSpeedMS(builder, float(os.environ.get("CLOUD_SMOKE_WIND_SPEED", "20.0")))
CloudSystem.AddShadowEnabled(builder,
    os.environ.get("CLOUD_SMOKE_SHADOWS", "1") != "0")
CloudSystem.AddShadowExtentM(builder, 8000.0)
CloudSystem.AddLayers(builder, layers)
cloud = CloudSystem.End(builder)

Sky.Start(builder)
ground = Vec3.CreateVec3(builder, 0.1, 0.1, 0.1)
Sky.AddGroundAlbedo(builder, ground)
sky = Sky.End(builder)

Container.Start(builder)
Container.AddValueType(builder,
    GameplayComponent.GameplayComponent().GameplayComponentSkyAtmosphere)
Container.AddValue(builder, sky)
sky_container = Container.End(builder)
Container.Start(builder)
Container.AddValueType(builder,
    GameplayComponent.GameplayComponent().GameplayComponentCloudSystem)
Container.AddValue(builder, cloud)
cloud_container = Container.End(builder)
Object.StartComponentsVector(builder, 2)
builder.PrependUOffsetTRelative(cloud_container)
builder.PrependUOffsetTRelative(sky_container)
components = builder.EndVector()

Light.Start(builder)
Light.AddType(builder, LightType.LightType.Sun)
color = Vec3.CreateVec3(builder, 1.0, 1.0, 1.0)
Light.AddColor(builder, color)
Light.AddUseShadow(builder, True)
sun = SunLight.CreateSunLight(builder, 1361.0, True)
Light.AddSunLight(builder, sun)
light_value = Light.End(builder)

name = builder.CreateString("Cloud Runtime Smoke Sun")
Object.Start(builder)
Object.AddName(builder, name)
Object.AddUniqueId(builder, SUN_ID)
Object.AddVisibility(builder, True)
location = Vec3.CreateVec3(builder, 0.0, 0.0, 0.0)
Object.AddLocation(builder, location)
scale = Vec3.CreateVec3(builder, 1.0, 1.0, 1.0)
Object.AddScale(builder, scale)
rotation = Quat.CreateQuat(builder, 0.0, 0.0, 0.0, 1.0)
Object.AddRotation(builder, rotation)
Object.AddLight(builder, light_value)
Object.AddComponents(builder, components)
sun_object = Object.End(builder)

# A 40 km square is large enough to cover the camera-centered cloud-shadow
# footprint while remaining a normal opaque mesh in the deferred lighting path.
half_extent = 20000.0
positions = float_vector(builder, Mesh.StartPositionsVector, [
    -half_extent, -half_extent, 0.0,
     half_extent, -half_extent, 0.0,
     half_extent,  half_extent, 0.0,
    -half_extent,  half_extent, 0.0,
])
normals = float_vector(builder, Mesh.StartNormalsVector, [
    0.0, 0.0, 1.0,
    0.0, 0.0, 1.0,
    0.0, 0.0, 1.0,
    0.0, 0.0, 1.0,
])
texcoords = float_vector(builder, Mesh.StartTexcoordsVector, [
    0.0, 0.0,
    1.0, 0.0,
    1.0, 1.0,
    0.0, 1.0,
])
indices = uint_vector(builder, Mesh.StartIndicesVector, [0, 1, 2, 0, 2, 3])
material_ids = int_vector(
    builder, Mesh.StartMaterialIdsVector, [GROUND_MATERIAL_ID])
Mesh.Start(builder)
Mesh.AddPositions(builder, positions)
Mesh.AddNormals(builder, normals)
Mesh.AddTexcoords(builder, texcoords)
Mesh.AddIndices(builder, indices)
Mesh.AddMaterialIds(builder, material_ids)
Mesh.AddArmatureId(builder, -1)
ground_mesh = Mesh.End(builder)

ground_name = builder.CreateString("Cloud Shadow Receiver")
Object.Start(builder)
Object.AddName(builder, ground_name)
Object.AddUniqueId(builder, GROUND_ID)
Object.AddVisibility(builder, True)
ground_location = Vec3.CreateVec3(builder, 0.0, 0.0, 0.0)
Object.AddLocation(builder, ground_location)
ground_scale = Vec3.CreateVec3(builder, 1.0, 1.0, 1.0)
Object.AddScale(builder, ground_scale)
ground_rotation = Quat.CreateQuat(builder, 0.0, 0.0, 0.0, 1.0)
Object.AddRotation(builder, ground_rotation)
Object.AddMesh(builder, ground_mesh)
ground_object = Object.End(builder)

material_name = builder.CreateString("Cloud Shadow Ground")
Material.Start(builder)
Material.AddUniqueId(builder, GROUND_MATERIAL_ID)
Material.AddName(builder, material_name)
base_color = Vec4.CreateVec4(builder, 0.18, 0.24, 0.12, 1.0)
Material.AddBaseColor(builder, base_color)
Material.AddMetallic(builder, 0.0)
Material.AddRoughness(builder, 0.9)
emission_color = Vec4.CreateVec4(builder, 0.0, 0.0, 0.0, 1.0)
Material.AddEmissionColor(builder, emission_color)
ground_material = Material.End(builder)

Update.StartObjectsVector(builder, 2)
builder.PrependUOffsetTRelative(ground_object)
builder.PrependUOffsetTRelative(sun_object)
objects = builder.EndVector()
Update.StartMaterialsVector(builder, 1)
builder.PrependUOffsetTRelative(ground_material)
materials = builder.EndVector()

camera_mode = os.environ.get("CLOUD_SMOKE_CAMERA", "zenith")
ground_view = camera_mode == "ground"
horizon_view = camera_mode == "horizon"
EditorCamera.Start(builder)
camera_location = Vec3.CreateVec3(
    builder, 0.0, -800.0 if ground_view else 0.0, 300.0 if ground_view else 100.0)
EditorCamera.AddLocation(builder, camera_location)
camera_forward = Vec3.CreateVec3(
    builder, 0.0,
    0.936329 if ground_view else (0.999391 if horizon_view else 0.0),
    -0.351123 if ground_view else (0.034899 if horizon_view else 1.0))
EditorCamera.AddForward(builder, camera_forward)
camera_up = Vec3.CreateVec3(
    builder, 0.0,
    0.351123 if ground_view else (-0.034899 if horizon_view else 1.0),
    0.936329 if ground_view else (0.999391 if horizon_view else 0.0))
EditorCamera.AddUp(builder, camera_up)
editor_camera = EditorCamera.End(builder)

Update.Start(builder)
Update.AddObjects(builder, objects)
Update.AddMaterials(builder, materials)
Update.AddEditorCamera(builder, editor_camera)
root = Update.End(builder)
builder.FinishSizePrefixed(root)

payload = builder.Output()
decoded = Update.Update.GetRootAs(payload, 4)
assert decoded.ObjectsLength() == 2
assert decoded.MaterialsLength() == 1
assert decoded.Reset() is False
decoded_sun = next(
    decoded.Objects(index)
    for index in range(decoded.ObjectsLength())
    if decoded.Objects(index).UniqueId() == SUN_ID
)
decoded_ground = next(
    decoded.Objects(index)
    for index in range(decoded.ObjectsLength())
    if decoded.Objects(index).UniqueId() == GROUND_ID
)
assert decoded_sun.ComponentsLength() == 2
assert {decoded_sun.Components(index).ValueType() for index in range(2)} == {
    GameplayComponent.GameplayComponent().GameplayComponentSkyAtmosphere,
    GameplayComponent.GameplayComponent().GameplayComponentCloudSystem,
}
assert decoded_ground.Mesh() is not None
assert decoded_ground.Mesh().PositionsLength() == 12
assert decoded_ground.Mesh().IndicesLength() == 6
assert decoded_ground.Mesh().MaterialIds(0) == GROUND_MATERIAL_ID

if output_path := os.environ.get("CLOUD_SMOKE_OUTPUT"):
    pathlib.Path(output_path).write_bytes(payload)
    print(f"CLOUD_RUNTIME_SMOKE_PAYLOAD_WRITTEN {output_path}")

if os.environ.get("CLOUD_SMOKE_PAYLOAD_ONLY") == "1":
    print("CLOUD_RUNTIME_SMOKE_PAYLOAD_OK")
    raise SystemExit(0)

deadline = time.monotonic() + float(os.environ.get("CLOUD_SMOKE_CONNECT_SECONDS", "30"))
while True:
    try:
        connection = socket.create_connection(("127.0.0.1", 65432), timeout=1.0)
        break
    except ConnectionRefusedError:
        if time.monotonic() >= deadline:
            raise
        time.sleep(0.1)
with connection:
    connection.sendall(payload)
    # Keep the controller alive through cache generation and enough temporal
    # and interleaved shadow frames for smoke/performance capture.
    time.sleep(float(os.environ.get("CLOUD_SMOKE_HOLD_SECONDS", "8")))
print("CLOUD_RUNTIME_SMOKE_SENT")
