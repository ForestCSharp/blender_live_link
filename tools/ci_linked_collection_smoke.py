"""Linked/local collection-instance coverage for ``ci_blender_smoke.py``."""

from __future__ import annotations

from pathlib import Path

import bpy
from mathutils import Matrix


def _name(value) -> str:
    return value.decode("utf-8") if isinstance(value, bytes) else str(value)


def _objects(update):
    return {
        _name(update.Objects(index).Name()): update.Objects(index)
        for index in range(update.ObjectsLength())
    }


def _attachment_component(extension_module, exported_object):
    expected_type = (
        extension_module.GameplayComponent.GameplayComponent
        .GameplayComponentAttachmentPoint
    )
    for index in range(exported_object.ComponentsLength()):
        container = exported_object.Components(index)
        if container.ValueType() != expected_type:
            continue
        union = container.Value()
        attachment = (
            extension_module.GameplayComponentAttachmentPoint
            .GameplayComponentAttachmentPoint()
        )
        attachment.Init(union.Bytes, union.Pos)
        return attachment
    raise AssertionError(f"{_name(exported_object.Name())} has no Attachment Point")


def _clear_objects_and_collections() -> None:
    for scene_object in list(bpy.data.objects):
        bpy.data.objects.remove(scene_object, do_unlink=True)
    for collection in list(bpy.data.collections):
        bpy.data.collections.remove(collection)


def _create_library(library_path: Path) -> None:
    _clear_objects_and_collections()
    source = bpy.data.collections.new("LL Linked Asset")
    source.instance_offset = (0.5, -0.25, 0.75)
    bpy.context.scene.collection.children.link(source)

    ordinary_child = bpy.data.collections.new("LL Ordinary Child")
    source.children.link(ordinary_child)
    nested = bpy.data.collections.new("LL Nested Asset")
    nested.use_fake_user = True

    material = bpy.data.materials.new("LL Shared Material")
    material.use_nodes = True
    principled = material.node_tree.nodes.get("Principled BSDF")
    image = bpy.data.images.new("LL Shared Image", width=1, height=1)
    image.pixels[:] = (0.15, 0.55, 0.9, 1.0)
    image_node = material.node_tree.nodes.new("ShaderNodeTexImage")
    image_node.image = image
    material.node_tree.links.new(
        image_node.outputs["Color"], principled.inputs["Base Color"]
    )

    armature_data = bpy.data.armatures.new("LL Asset Armature Data")
    armature = bpy.data.objects.new("LL Asset Armature", armature_data)
    source.objects.link(armature)
    bpy.context.view_layer.objects.active = armature
    armature.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    bone = armature_data.edit_bones.new("Mount")
    bone.head = (0.0, 0.0, 0.0)
    bone.tail = (0.0, 0.0, 1.0)
    bpy.ops.object.mode_set(mode="POSE")
    pose_bone = armature.pose.bones["Mount"]
    pose_bone.rotation_mode = "XYZ"
    pose_bone.rotation_euler.z = 0.0
    pose_bone.keyframe_insert(data_path="rotation_euler", frame=1)
    pose_bone.rotation_euler.z = 0.35
    pose_bone.keyframe_insert(data_path="rotation_euler", frame=3)
    armature.animation_data.action.name = "LL Asset Idle"
    bpy.ops.object.mode_set(mode="OBJECT")
    armature.select_set(False)

    mesh_data = bpy.data.meshes.new("LL Asset Mesh Data")
    mesh_data.from_pydata(
        [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (0.0, 1.0, 0.0)],
        [],
        [(0, 1, 2)],
    )
    mesh_data.materials.append(material)
    mesh = bpy.data.objects.new("LL Asset Mesh", mesh_data)
    mesh.location = (1.5, 0.0, 0.0)
    source.objects.link(mesh)
    subdivision = mesh.modifiers.new("LL Evaluated Subdivision", type="SUBSURF")
    subdivision.levels = 1
    armature_modifier = mesh.modifiers.new("LL Asset Armature", type="ARMATURE")
    armature_modifier.object = armature
    vertex_group = mesh.vertex_groups.new(name="Mount")
    vertex_group.add([0, 1, 2], 1.0, "REPLACE")
    part = mesh.live_link_settings.components.add()
    part.type = "PART"
    part.part.part_type = "BODY"

    attachment = bpy.data.objects.new("LL Bone Attachment", None)
    source.objects.link(attachment)
    attachment.parent = armature
    attachment.parent_type = "BONE"
    attachment.parent_bone = "Mount"
    attachment.matrix_world = Matrix.Translation((0.0, 0.0, 1.0))
    attachment_data = attachment.live_link_settings.components.add()
    attachment_data.type = "ATTACHMENT_POINT"
    attachment_data.attachment_point.owner_part = mesh
    attachment_data.attachment_point.part_type = "HEAD"

    def triangle(name, collection, location, scale):
        data = bpy.data.meshes.new(name + " Data")
        data.from_pydata(
            [(0.0, 0.0, 0.0), (scale, 0.0, 0.0), (0.0, scale, 0.0)],
            [],
            [(0, 1, 2)],
        )
        data.materials.append(material)
        obj = bpy.data.objects.new(name, data)
        obj.location = location
        collection.objects.link(obj)
        return obj

    child_mesh = triangle("LL Child Mesh", ordinary_child, (0.0, 2.0, 0.0), 0.5)
    nested_mesh = triangle("LL Nested Mesh", nested, (0.0, 0.0, 2.0), 0.25)

    nested_instance = bpy.data.objects.new("LL Nested Instance", None)
    nested_instance.instance_type = "COLLECTION"
    nested_instance.instance_collection = nested
    nested_instance.location = (3.0, 0.0, 0.0)
    source.objects.link(nested_instance)

    for obj in (
        mesh, attachment, child_mesh, nested_mesh, nested_instance,
    ):
        obj.live_link_settings.enable_live_link = True
    # A referenced armature is an export dependency even if it is not enabled
    # as an independently authored source object.
    armature.live_link_settings.enable_live_link = False

    bpy.context.scene.frame_start = 1
    bpy.context.scene.frame_end = 3
    bpy.context.scene.render.fps = 30
    bpy.context.view_layer.update()
    library_path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(library_path))


