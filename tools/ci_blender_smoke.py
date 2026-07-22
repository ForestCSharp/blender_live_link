"""Cross-platform Blender process smoke test for the packaged extension.

Run this file with Blender, not the system Python interpreter:

    blender --background --python-exit-code 1 --python tools/ci_blender_smoke.py \
        -- blend_files/test_file.blend /tmp/scene_update.bin
"""

from __future__ import annotations

import importlib
import struct
import sys
from pathlib import Path

import bpy
from mathutils import Matrix


DELETED_OBJECT_UID = 424242


def ci_arguments() -> tuple[Path, Path]:
    try:
        separator = sys.argv.index("--")
    except ValueError as exc:
        raise RuntimeError(
            "Expected arguments after '--': <test-file.blend> <captured-update.bin>"
        ) from exc

    arguments = sys.argv[separator + 1 :]
    if len(arguments) != 2:
        raise RuntimeError(
            "Expected exactly two arguments after '--': "
            "<test-file.blend> <captured-update.bin>"
        )

    return Path(arguments[0]).resolve(), Path(arguments[1]).resolve()


def load_extension_module():
    module_name = "bl_ext.user_default.blender_live_link.extension_main"
    try:
        return importlib.import_module(module_name)
    except ModuleNotFoundError:
        for loaded_name, module in sys.modules.items():
            if loaded_name.endswith("blender_live_link.extension_main") and hasattr(
                module, "LiveLinkConnection"
            ):
                return module
        raise RuntimeError(
            "The Blender Live Link extension is not installed and enabled; "
            f"could not import {module_name}"
        )


def disable_live_update_scheduling(extension_module) -> None:
    """Keep exporter-driven dependency graph changes from scheduling socket sends."""
    extension_module.depsgraph_update_post_callback.enabled = False
    extension_module.automatic_initial_full_update_timer.pending = False
    for callback in (
        extension_module.send_updates_timer,
        extension_module.automatic_initial_full_update_timer,
    ):
        if bpy.app.timers.is_registered(callback):
            bpy.app.timers.unregister(callback)


def parse_update(extension_module, payload: bytes):
    if len(payload) < 8:
        raise AssertionError(f"FlatBuffer payload is unexpectedly short: {len(payload)} bytes")

    (declared_size,) = struct.unpack_from("<I", payload)
    actual_size = len(payload) - 4
    if declared_size != actual_size:
        raise AssertionError(
            f"Size prefix mismatch: declared {declared_size}, actual {actual_size}"
        )

    return extension_module.Update.Update.GetRootAs(payload, 4)


def clear_scene() -> None:
    for scene_object in list(bpy.data.objects):
        bpy.data.objects.remove(scene_object, do_unlink=True)


