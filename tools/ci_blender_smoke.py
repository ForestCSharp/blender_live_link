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


DELETED_OBJECT_UIDS = (424242, 424243, 424244)


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


def validate_compression_registries(extension_module) -> None:
    expected_components = [
        ("CHARACTER", "player"),
        ("CAMERA_CONTROL", "camera_control"),
        ("FOG_CONTROLLER", "fog_controller"),
        ("PART", "part"),
        ("ATTACHMENT_POINT", "attachment_point"),
        ("SKY_ATMOSPHERE", "sky_atmosphere"),
    ]
    actual_components = [
        (component_class.type_name, group_name)
        for component_class, group_name in extension_module.COMPONENT_SPECS
    ]
    if actual_components != expected_components:
        raise AssertionError(f"Component registry identifiers changed: {actual_components}")
    if extension_module.TYPE_TO_GROUP != dict(expected_components):
        raise AssertionError("Component type-to-group mapping diverged from COMPONENT_SPECS")

    expected_parts = [
        ("BODY", "Body", extension_module.PartType.PartType.Body, False),
        ("LEGS", "Legs", extension_module.PartType.PartType.Legs, True),
        ("LEFT_ARM", "Left Arm", extension_module.PartType.PartType.LeftArm, True),
        ("RIGHT_ARM", "Right Arm", extension_module.PartType.PartType.RightArm, True),
        ("HEAD", "Head", extension_module.PartType.PartType.Head, True),
    ]
    if extension_module.PART_TYPE_SPECS != expected_parts:
        raise AssertionError("Part registry identifiers or wire mappings changed")
    expected_attachment_ids = [spec[0] for spec in expected_parts if spec[3]]
    actual_attachment_ids = [
        identifier
        for identifier, _label, _description in extension_module.ATTACHMENT_PART_TYPE_ITEMS
    ]
    if actual_attachment_ids != expected_attachment_ids:
        raise AssertionError("Attachment eligibility diverged from PART_TYPE_SPECS")

    callback = extension_module.depsgraph_update_post_callback
    previous_enabled = callback.enabled
    callback.enabled = True
    try:
        with extension_module.suspend_depsgraph_updates():
            if callback.enabled:
                raise AssertionError("Depsgraph callback remained enabled while suspended")
            with extension_module.suspend_depsgraph_updates():
                if callback.enabled:
                    raise AssertionError("Nested depsgraph suspension re-enabled the callback")
            if callback.enabled:
                raise AssertionError("Nested suspension restored the wrong callback state")
        if not callback.enabled:
            raise AssertionError("Depsgraph suspension did not restore the prior state")

        try:
            with extension_module.suspend_depsgraph_updates():
                raise RuntimeError("intentional suspension-scope failure")
        except RuntimeError:
            pass
        if not callback.enabled:
            raise AssertionError("Exceptional depsgraph suspension did not restore state")
    finally:
        callback.enabled = previous_enabled


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
    image_a = bpy.data.images.new("CI Image A", width=1, height=1)
    image_a.pixels[:] = (0.2, 0.4, 0.8, 1.0)
    image_a_node = material.node_tree.nodes.new("ShaderNodeTexImage")
    image_a_node.image = image_a
    material.node_tree.links.new(image_a_node.outputs["Color"], principled.inputs["Base Color"])
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
    child_bone = armature_data.edit_bones.new("Child")
    child_bone.head = root_bone.tail
    child_bone.tail = (0.0, 0.0, 2.0)
    child_bone.parent = root_bone
    bpy.ops.object.mode_set(mode="POSE")
    pose_bone = armature_object.pose.bones["Root"]
    pose_bone.location = (0.0, 0.0, 0.0)
    pose_bone.keyframe_insert(data_path="location", frame=1)
    pose_bone.location = (0.0, 0.0, 0.25)
    pose_bone.keyframe_insert(data_path="location", frame=2)
    secondary_action = armature_object.animation_data.action.copy()
    secondary_action.name = "CI Secondary Action"
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
    # Deliberately bypass the add operator to verify exporters preserve an
    # invalid non-Sun host for runtime-side defensive rejection.
    add_component(duplicate_body, "SKY_ATMOSPHERE")
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

    character_mesh = bpy.data.meshes.new("CI Character Collision Mesh")
    character_mesh.from_pydata(
        [(-0.5, -0.5, 0.0), (0.5, -0.5, 0.0), (0.0, 0.5, 0.0)],
        [],
        [(0, 1, 2)],
    )
    character_mesh.update()
    character_material = bpy.data.materials.new("CI Character Material")
    character_material.use_nodes = True
    character_principled = character_material.node_tree.nodes.get("Principled BSDF")
    image_b = bpy.data.images.new("CI Image B", width=1, height=1)
    image_b.pixels[:] = (0.8, 0.3, 0.1, 1.0)
    image_b_node = character_material.node_tree.nodes.new("ShaderNodeTexImage")
    image_b_node.image = image_b
    character_material.node_tree.links.new(
        image_b_node.outputs["Color"],
        character_principled.inputs["Base Color"],
    )
    character_mesh.materials.append(character_material)
    character_object = bpy.data.objects.new("CI Player Character", character_mesh)
    bpy.context.scene.collection.objects.link(character_object)
    character_component = add_component(character_object, "CHARACTER")
    character_component.player.player_controlled = True

    second_character_object = bpy.data.objects.new("CI Second Character", None)
    bpy.context.scene.collection.objects.link(second_character_object)
    second_character_object.location = (8.0, 0.0, 0.0)
    second_character_component = add_component(second_character_object, "CHARACTER")
    second_character_component.player.player_controlled = False
    second_character_component.player.hide_mesh_in_game = False
    add_component(second_character_object, "CAMERA_CONTROL")

    blender_hidden_character_object = bpy.data.objects.new("CI Blender Hidden Character", None)
    bpy.context.scene.collection.objects.link(blender_hidden_character_object)
    blender_hidden_character_component = add_component(blender_hidden_character_object, "CHARACTER")
    blender_hidden_character_component.player.hide_mesh_in_game = False
    blender_hidden_character_object.hide_set(True)

    light_data = bpy.data.lights.new("CI Sun Data", type="SUN")
    # Blender keeps its normal artistic strength; export converts it to W/m^2.
    light_data.energy = 2.0
    light_object = bpy.data.objects.new("CI Sun", light_data)
    bpy.context.scene.collection.objects.link(light_object)
    add_component(light_object, "FOG_CONTROLLER")
    sky_component = add_component(light_object, "SKY_ATMOSPHERE")
    sky_component.sky_atmosphere.enabled = True
    sky_component.sky_atmosphere.planet_center_z_m = -6358766.0
    sky_component.sky_atmosphere.air_density = 1.2
    sky_component.sky_atmosphere.aerosol_density = 2.3
    sky_component.sky_atmosphere.ozone_density = 0.8
    sky_component.sky_atmosphere.ground_albedo = (0.2, 0.3, 0.4)
    sky_component.sky_atmosphere.sky_intensity = 1.4
    sky_component.sky_atmosphere.sun_disc_angular_diameter_degrees = 0.7
    sky_component.sky_atmosphere.sun_disc_intensity = 1.6
    sky_component.sky_atmosphere.atmosphere_height_m = 70000.0
    sky_component.sky_atmosphere.rayleigh_scale_height_m = 9000.0
    sky_component.sky_atmosphere.mie_scale_height_m = 1500.0
    sky_component.sky_atmosphere.mie_anisotropy = 0.75
    sky_component.sky_atmosphere.max_sun_zenith_angle_degrees = 105.0

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
        blender_hidden_character_object,
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
        closure = connection.collect_export_objects(
            [synthetic_objects[0], synthetic_objects[0]],
            bpy.context.scene.objects,
        )
        if closure != synthetic_objects[:2]:
            raise AssertionError(
                "Mesh/armature dependency closure lost stable ordering or UID deduplication"
            )

        payload = connection.make_update_python(
            synthetic_objects,
            list(DELETED_OBJECT_UIDS),
            reset=False,
            update_reason="ci_synthetic_scene",
        )
        update = parse_update(extension_module, payload)

        if update.ObjectsLength() != len(synthetic_objects):
            raise AssertionError(
                f"Expected {len(synthetic_objects)} exported objects, got {update.ObjectsLength()}"
            )
        if update.MaterialsLength() != 2:
            raise AssertionError(f"Expected 2 exported materials, got {update.MaterialsLength()}")
        if update.ImagesLength() != 2:
            raise AssertionError(f"Expected 2 exported images, got {update.ImagesLength()}")
        decoded_object_names = [
            decoded_name(update.Objects(index).Name())
            for index in range(update.ObjectsLength())
        ]
        expected_object_names = [obj.name for obj in reversed(synthetic_objects)]
        if decoded_object_names != expected_object_names:
            raise AssertionError(
                "Decoded object vector ordering changed: "
                f"expected {expected_object_names}, got {decoded_object_names}"
            )
        decoded_deleted_uids = [
            update.DeletedObjectUids(index)
            for index in range(update.DeletedObjectUidsLength())
        ]
        if decoded_deleted_uids != list(reversed(DELETED_OBJECT_UIDS)):
            raise AssertionError(
                f"Decoded deletion order changed: {decoded_deleted_uids}"
            )
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
        exported_sun = exported_objects["CI Sun"].Light().SunLight()
        if exported_sun is None or abs(exported_sun.Power() - 2722.0) > 1.0e-3:
            raise AssertionError(
                f"Physical Sun irradiance export mismatch: {exported_sun.Power() if exported_sun else None}"
            )

        def component_of_type(exported_object, component_type):
            for component_index in range(exported_object.ComponentsLength()):
                container = exported_object.Components(component_index)
                if container.ValueType() != component_type:
                    continue
                value = container.Value()
                return value
            return None

        sky_union = component_of_type(
            exported_objects["CI Sun"],
            extension_module.GameplayComponent.GameplayComponent.GameplayComponentSkyAtmosphere,
        )
        if sky_union is None:
            raise AssertionError("Sun object has no Sky Atmosphere component")
        sky = extension_module.GameplayComponentSkyAtmosphere.GameplayComponentSkyAtmosphere()
        sky.Init(sky_union.Bytes, sky_union.Pos)
        sky_values = (
            sky.Enabled(), sky.PlanetCenterZM(), sky.AirDensity(),
            sky.AerosolDensity(), sky.OzoneDensity(), sky.SkyIntensity(),
            sky.SunDiscAngularDiameterDegrees(), sky.SunDiscIntensity(),
            sky.AtmosphereHeightM(), sky.RayleighScaleHeightM(),
            sky.MieScaleHeightM(), sky.MieAnisotropy(),
            sky.MaxSunZenithAngleDegrees(),
        )
        expected_sky_values = (
            True, -6358766.0, 1.2, 2.3, 0.8, 1.4, 0.7, 1.6,
            70000.0, 9000.0, 1500.0, 0.75, 105.0,
        )
        for actual, expected in zip(sky_values, expected_sky_values):
            if isinstance(expected, bool):
                if actual != expected:
                    raise AssertionError(f"Sky enabled mismatch: {actual}")
            elif abs(actual - expected) > 1.0e-4:
                raise AssertionError(f"Sky field mismatch: expected {expected}, got {actual}")
        albedo = sky.GroundAlbedo()
        if albedo is None or any(abs(actual - expected) > 1.0e-4 for actual, expected in zip(
            (albedo.X(), albedo.Y(), albedo.Z()), (0.2, 0.3, 0.4)
        )):
            raise AssertionError("Sky ground albedo changed during export")
        invalid_sky_union = component_of_type(
            exported_objects["CI Duplicate Body"],
            extension_module.GameplayComponent.GameplayComponent.GameplayComponentSkyAtmosphere,
        )
        invalid_sky = extension_module.GameplayComponentSkyAtmosphere.GameplayComponentSkyAtmosphere()
        invalid_sky.Init(invalid_sky_union.Bytes, invalid_sky_union.Pos)
        if (not invalid_sky.Enabled()
                or invalid_sky.PlanetCenterZM() != -6360000.0
                or invalid_sky.AtmosphereHeightM() != 60000.0):
            raise AssertionError("Default Sky Atmosphere fields changed on invalid host fixture")
        if exported_objects["CI Triangle"].Visibility():
            raise AssertionError("Hidden Body part became visible during export")
        if exported_objects["CI Player Character"].Visibility():
            raise AssertionError("Character collision mesh was visible with the default setting")
        if exported_objects["CI Player Character"].Mesh() is None:
            raise AssertionError("Hidden Character collision mesh was omitted from the export")
        if not exported_objects["CI Second Character"].Visibility():
            raise AssertionError("Character remained hidden after disabling Hide Mesh in Game")
        if exported_objects["CI Blender Hidden Character"].Visibility():
            raise AssertionError("Hide Mesh in Game override forced a Blender-hidden object visible")
        if exported_objects["CI Second Character"].ComponentsLength() != 2:
            raise AssertionError("Second Character did not export both gameplay components")
        second_character_component_types = [
            exported_objects["CI Second Character"].Components(index).ValueType()
            for index in range(exported_objects["CI Second Character"].ComponentsLength())
        ]
        if second_character_component_types != [
            extension_module.GameplayComponent.GameplayComponent.GameplayComponentCharacter,
            extension_module.GameplayComponent.GameplayComponent.GameplayComponentCameraControl,
        ]:
            raise AssertionError(
                f"Decoded component vector ordering changed: {second_character_component_types}"
            )

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

        material_names = [
            decoded_name(update.Materials(index).Name())
            for index in range(update.MaterialsLength())
        ]
        if material_names != ["CI Material", "CI Character Material"]:
            raise AssertionError(f"Decoded material vector ordering changed: {material_names}")
        image_ids = [
            update.Images(index).UniqueId()
            for index in range(update.ImagesLength())
        ]
        expected_image_ids = [
            bpy.data.images[name].session_uid
            for name in ("CI Image A", "CI Image B")
        ]
        if image_ids != expected_image_ids:
            raise AssertionError(f"Decoded image vector ordering changed: {image_ids}")

        armature = exported_objects["CI Mech Armature"].Armature()
        bone_names = [
            decoded_name(armature.Bones(index).Name())
            for index in range(armature.BonesLength())
        ]
        if bone_names != ["Root", "Child"]:
            raise AssertionError(f"Decoded bone vector ordering changed: {bone_names}")
        expected_animation_names = [
            action.name
            for action in connection.get_armature_actions(
                bpy.data.objects["CI Mech Armature"]
            )
        ]
        animation_names = [
            decoded_name(armature.Animations(index).Name())
            for index in range(armature.AnimationsLength())
        ]
        if animation_names != expected_animation_names:
            raise AssertionError(
                "Decoded animation vector ordering changed: "
                f"expected {expected_animation_names}, got {animation_names}"
            )

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

    result = bpy.ops.wm.open_mainfile(filepath=str(fixture_path))
    if "FINISHED" not in result:
        raise AssertionError(f"Failed to reload all-component fixture: {result}")
    reloaded_component_types = {
        component.type
        for scene_object in bpy.context.scene.objects
        for component in scene_object.live_link_settings.components
    }
    expected_component_types = {
        component_class.type_name
        for component_class, _group_name in extension_module.COMPONENT_SPECS
    }
    if reloaded_component_types != expected_component_types:
        raise AssertionError(
            "Blender RNA component identifiers changed across save/reload: "
            f"{reloaded_component_types}"
        )

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
    validate_compression_registries(extension_module)
    disable_live_update_scheduling(extension_module)
    validate_synthetic_export(extension_module, capture_path)
    validate_repository_blend_file(extension_module, blend_file)
    print("BLENDER_LIVE_LINK_CI_OK")


if __name__ == "__main__":
    main()