def _collect(connection, roots):
    with connection.export_evaluation_context(roots) as evaluation:
        return connection.collect_export_occurrences(roots, *evaluation)


def validate(extension_module, capture_path: Path, parse_update, host_fixture_path: Path) -> None:
    library_path = capture_path.with_name(capture_path.stem + "_linked_asset.blend")
    _create_library(library_path)

    result = bpy.ops.wm.open_mainfile(filepath=str(host_fixture_path))
    if "FINISHED" not in result:
        raise AssertionError(f"Failed to restore host fixture: {result}")
    extension_module.depsgraph_update_post_callback.enabled = False
    _clear_objects_and_collections()

    with bpy.data.libraries.load(str(library_path), link=True) as (source, target):
        if "LL Linked Asset" not in source.collections:
            raise AssertionError("External fixture collection was not saved")
        target.collections = ["LL Linked Asset"]
    linked_collection = target.collections[0]

    def instance(name, collection, matrix, hidden=False):
        obj = bpy.data.objects.new(name, None)
        obj.instance_type = "COLLECTION"
        obj.instance_collection = collection
        obj.matrix_world = matrix
        bpy.context.scene.collection.objects.link(obj)
        obj.live_link_settings.enable_live_link = True
        obj.hide_set(hidden)
        return obj

    root_a = instance(
        "LL Root A", linked_collection,
        Matrix.Translation((10.0, 1.0, 2.0)) @ Matrix.Rotation(0.25, 4, "Z"),
    )
    root_b = instance(
        "LL Root B", linked_collection,
        Matrix.Translation((-4.0, 5.0, 1.0)) @ Matrix.Rotation(-0.4, 4, "Z"),
        hidden=True,
    )

    local_collection = bpy.data.collections.new("LL Local Asset")
    local_collection.use_fake_user = True
    local_collection.instance_offset = (-0.25, 0.5, 0.0)
    local_data = bpy.data.meshes.new("LL Local Mesh Data")
    local_data.from_pydata(
        [(0.0, 0.0, 0.0), (0.4, 0.0, 0.0), (0.0, 0.4, 0.0)], [], [(0, 1, 2)]
    )
    local_mesh = bpy.data.objects.new("LL Local Mesh", local_data)
    local_mesh.live_link_settings.enable_live_link = True
    local_collection.objects.link(local_mesh)
    local_root = instance(
        "LL Local Root", local_collection, Matrix.Translation((0.0, -6.0, 0.0))
    )
    bpy.context.view_layer.update()

    connection = extension_module.LiveLinkConnection()
    roots = list(bpy.context.scene.objects)
    occurrences = _collect(connection, roots)
    repeated = _collect(connection, roots)
    by_name = {occurrence.name: occurrence for occurrence in occurrences}
    repeated_uids = {occurrence.name: occurrence.unique_id for occurrence in repeated}
    if len(by_name) != len(occurrences):
        raise AssertionError("Collection occurrence names are not unique")
    if {name: value.unique_id for name, value in by_name.items()} != repeated_uids:
        raise AssertionError("Collection occurrence UIDs changed across repeated collection")

    required = (
        "LL Root A", "LL Root B", "LL Root A/LL Asset Mesh",
        "LL Root B/LL Asset Mesh", "LL Root A/LL Child Mesh",
        "LL Root A/LL Nested Instance/LL Nested Mesh",
        "LL Local Root/LL Local Mesh",
    )
    for name in required:
        if name not in by_name:
            raise AssertionError(f"Missing collection occurrence: {name}")
    expanded = [item for item in occurrences if item.occurrence_key[0] == "INSTANCE"]
    if any(not (0x40000000 <= item.unique_id <= 0x7FFFFFFF) for item in expanded):
        raise AssertionError("Expanded UID escaped the reserved positive int32 range")
    if by_name["LL Root A/LL Asset Mesh"].unique_id == by_name[
        "LL Root B/LL Asset Mesh"
    ].unique_id:
        raise AssertionError("Two placements reused the same occurrence UID")

    source_mesh = linked_collection.all_objects["LL Asset Mesh"]
    expected = (
        root_a.matrix_world
        @ Matrix.Translation(tuple(-value for value in linked_collection.instance_offset))
        @ source_mesh.matrix_world
    )
    actual = by_name["LL Root A/LL Asset Mesh"].matrix_world
    if any(abs(actual[row][column] - expected[row][column]) > 1.0e-5
           for row in range(4) for column in range(4)):
        raise AssertionError("Collection offset/world transform composition changed")

    payload = connection.make_update_python(roots, [], update_reason="ci_linked_collection")
    update = parse_update(extension_module, payload)
    exported = _objects(update)
    if update.MaterialsLength() != 1 or update.ImagesLength() != 1:
        raise AssertionError(
            "Materials/images were not deduplicated across occurrences: "
            f"{update.MaterialsLength()}/{update.ImagesLength()}"
        )
    for name, obj in exported.items():
        if name.startswith("LL Root B/") and obj.Visibility():
            raise AssertionError(f"Hidden enabled occurrence became visible: {name}")
    for root_name in ("LL Root A", "LL Root B"):
        mesh = exported[f"{root_name}/LL Asset Mesh"]
        armature = exported[f"{root_name}/LL Asset Armature"]
        attachment = exported[f"{root_name}/LL Bone Attachment"]
        if mesh.Mesh() is None or mesh.Mesh().PositionsLength() <= 9:
            raise AssertionError(f"Evaluated modifier geometry is missing for {root_name}")
        if mesh.Mesh().ArmatureId() != armature.UniqueId():
            raise AssertionError(f"Mesh armature UID escaped {root_name}")
        if armature.Armature() is None or armature.Armature().AnimationsLength() != 1:
            raise AssertionError(f"Evaluated animation is missing for {root_name}")
        attachment_data = _attachment_component(extension_module, attachment)
        if attachment_data.OwnerPartId() != mesh.UniqueId():
            raise AssertionError(f"Attachment owner UID escaped {root_name}")
        if attachment_data.ArmatureId() != armature.UniqueId():
            raise AssertionError(f"Attachment armature UID escaped {root_name}")

    captured = []
    connection.send = lambda output: captured.append(output) or True
    connection.send_scene_changes(set(), "ci_occurrence_initial", force_full=True)
    initial = {
        name: obj.UniqueId()
        for name, obj in _objects(parse_update(extension_module, captured[-1])).items()
    }

    root_a.location.x += 2.0
    bpy.context.view_layer.update()
    connection.send_scene_changes({root_a.session_uid}, "ci_move_instance")
    moved_names = set(_objects(parse_update(extension_module, captured[-1])))
    expected_moved = {
        name for name in initial if name == "LL Root A" or name.startswith("LL Root A/")
    }
    if moved_names != expected_moved:
        raise AssertionError(f"Moving one instancer updated the wrong scope: {moved_names}")

    root_a.live_link_settings.enable_live_link = False
    connection.send_scene_changes({root_a.session_uid}, "ci_disable_instance")
    disabled = parse_update(extension_module, captured[-1])
    deleted = {
        disabled.DeletedObjectUids(index)
        for index in range(disabled.DeletedObjectUidsLength())
    }
    expected_deleted = {initial[name] for name in expected_moved}
    if deleted != expected_deleted:
        raise AssertionError("Disabling an instancer did not delete its complete subtree")

    root_a.live_link_settings.enable_live_link = True
    connection.send_scene_changes({root_a.session_uid}, "ci_reenable_instance")
    restored = _objects(parse_update(extension_module, captured[-1]))
    for name in expected_moved:
        if restored[name].UniqueId() != initial[name]:
            raise AssertionError(f"Re-enabled UID was not retained: {name}")

    local_root_uid = local_root.session_uid
    bpy.data.objects.remove(local_root, do_unlink=True)
    connection.send_scene_changes({local_root_uid}, "ci_delete_instance")
    deleted_local_update = parse_update(extension_module, captured[-1])
    deleted_local = {
        deleted_local_update.DeletedObjectUids(index)
        for index in range(deleted_local_update.DeletedObjectUidsLength())
    }
    expected_deleted_local = {
        uid for name, uid in initial.items()
        if name == "LL Local Root" or name.startswith("LL Local Root/")
    }
    if deleted_local != expected_deleted_local:
        raise AssertionError("Deleting an instancer did not delete its complete subtree")

    snapshot = dict(connection.export_snapshot)
    root_b.location.y += 1.0
    bpy.context.view_layer.update()
    connection.send = lambda _output: False
    if connection.send_scene_changes({root_b.session_uid}, "ci_failed_send"):
        raise AssertionError("Synthetic failed send unexpectedly succeeded")
    if connection.export_snapshot != snapshot:
        raise AssertionError("Occurrence snapshot committed after a failed send")

    if extension_module.native_live_link_available():
        matched, message = connection.compare_native_python_full_update()
        if not matched:
            raise AssertionError(f"Linked collection native/Python parity failed: {message}")

    cycle_a = bpy.data.collections.new("LL Cycle Test A")
    cycle_b = bpy.data.collections.new("LL Cycle Test B")
    a_to_b = bpy.data.objects.new("LL Cycle Test A To B", None)
    a_to_b.instance_type = "COLLECTION"
    a_to_b.instance_collection = cycle_b
    cycle_a.objects.link(a_to_b)
    b_to_a = bpy.data.objects.new("LL Cycle Test B To A", None)
    b_to_a.instance_type = "COLLECTION"
    b_to_a.instance_collection = cycle_a
    cycle_b.objects.link(b_to_a)
    cycle_root = bpy.data.objects.new("LL Cycle Test Root", None)
    cycle_root.instance_type = "COLLECTION"
    cycle_root.instance_collection = cycle_a
    cycle_collections, cycle_keys = connection._walk_instance_collections([cycle_root])
    if len(cycle_collections) != 2 or len(cycle_keys) != 2:
        raise AssertionError("Recursive collection cycle was not rejected deterministically")

    connection.close_socket()
    print(
        "BLENDER_LIVE_LINK_CI_LINKED_COLLECTION_OK",
        f"library={library_path}",
        f"occurrences={len(occurrences)}",
        f"bytes={len(payload)}",
    )
