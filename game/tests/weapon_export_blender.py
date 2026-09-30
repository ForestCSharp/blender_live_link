"""Blender --background --factory-startup --python game/tests/weapon_export_blender.py.
Writes a playable fixture to /tmp/weapon_fixture.bin and checks both export paths.
"""
import importlib
from pathlib import Path
import sys

import bpy

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root.parent))
ext = importlib.import_module(root.name + '.extension_main')
ext.register()
ext.depsgraph_update_post_callback.enabled = False
ext.get_editor_camera_snapshot = lambda context=None: (12, -18, 10, -.5, .8, -.3, 0, 0, 1)


def component(obj, kind):
    value = obj.live_link_settings.components.add()
    value.type = kind
    return getattr(value, ext.TYPE_TO_GROUP[kind])


def cube(name, location, scale):
    bpy.ops.mesh.primitive_cube_add(location=location)
    obj = bpy.context.object
    obj.name = name
    obj.scale = scale
    return obj


def decode(payload):
    return ext.Update.Update.GetRootAs(payload, 4)


def objects(update):
    return [update.Objects(i) for i in range(update.ObjectsLength())]


def verify(payload):
    exported = objects(decode(payload))
    weapons = []
    rigs = []
    for obj in exported:
        if obj.Armature():
            rigs.append(obj)
            labels = [obj.Armature().Bones(i).AttachmentLabel() for i in range(obj.Armature().BonesLength())]
            assert b'Hand' in labels, labels
            assert b'' in labels, labels
        for i in range(obj.ComponentsLength()):
            c = obj.Components(i)
            if c.ValueType() == ext.GameplayComponent.GameplayComponent.GameplayComponentWeapon:
                value = ext.GameplayComponentWeapon.GameplayComponentWeapon()
                table = c.Value()
                value.Init(table.Bytes, table.Pos)
                weapons.append((obj.Name(), value.AcceptedBoneLabel(), value.MuzzleValid(), value.MuzzleLocalTransform()))
    assert sorted(label for _, label, _, _ in weapons) == [b'Foot', b'Hand', b'Hand', b'LibraryOnly', b'LibraryOnly'], weapons
    assert sorted(valid for _, _, valid, _ in weapons) == [False, True, True, True, True], weapons
    for name, _, valid, matrix in weapons:
        if valid:
            expected_y = .8 if b'LibraryWeapon' in name else 1.4
            assert matrix is not None and abs(matrix.Elements(13) - expected_y) < 1e-4, name
    assert len(rigs) == 5, [(o.Name(), o.UniqueId()) for o in rigs]
    assert len({o.UniqueId() for o in rigs}) == 5
    # Each linked mesh must bind to the rig in its own occurrence.
    linked_meshes = [o for o in exported if o.Mesh() and b'LibraryArm' in o.Name()]
    assert len(linked_meshes) == 2
    rig_ids = {o.UniqueId() for o in rigs}
    assert len({o.Mesh().ArmatureId() for o in linked_meshes}) == 2
    assert all(o.Mesh().ArmatureId() in rig_ids for o in linked_meshes)


