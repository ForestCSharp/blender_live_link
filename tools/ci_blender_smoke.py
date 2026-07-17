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

    light_data = bpy.data.lights.new("CI Sun Data", type="SUN")
    light_data.energy = 2.0
    light_object = bpy.data.objects.new("CI Sun", light_data)
    bpy.context.scene.collection.objects.link(light_object)

    for scene_object in (mesh_object, light_object):
        if not hasattr(scene_object, "live_link_settings"):
            raise AssertionError("Live Link object properties were not registered")
        scene_object.live_link_settings.enable_live_link = True

    bpy.context.view_layer.update()
    return mesh_object, light_object


def decoded_name(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8")
    return str(value)


def validate_synthetic_export(extension_module, capture_path: Path) -> None:
    clear_scene()
    mesh_object, light_object = create_synthetic_scene()

    connection = extension_module.LiveLinkConnection()
    try:
        payload = connection.make_update_python(
            [mesh_object, light_object],
            [DELETED_OBJECT_UID],
            reset=False,
            update_reason="ci_synthetic_scene",
        )
        update = parse_update(extension_module, payload)

        if update.ObjectsLength() != 2:
            raise AssertionError(f"Expected 2 exported objects, got {update.ObjectsLength()}")
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
        if set(exported_objects) != {"CI Triangle", "CI Sun"}:
            raise AssertionError(f"Unexpected exported object names: {sorted(exported_objects)}")
        if exported_objects["CI Triangle"].Mesh() is None:
            raise AssertionError("Synthetic mesh object has no mesh payload")
        if exported_objects["CI Sun"].Light() is None:
            raise AssertionError("Synthetic light object has no light payload")

        material_name = decoded_name(update.Materials(0).Name())
        if material_name != "CI Material":
            raise AssertionError(f"Unexpected material name: {material_name}")

        capture_path.parent.mkdir(parents=True, exist_ok=True)
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
    if extension_module.native_live_link_available():
        raise AssertionError("Stock Blender unexpectedly exposes the native Live Link hook")

    disable_live_update_scheduling(extension_module)
    validate_synthetic_export(extension_module, capture_path)
    validate_repository_blend_file(extension_module, blend_file)
    print("BLENDER_LIVE_LINK_CI_OK")


if __name__ == "__main__":
    main()