def create_synthetic_scene():
    mesh = bpy.data.meshes.new("CI Triangle Mesh")
    mesh.from_pydata(
        [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (0.0, 1.0, 0.0)],
        [],
        [(0, 1, 2)],
    )
    mesh.update()

    material = bpy.data.materials.new("CI Material")
    material.use_nodes = True
    principled = material.node_tree.nodes.get("Principled BSDF")
    if principled is None:
        raise AssertionError("Blender did not create a Principled BSDF material node")
    principled.inputs["Base Color"].default_value = (0.2, 0.4, 0.8, 1.0)
    principled.inputs["Metallic"].default_value = 0.25
    principled.inputs["Roughness"].default_value = 0.6
    mesh.materials.append(material)

    mesh_object = bpy.data.objects.new("CI Triangle", mesh)
    bpy.context.scene.collection.objects.link(mesh_object)
    mesh_object.location = (1.0, 2.0, 3.0)

    armature_data = bpy.data.armatures.new("CI Mech Armature Data")
    armature_object = bpy.data.objects.new("CI Mech Armature", armature_data)
    bpy.context.scene.collection.objects.link(armature_object)
    bpy.context.view_layer.objects.active = armature_object
    armature_object.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    root_bone = armature_data.edit_bones.new("Root")
    root_bone.head = (0.0, 0.0, 0.0)
    root_bone.tail = (0.0, 0.0, 1.0)
    bpy.ops.object.mode_set(mode="POSE")
    pose_bone = armature_object.pose.bones["Root"]
    pose_bone.location = (0.0, 0.0, 0.0)
    pose_bone.keyframe_insert(data_path="location", frame=1)
    pose_bone.location = (0.0, 0.0, 0.25)
    pose_bone.keyframe_insert(data_path="location", frame=2)
    bpy.ops.object.mode_set(mode="OBJECT")
    armature_object.select_set(False)

    armature_modifier = mesh_object.modifiers.new("CI Mech Armature", type="ARMATURE")
    armature_modifier.object = armature_object

    def add_component(scene_object, component_type):
        component = scene_object.live_link_settings.components.add()
        component.type = component_type
        return component

    body_component = add_component(mesh_object, "PART")
    body_component.part.part_type = "BODY"
    mesh_object.hide_set(True)

    duplicate_body = bpy.data.objects.new("CI Duplicate Body", None)
    bpy.context.scene.collection.objects.link(duplicate_body)
    duplicate_body_component = add_component(duplicate_body, "PART")
    duplicate_body_component.part.part_type = "BODY"
    duplicate_body.hide_set(True)

    part_objects = {}
    for part_type, name in (
        ("LEGS", "CI Legs"),
        ("LEFT_ARM", "CI Left Arm"),
        ("RIGHT_ARM", "CI Right Arm"),
        ("HEAD", "CI Head"),
    ):
        part_object = bpy.data.objects.new(name, None)
        bpy.context.scene.collection.objects.link(part_object)
        part_component = add_component(part_object, "PART")
        part_component.part.part_type = part_type
        part_object.hide_set(True)
        part_objects[part_type] = part_object

    legs_socket = bpy.data.objects.new("CI Legs Socket", None)
    bpy.context.scene.collection.objects.link(legs_socket)
    legs_socket.parent = mesh_object
    legs_socket.matrix_parent_inverse = mesh_object.matrix_world.inverted()
    legs_socket.matrix_world = Matrix.Translation((1.0, 2.0, 2.0))
    legs_attachment = add_component(legs_socket, "ATTACHMENT_POINT")
    legs_attachment.attachment_point.owner_part = mesh_object
    legs_attachment.attachment_point.part_type = "LEGS"

    arm_sockets = []
    for part_type, name, location in (
        ("LEFT_ARM", "CI Left Arm Socket", (-1.0, 0.0, 2.5)),
        ("RIGHT_ARM", "CI Right Arm Socket", (3.0, 0.0, 2.5)),
    ):
        arm_socket = bpy.data.objects.new(name, None)
        bpy.context.scene.collection.objects.link(arm_socket)
        arm_socket.parent = mesh_object
        arm_socket.matrix_parent_inverse = mesh_object.matrix_world.inverted()
        arm_socket.matrix_world = Matrix.Translation(location)
        arm_attachment = add_component(arm_socket, "ATTACHMENT_POINT")
        arm_attachment.attachment_point.owner_part = mesh_object
        arm_attachment.attachment_point.part_type = part_type
        arm_sockets.append(arm_socket)

    head_socket = bpy.data.objects.new("CI Head Bone Socket", None)
    bpy.context.scene.collection.objects.link(head_socket)
    head_socket.parent = armature_object
    head_socket.parent_type = "BONE"
    head_socket.parent_bone = "Root"
    head_socket.matrix_world = Matrix.Translation((1.0, 2.0, 4.0))
    head_attachment = add_component(head_socket, "ATTACHMENT_POINT")
    head_attachment.attachment_point.owner_part = mesh_object
    head_attachment.attachment_point.part_type = "HEAD"

    character_object = bpy.data.objects.new("CI Player Character", None)
    bpy.context.scene.collection.objects.link(character_object)
    character_component = add_component(character_object, "CHARACTER")
    character_component.player.player_controlled = True

    second_character_object = bpy.data.objects.new("CI Second Character", None)
    bpy.context.scene.collection.objects.link(second_character_object)
    second_character_object.location = (8.0, 0.0, 0.0)
    second_character_component = add_component(second_character_object, "CHARACTER")
    second_character_component.player.player_controlled = False

    light_data = bpy.data.lights.new("CI Sun Data", type="SUN")
    light_data.energy = 2.0
    light_object = bpy.data.objects.new("CI Sun", light_data)
    bpy.context.scene.collection.objects.link(light_object)

    synthetic_objects = [
        mesh_object,
        armature_object,
        duplicate_body,
        *part_objects.values(),
        legs_socket,
        *arm_sockets,
        head_socket,
        character_object,
        second_character_object,
        light_object,
    ]
    for scene_object in synthetic_objects:
        if not hasattr(scene_object, "live_link_settings"):
            raise AssertionError("Live Link object properties were not registered")
        scene_object.live_link_settings.enable_live_link = True

    bpy.context.view_layer.update()
    return synthetic_objects


