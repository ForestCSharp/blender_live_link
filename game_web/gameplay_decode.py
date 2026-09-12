"""Gameplay and sampled skeletal data from the existing wire schema."""
import math
from compiled_schemas.python.Blender.LiveLink.GameplayComponentCharacter import GameplayComponentCharacter
from compiled_schemas.python.Blender.LiveLink.GameplayComponentCameraControl import GameplayComponentCameraControl
from compiled_schemas.python.Blender.LiveLink.GameplayComponentPart import GameplayComponentPart
from compiled_schemas.python.Blender.LiveLink.GameplayComponentAttachmentPoint import GameplayComponentAttachmentPoint

IDENTITY = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]


def decode_gameplay(obj, item, vector, bounds):
    def finite(values):
        if not all(math.isfinite(v) for v in values):
            raise ValueError('Non-finite gameplay data')
        return values

    def matrix(value):
        if value is None:
            return IDENTITY[:]
        bounds(value)
        values = finite(vector(value, 'Elements'))
        if len(values) != 16:
            raise ValueError('Invalid gameplay matrix')
        return values

    types = {1: ('character', GameplayComponentCharacter), 2: ('cameraControl', GameplayComponentCameraControl),
             4: ('part', GameplayComponentPart), 5: ('attachment', GameplayComponentAttachmentPoint)}
    for name, _ in types.values():
        item[name] = None
    for container in vector(obj, 'Components'):
        bounds(container)
        entry = types.get(container.ValueType())
        if entry is None:
            continue
        name, cls = entry
        raw = container.Value()
        if raw is None:
            raise ValueError('Missing gameplay component')
        value = cls()
        value.Init(raw.Bytes, raw.Pos)
        bounds(value)
        if name == 'character':
            speed, jump, height, radius = finite([value.MoveSpeed(), value.JumpSpeed(), value.Height(), value.Radius()])
            if height < 0 or radius <= 0 or speed < 0 or jump < 0:
                raise ValueError('Invalid character settings')
            item[name] = dict(playerControlled=value.PlayerControlled(), moveSpeed=speed, jumpSpeed=jump, height=height, radius=radius)
        elif name == 'cameraControl':
            distance, speed = finite([value.FollowDistance(), value.FollowSpeed()])
            if distance <= 0 or speed < 0:
                raise ValueError('Invalid follow-camera settings')
            item[name] = dict(followDistance=distance, followSpeed=speed)
        elif name == 'part':
            if not 0 <= value.PartType() < 5:
                raise ValueError('Invalid part type')
            item[name] = dict(type=value.PartType())
        else:
            if not 0 <= value.PartType() < 5 or value.BindingType() not in (0, 1):
                raise ValueError('Invalid attachment type')
            item[name] = dict(ownerPartId=value.OwnerPartId(), type=value.PartType(), bindingType=value.BindingType(),
                              armatureId=value.ArmatureId(), boneName=(value.BoneName() or b'').decode('utf-8', 'replace'),
                              localTransform=matrix(value.LocalTransform()), valid=value.Valid())

    mesh = obj.Mesh()
    if mesh is not None:
        indices = vector(mesh, 'JointIndices')
        weights = finite(vector(mesh, 'JointWeights'))
        count = len(item['mesh']['positions']) // 3 * 4
        if (indices or weights) and (len(indices) != count or len(weights) != count or
                                    any(i < 0 for i in indices) or any(w < 0 for w in weights)):
            raise ValueError('Invalid skinning vectors')
        item['mesh'].update(jointIndices=indices, jointWeights=weights, armatureId=mesh.ArmatureId(),
                            meshToArmature=matrix(mesh.MeshToArmature()), armatureToMesh=matrix(mesh.ArmatureToMesh()))

    armature = obj.Armature()
    item['armature'] = None
    if armature is not None:
        bounds(armature)
        bones = []
        for bone in vector(armature, 'Bones'):
            bounds(bone)
            bones.append(dict(name=(bone.Name() or b'').decode('utf-8', 'replace'), parentIndex=bone.ParentIndex(),
                              inverseBindMatrix=matrix(bone.InverseBindMatrix())))
        if any(b['parentIndex'] < -1 or b['parentIndex'] >= len(bones) for b in bones):
            raise ValueError('Invalid bone parent')
        clips = []
        for clip in vector(armature, 'Animations'):
            bounds(clip)
            rate, duration = finite([clip.FrameRate(), clip.DurationSeconds()])
            frames, count = clip.FrameCount(), clip.BoneCount()
            matrices = finite(vector(clip, 'SkinMatrices'))
            if frames < 0 or count < 0 or count > len(bones) or len(matrices) != frames * count * 16 or rate < 0 or duration < 0:
                raise ValueError('Invalid animation dimensions')
            clips.append(dict(name=(clip.Name() or b'').decode('utf-8', 'replace'), frameRate=rate,
                              duration=duration, frameCount=frames, boneCount=count, matrices=matrices))
        item['armature'] = dict(bones=bones, animations=clips)
