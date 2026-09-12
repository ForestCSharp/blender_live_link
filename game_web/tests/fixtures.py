"""Small deterministic wire fixtures; no Blender or third-party Python needed."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from compiled_schemas.python import flatbuffers
from compiled_schemas.python.Blender.LiveLink import Update, Object, Mesh, Vec3, Vec4, Quat, Material, Image, EditorCamera, Light, PointLight, SpotLight, SunLight, RigidBody, Matrix, Bone, Armature, Animation, GameplayComponentContainer, GameplayComponentCharacter, GameplayComponentCameraControl, GameplayComponentPart, GameplayComponentAttachmentPoint

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
    def matrix(values):
        data = vector(values, 'Float32')
        Matrix.Start(b); Matrix.AddElements(b, data)
        return Matrix.End(b)
    object_offsets = []
    for obj in objects:
        mesh_offset = light_offset = None
        if 'mesh' in obj:
            mesh = obj['mesh']
            vectors = [(Mesh.AddPositions, vector(mesh['positions'], 'Float32')),
                       (Mesh.AddNormals, vector(mesh.get('normals', []), 'Float32')),
                       (Mesh.AddTexcoords, vector(mesh.get('uvs', []), 'Float32')),
                       (Mesh.AddIndices, vector(mesh['indices'], 'Uint32')),
                       (Mesh.AddMaterialIds, vector(mesh.get('materialIds', []), 'Int32')),
                       (Mesh.AddJointIndices, vector(mesh.get('jointIndices', []), 'Int32')),
                       (Mesh.AddJointWeights, vector(mesh.get('jointWeights', []), 'Float32'))]
            for key, add in [('meshToArmature', Mesh.AddMeshToArmature), ('armatureToMesh', Mesh.AddArmatureToMesh)]:
                if key in mesh: vectors.append((add, matrix(mesh[key])))
            Mesh.Start(b)
            for add, value in vectors: add(b, value)
            Mesh.AddArmatureId(b, mesh.get('armatureId', -1))
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
        component_offsets = []
        for kind, key, module in [(1,'character',GameplayComponentCharacter), (2,'cameraControl',GameplayComponentCameraControl),
                                  (4,'part',GameplayComponentPart),(5,'attachment',GameplayComponentAttachmentPoint)]:
            value = obj.get(key)
            if value is None: continue
            if key == 'attachment':
                local = matrix(value['localTransform']); bone_name = b.CreateString(value.get('boneName',''))
            module.Start(b)
            if key == 'character':
                module.AddPlayerControlled(b, value.get('playerControlled',False))
                module.AddMoveSpeed(b,value.get('moveSpeed',20));module.AddJumpSpeed(b,value.get('jumpSpeed',10))
                module.AddHeight(b,value.get('height',6));module.AddRadius(b,value.get('radius',1))
            elif key == 'cameraControl':
                module.AddFollowDistance(b,value['followDistance']);module.AddFollowSpeed(b,value['followSpeed'])
            elif key == 'part': module.AddPartType(b,value['type'])
            else:
                module.AddOwnerPartId(b,value['ownerPartId']);module.AddPartType(b,value['type'])
                module.AddBindingType(b,value.get('bindingType',0));module.AddArmatureId(b,value.get('armatureId',-1))
                module.AddBoneName(b,bone_name);module.AddLocalTransform(b,local);module.AddValid(b,value.get('valid',True))
            offset=module.End(b)
            GameplayComponentContainer.Start(b);GameplayComponentContainer.AddValueType(b,kind);GameplayComponentContainer.AddValue(b,offset)
            component_offsets.append(GameplayComponentContainer.End(b))
        component_vector = vector(component_offsets,'UOffsetTRelative')
        armature_offset = None
        if obj.get('armature') is not None:
            data=obj['armature']; bone_offsets=[]; clip_offsets=[]
            for bone in data['bones']:
                name=b.CreateString(bone['name']); inverse=matrix(bone['inverseBindMatrix'])
                Bone.Start(b);Bone.AddName(b,name);Bone.AddParentIndex(b,bone.get('parentIndex',-1));Bone.AddInverseBindMatrix(b,inverse)
                bone_offsets.append(Bone.End(b))
            for clip in data['animations']:
                name=b.CreateString(clip['name']); matrices=vector(clip['matrices'],'Float32')
                Animation.Start(b);Animation.AddName(b,name);Animation.AddFrameRate(b,clip['frameRate']);Animation.AddDurationSeconds(b,clip['duration'])
                Animation.AddFrameCount(b,clip['frameCount']);Animation.AddBoneCount(b,clip['boneCount']);Animation.AddSkinMatrices(b,matrices)
                clip_offsets.append(Animation.End(b))
            bones=vector(bone_offsets,'UOffsetTRelative');clips=vector(clip_offsets,'UOffsetTRelative')
            Armature.Start(b);Armature.AddBones(b,bones);Armature.AddAnimations(b,clips);armature_offset=Armature.End(b)
        name = b.CreateString(obj.get('name', 'Fixture'))
        Object.Start(b)
        Object.AddName(b, name)
        Object.AddUniqueId(b, obj['id'])
        Object.AddComponents(b,component_vector)
        if armature_offset is not None: Object.AddArmature(b,armature_offset)
        Object.AddVisibility(b, obj.get('visible', True))
        Object.AddLocation(b, Vec3.CreateVec3(b, *obj.get('position', [0,0,0])))
        Object.AddScale(b, Vec3.CreateVec3(b, *obj.get('scale', [1,1,1])))
        Object.AddRotation(b, Quat.CreateQuat(b, *obj.get('rotation', [0,0,0,1])))
        if obj.get('rigidBody') is not None:
            rigid = obj['rigidBody']
            Object.AddRigidBody(b, RigidBody.CreateRigidBody(b, rigid['isDynamic'], rigid['mass']))
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


def gameplay_scene():
    identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
    def translate(x,y,z):
        m=identity[:];m[12:15]=[x,y,z];return m
    mesh={'positions':[-.5,0,0,.5,0,0,0,0,1], 'normals':[0,-1,0]*3,'indices':[0,1,2],'materialIds':[]}
    character={'playerControlled':True,'moveSpeed':20,'jumpSpeed':10,'height':2,'radius':.5}
    objects=[{'id':1,'name':'Pilot one','position':[0,0,20],'character':character,'cameraControl':{'followDistance':10,'followSpeed':5}},
             {'id':2,'name':'Pilot two','position':[10,0,20],'character':{**character,'playerControlled':False}},
             {'id':100,'name':'Body rig','armature':{'bones':[{'name':'root','inverseBindMatrix':translate(0,-1,0)}],
                'animations':[{'name':'Lift','frameRate':2,'duration':1,'frameCount':2,'boneCount':1,'matrices':identity+translate(0,0,2)},
                              {'name':'Slide','frameRate':2,'duration':1,'frameCount':2,'boneCount':1,'matrices':translate(3,0,0)*2}]}},
             {'id':10,'name':'Body','scale':[2,1,1],'part':{'type':0},'mesh':{**mesh,'jointIndices':[0,0,0,0]*3,
                'jointWeights':[1,0,0,0]*3,'armatureId':100,'meshToArmature':translate(2,0,0),'armatureToMesh':translate(-2,0,0)}}]
    for slot in range(1,5):
        objects.append({'id':10+slot,'name':['Body','Legs','Left arm','Right arm','Head'][slot],'scale':[.5,.5,.5],'part':{'type':slot},'mesh':mesh})
        objects.append({'id':20+slot,'name':'Socket '+str(slot),'attachment':{'ownerPartId':10,'type':slot,'valid':True,
                        'bindingType':1 if slot==4 else 0,'armatureId':100,'boneName':'root','localTransform':translate(1,2,3)}})
    objects.append({'id':40,'name':'Alternative left arm','part':{'type':2},'mesh':mesh})
    return encode(objects=objects,camera=CAMERA)