try:
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    body = cube('WeaponTestBody', (0, 0, 4), (1, .7, 1))
    component(body, 'PART').part_type = 'BODY'
    arms = []
    for x, side in ((-2, 'LEFT_ARM'), (2, 'RIGHT_ARM')):
        bpy.ops.object.armature_add(location=(x, 0, 4))
        rig = bpy.context.object
        rig.name = side + 'Rig'
        bpy.ops.object.mode_set(mode='EDIT')
        rig.data.edit_bones[0].name = 'Wrist'
        rig.data.edit_bones['Wrist'].live_link_attachment_label = 'Hand'
        extra = rig.data.edit_bones.new('Unlabeled')
        extra.head = (0, 0, 2)
        extra.tail = (0, 0, 3)
        bpy.ops.object.mode_set(mode='OBJECT')
        assert rig.data.bones['Wrist'].live_link_attachment_label == 'Hand'
        pose = rig.pose.bones['Wrist']
        pose.rotation_mode = 'XYZ'
        for frame, angle in ((1, 0), (12, .7), (24, 0)):
            pose.rotation_euler.x = angle
            pose.keyframe_insert('rotation_euler', frame=frame)
        arm = cube(side + 'Mesh', (x, 0, 4), (.4, .4, 1))
        component(arm, 'PART').part_type = side
        group = arm.vertex_groups.new(name='Wrist')
        group.add(list(range(len(arm.data.vertices))), 1, 'REPLACE')
        arm.modifiers.new('Rig', 'ARMATURE').object = rig
        marker = bpy.data.objects.new(side + 'Socket', None)
        bpy.context.collection.objects.link(marker)
        marker.parent = body
        marker.location = (x, 0, 0)
        socket = component(marker, 'ATTACHMENT_POINT')
        socket.owner_part = body
        socket.part_type = side
        arms.append((arm, rig))
    for index, label in enumerate(('Hand', 'Hand', 'Foot')):
        weapon = cube('Weapon' + str(index), (20 + index * 3, 0, 0), (.25, 1, .25))
        weapon_component = component(weapon, 'WEAPON')
        weapon_component.accepted_bone_label = label
        muzzle = bpy.data.objects.new('Weapon' + str(index) + 'Muzzle', None)
        bpy.context.collection.objects.link(muzzle)
        muzzle.parent = weapon
        muzzle.location = (0, 1.4, 0)
        if index == 0:
            muzzle.rotation_euler.z = .35
        weapon_component.muzzle_object = muzzle
        if index == 2:
            muzzle.parent = body  # Stale selection must serialize as invalid.
        if index == 0:
            first_muzzle = muzzle
        if index == 0:
            weapon_rig = arms[0][1].copy()
            weapon_rig.data = weapon_rig.data.copy()
            weapon_rig.name = 'WeaponRig'
            weapon_rig.location = weapon.location
            bpy.context.collection.objects.link(weapon_rig)
            group = weapon.vertex_groups.new(name='Wrist')
            group.add(list(range(len(weapon.data.vertices))), 1, 'REPLACE')
            weapon.modifiers.new('WeaponRig', 'ARMATURE').object = weapon_rig
    player = bpy.data.objects.new('WeaponTestPlayer', None)
    bpy.context.collection.objects.link(player)
    player.location.z = 4
    component(player, 'CHARACTER').player_controlled = True
    component(player, 'CAMERA_CONTROL').follow_distance = 15
    npc = bpy.data.objects.new('WeaponTestNPC', None)
    bpy.context.collection.objects.link(npc)
    npc.location = (10, 0, 4)
    component(npc, 'CHARACTER').player_controlled = False

    # Two linked collection occurrences carry the same labels but independent rig IDs.
    library = bpy.data.collections.new('WeaponTestLibrary')
    library_rig = arms[0][1].copy()
    library_rig.name = 'LibraryRig'
    library.objects.link(library_rig)
    library_arm = arms[0][0].copy()
    library_arm.name = 'LibraryArm'
    library_arm.live_link_settings.components.clear()
    library_arm.modifiers[0].object = library_rig
    library.objects.link(library_arm)
    library_weapon = bpy.data.objects.new('LibraryWeapon', bpy.data.objects['Weapon1'].data)
    library.objects.link(library_weapon)
    library_weapon.location = (5, 0, 0)
    library_weapon_component = component(library_weapon, 'WEAPON')
    library_weapon_component.accepted_bone_label = 'LibraryOnly'
    library_muzzle = bpy.data.objects.new('LibraryMuzzle', None)
    library.objects.link(library_muzzle)
    library_muzzle.parent = library_weapon
    library_muzzle.location.y = .8
    library_weapon_component.muzzle_object = library_muzzle
    for index in range(2):
        instance = bpy.data.objects.new('LibraryOccurrence' + str(index), None)
        bpy.context.collection.objects.link(instance)
        instance.instance_type = 'COLLECTION'
        instance.instance_collection = library
        instance.location.x = 30 + 5 * index
        instance.hide_set(True)

    bpy.context.scene.frame_set(1)
    bpy.context.view_layer.update()
    connection = ext.LiveLinkConnection()
    all_objects = list(bpy.context.scene.objects)
    verify(connection.make_update_python(all_objects, [], reset=False, update_reason='weapon_test'))
    assert first_muzzle.session_uid in connection._object_dependency_ids(bpy.data.objects['Weapon0'])
    assert library_muzzle.session_uid in connection._object_dependency_ids(library_weapon)
    with connection.export_evaluation_context(all_objects) as evaluation:
        occurrences = connection.collect_export_occurrences(all_objects, *evaluation)
        linked_weapons = [o for o in occurrences if o.source_object == library_weapon]
        assert len(linked_weapons) == 2
        assert all(library_muzzle.session_uid in o.dependency_ids for o in linked_weapons)
    library_muzzle.location.y = 1.1
    bpy.context.view_layer.update()
    linked_moved = objects(decode(connection.make_update_python(all_objects, [], reset=False, update_reason='linked_muzzle_move')))
    for linked_weapon in (obj for obj in linked_moved if b'LibraryWeapon' in obj.Name()):
        component_table = next(linked_weapon.Components(i).Value()
                               for i in range(linked_weapon.ComponentsLength())
                               if linked_weapon.Components(i).ValueType() == ext.GameplayComponent.GameplayComponent.GameplayComponentWeapon)
        value = ext.GameplayComponentWeapon.GameplayComponentWeapon()
        value.Init(component_table.Bytes, component_table.Pos)
        assert abs(value.MuzzleLocalTransform().Elements(13) - 1.1) < 1e-4
    library_muzzle.location.y = .8
    bpy.context.view_layer.update()
    first_muzzle.location.y = 1.7
    bpy.context.view_layer.update()
    moved = objects(decode(connection.make_update_python(all_objects, [], reset=False, update_reason='muzzle_move')))
    moved_weapon = next(obj for obj in moved if obj.Name() == b'Weapon0')
    moved_component = next(moved_weapon.Components(i) for i in range(moved_weapon.ComponentsLength())
                           if moved_weapon.Components(i).ValueType() == ext.GameplayComponent.GameplayComponent.GameplayComponentWeapon)
    moved_value = ext.GameplayComponentWeapon.GameplayComponentWeapon()
    moved_value.Init(moved_component.Value().Bytes, moved_component.Value().Pos)
    assert abs(moved_value.MuzzleLocalTransform().Elements(13) - 1.7) < 1e-4
    first_muzzle.location.y = 1.4
    bpy.context.view_layer.update()
    bpy.context.scene.live_link_use_python_export_fallback = False
    if ext.native_live_link_available():
        verify(connection.make_update(all_objects, [], reset=False, update_reason='weapon_test_native'))
        matched, message = connection.compare_native_python_full_update()
        assert matched, message
    else:
        print('Native exporter unavailable; Python export tested only')
    connection.save_to_file(all_objects, '/tmp/weapon_fixture.bin')
    verify(Path('/tmp/weapon_fixture.bin').read_bytes())

    # Callback queues every object sharing the edited Armature datablock.
    ext.live_link_connection.is_connected = lambda: True
    ext.batched_dirty_ids.clear()
    ext.depsgraph_update_post_callback.enabled = True
    arms[0][1].data.bones['Wrist'].live_link_attachment_label = 'UpdatedHand'
    assert arms[0][1].session_uid in ext.batched_dirty_ids
    assert library_rig.session_uid in ext.batched_dirty_ids
    ext.batched_dirty_ids.clear()
    bpy.data.objects['Weapon0'].live_link_settings.components[0].weapon.muzzle_object = None
    assert bpy.data.objects['Weapon0'].session_uid in ext.batched_dirty_ids
    ext.depsgraph_update_post_callback.enabled = False
    bpy.data.objects['Weapon0'].live_link_settings.components[0].weapon.muzzle_object = first_muzzle
    arms[0][1].data.bones['Wrist'].live_link_attachment_label = 'Hand'
    print('WEAPON_EXPORT_TESTS_PASSED /tmp/weapon_fixture.bin')
finally:
    ext.unregister()