def decoded_name(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8")
    return str(value)


def validate_synthetic_export(extension_module, capture_path: Path) -> None:
    clear_scene()
    synthetic_objects = create_synthetic_scene()
    capture_path.parent.mkdir(parents=True, exist_ok=True)
    fixture_path = capture_path.with_suffix(".blend")
    bpy.ops.wm.save_as_mainfile(filepath=str(fixture_path))

    connection = extension_module.LiveLinkConnection()
    try:
        payload = connection.make_update_python(
            synthetic_objects,
            [DELETED_OBJECT_UID],
            reset=False,
            update_reason="ci_synthetic_scene",
        )
        update = parse_update(extension_module, payload)

        if update.ObjectsLength() != len(synthetic_objects):
            raise AssertionError(
                f"Expected {len(synthetic_objects)} exported objects, got {update.ObjectsLength()}"
            )
        if update.MaterialsLength() != 1:
            raise AssertionError(f"Expected 1 exported material, got {update.MaterialsLength()}")
        if update.DeletedObjectUidsLength() != 1:
            raise AssertionError("Expected one deleted-object UID")
        if update.DeletedObjectUids(0) != DELETED_OBJECT_UID:
            raise AssertionError("Deleted-object UID changed during export")
        if update.Reset():
            raise AssertionError("Synthetic scene update unexpectedly has the reset flag")

        exported_objects = {
            decoded_name(update.Objects(index).Name()): update.Objects(index)
            for index in range(update.ObjectsLength())
        }
        if exported_objects["CI Triangle"].Mesh() is None:
            raise AssertionError("Synthetic mesh object has no mesh payload")
        if exported_objects["CI Sun"].Light() is None:
            raise AssertionError("Synthetic light object has no light payload")
        if exported_objects["CI Triangle"].Visibility():
            raise AssertionError("Hidden Body part became visible during export")
        if exported_objects["CI Second Character"].ComponentsLength() != 1:
            raise AssertionError("Second Character did not export its gameplay component")

        def component_of_type(exported_object, component_type):
            for component_index in range(exported_object.ComponentsLength()):
                container = exported_object.Components(component_index)
                if container.ValueType() != component_type:
                    continue
                value = container.Value()
                return value
            return None

        part_union = component_of_type(
            exported_objects["CI Triangle"],
            extension_module.GameplayComponent.GameplayComponent.GameplayComponentPart,
        )
        if part_union is None:
            raise AssertionError("Body object has no Part component")
        part = extension_module.GameplayComponentPart.GameplayComponentPart()
        part.Init(part_union.Bytes, part_union.Pos)
        if part.PartType() != extension_module.PartType.PartType.Body:
            raise AssertionError("Body Part type changed during export")

        legs_union = component_of_type(
            exported_objects["CI Legs Socket"],
            extension_module.GameplayComponent.GameplayComponent.GameplayComponentAttachmentPoint,
        )
        legs_attachment = extension_module.GameplayComponentAttachmentPoint.GameplayComponentAttachmentPoint()
        legs_attachment.Init(legs_union.Bytes, legs_union.Pos)
        if not legs_attachment.Valid():
            raise AssertionError("Object-local Legs socket exported as invalid")
        if legs_attachment.BindingType() != extension_module.AttachmentBindingType.AttachmentBindingType.Object:
            raise AssertionError("Legs socket did not export as an object binding")
        if legs_attachment.OwnerPartId() != exported_objects["CI Triangle"].UniqueId():
            raise AssertionError("Legs socket owner UID changed during export")

        head_union = component_of_type(
            exported_objects["CI Head Bone Socket"],
            extension_module.GameplayComponent.GameplayComponent.GameplayComponentAttachmentPoint,
        )
        head_attachment = extension_module.GameplayComponentAttachmentPoint.GameplayComponentAttachmentPoint()
        head_attachment.Init(head_union.Bytes, head_union.Pos)
        if not head_attachment.Valid():
            raise AssertionError("Bone-local Head socket exported as invalid")
        if head_attachment.BindingType() != extension_module.AttachmentBindingType.AttachmentBindingType.Bone:
            raise AssertionError("Head socket did not export as a bone binding")
        if decoded_name(head_attachment.BoneName()) != "Root":
            raise AssertionError("Head socket bone name changed during export")
        if head_attachment.LocalTransform() is None:
            raise AssertionError("Head socket has no local transform")

        material_name = decoded_name(update.Materials(0).Name())
        if material_name != "CI Material":
            raise AssertionError(f"Unexpected material name: {material_name}")

        capture_path.write_bytes(payload)

        reset_payload = connection.make_update_python(
            [],
            [],
            reset=True,
            update_reason="ci_reset",
        )
        reset_update = parse_update(extension_module, reset_payload)
        if not reset_update.Reset():
            raise AssertionError("Reset batch lost its reset flag")
        if reset_update.ObjectsLength() != 0:
            raise AssertionError("Reset-only batch unexpectedly contains objects")
    finally:
        connection.close_socket()

    print(
        "BLENDER_LIVE_LINK_CI_SYNTHETIC_OK",
        f"bytes={len(payload)}",
        f"capture={capture_path}",
        f"fixture={fixture_path}",
    )


def validate_repository_blend_file(extension_module, blend_file: Path) -> None:
    if not blend_file.is_file():
        raise FileNotFoundError(f"Repository blend file was not found: {blend_file}")

    result = bpy.ops.wm.open_mainfile(filepath=str(blend_file))
    if "FINISHED" not in result:
        raise AssertionError(f"Failed to open repository blend file: {result}")

    objects = list(bpy.context.scene.objects)
    if not objects:
        raise AssertionError("Repository blend file contains no scene objects")

    connection = extension_module.LiveLinkConnection()
    try:
        payload = connection.make_update_python(
            objects,
            [],
            reset=False,
            update_reason="ci_repository_blend_file",
        )
        update = parse_update(extension_module, payload)
    finally:
        connection.close_socket()

    if update.ObjectsLength() == 0:
        raise AssertionError("Repository blend file produced an empty Live Link update")

    print(
        "BLENDER_LIVE_LINK_CI_BLEND_FILE_OK",
        f"file={blend_file}",
        f"input_objects={len(objects)}",
        f"exported_objects={update.ObjectsLength()}",
        f"materials={update.MaterialsLength()}",
        f"images={update.ImagesLength()}",
        f"bytes={len(payload)}",
    )


def main() -> None:
    blend_file, capture_path = ci_arguments()

    if not hasattr(bpy.ops, "live_link") or not hasattr(
        bpy.ops.live_link, "send_full_update"
    ):
        raise RuntimeError("Blender Live Link operators were not registered")

    extension_module = load_extension_module()
    disable_live_update_scheduling(extension_module)
    validate_synthetic_export(extension_module, capture_path)
    validate_repository_blend_file(extension_module, blend_file)
    print("BLENDER_LIVE_LINK_CI_OK")


if __name__ == "__main__":
    main()
