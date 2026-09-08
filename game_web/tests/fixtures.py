"""Small deterministic wire fixtures; no Blender or third-party Python needed."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from compiled_schemas.python import flatbuffers
from compiled_schemas.python.Blender.LiveLink import Update, Object, Mesh, Vec3, Vec4, Quat, Material, Image, EditorCamera, Light, PointLight, SpotLight, SunLight

CAMERA = {'position': [0, -7, 3], 'forward': [0, 1, -0.3], 'up': [0, 0, 1]}
QUAD = {'positions': [-2,0,0, 2,0,0, 2,0,3, -2,0,3], 'normals': [0,-1,0]*4,
        'uvs': [0,0, 1,0, 1,1, 0,1], 'indices': [0,1,2, 0,2,3], 'materialIds': [11]}
# Bottom row red/green, top row blue/yellow; color bytes deliberately asymmetric.
PIXELS = bytes([255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255])


def encode(*, objects=(), materials=(), images=(), camera=None, deleted=(), reset=False):
    b = flatbuffers.Builder(1024)
    def vector(values, scalar):
        b.StartVector(4, len(values), 4)
        for v in reversed(values):
            getattr(b, 'Prepend' + scalar)(v)
        return b.EndVector()
    object_offsets = []
    for obj in objects:
        mesh_offset = light_offset = None
        if 'mesh' in obj:
            mesh = obj['mesh']
            vectors = [(Mesh.AddPositions, vector(mesh['positions'], 'Float32')),
                       (Mesh.AddNormals, vector(mesh.get('normals', []), 'Float32')),
                       (Mesh.AddTexcoords, vector(mesh.get('uvs', []), 'Float32')),
                       (Mesh.AddIndices, vector(mesh['indices'], 'Uint32')),
                       (Mesh.AddMaterialIds, vector(mesh.get('materialIds', []), 'Int32'))]
            Mesh.Start(b)
            for add, value in vectors: add(b, value)
            mesh_offset = Mesh.End(b)
        if 'light' in obj:
            light = obj['light']
            Light.Start(b)
            Light.AddType(b, light['type'])
            Light.AddColor(b, Vec3.CreateVec3(b, *light.get('color', [1,1,1])))
            Light.AddUseShadow(b, light.get('shadows', True))
            if light['type'] == 0:
                Light.AddPointLight(b, PointLight.CreatePointLight(b, light['power']))
            elif light['type'] == 1:
                Light.AddSpotLight(b, SpotLight.CreateSpotLight(b, light['power'], light.get('angle', 0.5)*2, light.get('edgeBlend', 0.5)))
            elif light['type'] == 2:
                Light.AddSunLight(b, SunLight.CreateSunLight(b, light['power'], light.get('shadows', True)))
            light_offset = Light.End(b)
        name = b.CreateString(obj.get('name', 'Fixture'))
        Object.Start(b)
        Object.AddName(b, name)
        Object.AddUniqueId(b, obj['id'])
        Object.AddVisibility(b, obj.get('visible', True))
        Object.AddLocation(b, Vec3.CreateVec3(b, *obj.get('position', [0,0,0])))
        Object.AddScale(b, Vec3.CreateVec3(b, *obj.get('scale', [1,1,1])))
        Object.AddRotation(b, Quat.CreateQuat(b, *obj.get('rotation', [0,0,0,1])))
        if mesh_offset is not None: Object.AddMesh(b, mesh_offset)
        if light_offset is not None: Object.AddLight(b, light_offset)
        object_offsets.append(Object.End(b))
    material_offsets = []
    for material in materials:
        Material.Start(b)
        Material.AddUniqueId(b, material['id'])
        Material.AddBaseColor(b, Vec4.CreateVec4(b, *material.get('color', [0.6,0.6,0.6,1])))
        Material.AddEmissionColor(b, Vec4.CreateVec4(b, *material.get('emission', [0,0,0,1])))
        for add, key, default in [(Material.AddMetallic,'metallic',0), (Material.AddRoughness,'roughness',0.5),
            (Material.AddEmissionStrength,'emissionStrength',0), (Material.AddBaseColorImageId,'colorImage',-1),
            (Material.AddMetallicImageId,'metallicImage',-1), (Material.AddRoughnessImageId,'roughnessImage',-1),
            (Material.AddEmissionColorImageId,'emissionImage',-1)]: add(b, material.get(key, default))
        material_offsets.append(Material.End(b))
    image_offsets = []
    for image in images:
        data = b.CreateByteVector(image['pixels'])
        Image.Start(b)
        Image.AddUniqueId(b, image['id'])
        Image.AddWidth(b, image['width'])
        Image.AddHeight(b, image['height'])
        Image.AddData(b, data)
        image_offsets.append(Image.End(b))
    camera_offset = None
    if camera:
        EditorCamera.Start(b)
        EditorCamera.AddLocation(b, Vec3.CreateVec3(b, *camera['position']))
        EditorCamera.AddForward(b, Vec3.CreateVec3(b, *camera['forward']))
        EditorCamera.AddUp(b, Vec3.CreateVec3(b, *camera['up']))
        camera_offset = EditorCamera.End(b)
    vectors = [(Update.AddObjects, vector(object_offsets,'UOffsetTRelative')),
               (Update.AddMaterials, vector(material_offsets,'UOffsetTRelative')),
               (Update.AddImages, vector(image_offsets,'UOffsetTRelative')),
               (Update.AddDeletedObjectUids, vector(deleted,'Int32'))]
    Update.Start(b)
    for add, value in vectors: add(b, value)
    Update.AddReset(b, reset)
    if camera_offset is not None: Update.AddEditorCamera(b, camera_offset)
    root = Update.End(b)
    b.FinishSizePrefixed(root)
    return bytes(b.Output())


def textured_scene():
    return encode(objects=[{'id':1,'mesh':QUAD}, {'id':2,'position':[0,-3,4],
        'light':{'type':0,'power':100,'shadows':True}}],
        materials=[{'id':11,'color':[0.1,0.1,0.1,1],'colorImage':21}],
        images=[{'id':21,'width':2,'height':2,'pixels':PIXELS}],camera=CAMERA)
