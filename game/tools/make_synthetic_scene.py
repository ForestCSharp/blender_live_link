#!/usr/bin/env python3
"""Generates a synthetic live-link payload so the renderer can be benchmarked
without driving Blender.

The output is a size-prefixed FlatBuffer, byte-for-byte what the game would have
received over the live-link socket, so it replays through:

    ./bin/game --no-live-link -f tools/scenes/grid_10k_unique.bin --benchmark-frames 1000

Two variants matter and they measure different things:

  unique     - every cube's vertices are jittered by a per-object epsilon, so
               mesh de-duplication cannot collapse them. This is the scene for
               measuring draw-submission cost across stages.
  identical  - every cube is byte-identical, which is the de-duplication and
               multi-draw-indirect batching best case.

Modelled on the flatbuffer assembly in extension_main.py (see build_int32_vector
and the Mesh/Object/Update construction there).
"""

import argparse
import array
import math
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
# The generated modules do `from ... import flatbuffers`, so they only resolve
# when imported as a subpackage of compiled_schemas.python - the same way
# extension_main.py imports them.
sys.path.insert(0, REPO_ROOT)

from compiled_schemas.python import flatbuffers
from compiled_schemas.python.Blender.LiveLink import (
    EditorCamera, Light, LightType, Material, Mesh, Object, Quat, SunLight,
    Update, Vec3, Vec4)


# ---------------------------------------------------------------------------
# Fast scalar vectors without numpy.
#
# flatbuffers' CreateNumpyVector requires numpy, which the system python does
# not have. It only does StartVector followed by a raw byte write, so the same
# contract is reproduced here using the stdlib array module. Element-by-element
# Prepend* would mean ~2.4M python-level calls for a 10k-cube scene.
# ---------------------------------------------------------------------------
def create_scalar_vector(builder, values, typecode, itemsize):
    buf = array.array(typecode, values)
    if sys.byteorder != "little":
        buf.byteswap()
    raw = buf.tobytes()

    builder.StartVector(itemsize, len(values), itemsize)
    builder.head = builder.Head() - len(raw)
    builder.Bytes[builder.Head():builder.Head() + len(raw)] = raw
    builder.vectorNumElems = len(values)
    return builder.EndVector()


def create_float_vector(builder, values):
    return create_scalar_vector(builder, values, "f", 4)


def create_uint32_vector(builder, values):
    return create_scalar_vector(builder, values, "I", 4)


def create_int32_vector(builder, values):
    return create_scalar_vector(builder, values, "i", 4)


# ---------------------------------------------------------------------------
# Cube geometry: four verts per face so face normals stay flat. 24 verts, 36
# indices, 12 triangles.
# ---------------------------------------------------------------------------
CUBE_FACES = [
    ((1, 0, 0),  [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]),
    ((-1, 0, 0), [(-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1)]),
    ((0, 1, 0),  [(-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1)]),
    ((0, -1, 0), [(-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1)]),
    ((0, 0, 1),  [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
    ((0, 0, -1), [(-1, 1, -1), (1, 1, -1), (1, -1, -1), (-1, -1, -1)]),
]
FACE_UVS = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]


def build_cube(half_extent, jitter):
    """Returns (positions, normals, texcoords, indices, material_ids).

    `jitter` displaces every position by a per-object epsilon so that otherwise
    identical cubes hash differently and cannot be de-duplicated.
    """
    positions, normals, texcoords, indices = [], [], [], []
    for face_index, (normal, corners) in enumerate(CUBE_FACES):
        base = face_index * 4
        for corner_index, corner in enumerate(corners):
            positions.extend([
                corner[0] * half_extent + jitter,
                corner[1] * half_extent + jitter,
                corner[2] * half_extent + jitter,
            ])
            normals.extend(normal)
            texcoords.extend(FACE_UVS[corner_index])
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])

    material_ids = [0] * (len(indices) // 3)
    return positions, normals, texcoords, indices, material_ids


def build_scene(count, variant, spacing, half_extent):
    builder = flatbuffers.Builder(1 << 22)

    # --- material -----------------------------------------------------------
    material_name = builder.CreateString("SyntheticGrid")
    Material.Start(builder)
    Material.AddUniqueId(builder, 0)
    Material.AddName(builder, material_name)
    Material.AddBaseColor(builder, Vec4_(builder, 0.8, 0.8, 0.8, 1.0))
    Material.AddBaseColorImageId(builder, -1)
    Material.AddMetallic(builder, 0.0)
    Material.AddMetallicImageId(builder, -1)
    Material.AddRoughness(builder, 0.5)
    Material.AddRoughnessImageId(builder, -1)
    Material.AddEmissionColor(builder, Vec4_(builder, 0.0, 0.0, 0.0, 1.0))
    Material.AddEmissionColorImageId(builder, -1)
    Material.AddEmissionStrength(builder, 0.0)
    material_fb = Material.End(builder)

    Update.StartMaterialsVector(builder, 1)
    builder.PrependUOffsetTRelative(material_fb)
    materials_fb = builder.EndVector()

    # --- objects ------------------------------------------------------------
    # Square-ish grid on the XY plane so a corner camera sees most of it.
    side = int(math.ceil(math.sqrt(count)))
    shared = build_cube(half_extent, 0.0) if variant == "identical" else None

    object_offsets = []
    for i in range(count):
        geo = shared if shared is not None else build_cube(half_extent, i * 1e-5)
        positions, normals, texcoords, indices, material_ids = geo

        positions_fb = create_float_vector(builder, positions)
        normals_fb = create_float_vector(builder, normals)
        texcoords_fb = create_float_vector(builder, texcoords)
        indices_fb = create_uint32_vector(builder, indices)
        material_ids_fb = create_int32_vector(builder, material_ids)

        Mesh.Start(builder)
        Mesh.AddPositions(builder, positions_fb)
        Mesh.AddNormals(builder, normals_fb)
        Mesh.AddTexcoords(builder, texcoords_fb)
        Mesh.AddIndices(builder, indices_fb)
        Mesh.AddMaterialIds(builder, material_ids_fb)
        Mesh.AddArmatureId(builder, -1)
        mesh_fb = Mesh.End(builder)

        name_fb = builder.CreateString("Cube_%05d" % i)

        Object.Start(builder)
        Object.AddName(builder, name_fb)
        # unique_id 0 is reserved for the material above; start objects at 1.
        Object.AddUniqueId(builder, i + 1)
        Object.AddVisibility(builder, True)
        Object.AddLocation(builder, Vec3_(
            builder,
            (i % side) * spacing,
            (i // side) * spacing,
            0.0))
        Object.AddScale(builder, Vec3_(builder, 1.0, 1.0, 1.0))
        Object.AddRotation(builder, Quat_(builder, 0.0, 0.0, 0.0, 1.0))
        Object.AddMesh(builder, mesh_fb)
        object_offsets.append(Object.End(builder))

    # --- sun ----------------------------------------------------------------
    # Without a shadow-casting sun the shadow cascades never run, and the shadow
    # pass is where the per-draw push constant is heaviest (72 bytes, re-pushed
    # per draw per cascade). A benchmark with no sun would miss that entirely.
    sun_name_fb = builder.CreateString("SyntheticSun")
    Light.Start(builder)
    Light.AddType(builder, LightType.LightType.Sun)
    Light.AddColor(builder, Vec3_(builder, 1.0, 1.0, 1.0))
    Light.AddUseShadow(builder, True)
    Light.AddSunLight(builder, SunLight.CreateSunLight(builder, 5.0, True))
    sun_light_fb = Light.End(builder)

    Object.Start(builder)
    Object.AddName(builder, sun_name_fb)
    Object.AddUniqueId(builder, count + 1)
    Object.AddVisibility(builder, True)
    Object.AddLocation(builder, Vec3_(builder, 0.0, 0.0, 50.0))
    Object.AddScale(builder, Vec3_(builder, 1.0, 1.0, 1.0))
    # Tilt the default -Z sun direction ~50 degrees about X so cascades get a
    # slanted light rather than a straight-down degenerate one.
    Object.AddRotation(builder, Quat_(builder, 0.42262, 0.0, 0.0, 0.90631))
    Object.AddLight(builder, sun_light_fb)
    object_offsets.append(Object.End(builder))

    Update.StartObjectsVector(builder, len(object_offsets))
    for offset in reversed(object_offsets):
        builder.PrependUOffsetTRelative(offset)
    objects_fb = builder.EndVector()

    # --- camera -------------------------------------------------------------
    # Pull back along -Y and up in +Z, looking at the middle of the grid, so the
    # bulk of the cubes are on screen and the benchmark measures submission
    # rather than culling.
    extent = side * spacing
    EditorCamera.Start(builder)
    EditorCamera.AddLocation(builder, Vec3_(
        builder, extent * 0.5, -extent * 0.75, extent * 0.6))
    EditorCamera.AddForward(builder, Vec3_(builder, 0.0, 0.7071, -0.7071))
    EditorCamera.AddUp(builder, Vec3_(builder, 0.0, 0.7071, 0.7071))
    camera_fb = EditorCamera.End(builder)

    # --- update -------------------------------------------------------------
    # Deliberately no AddReset: the drain processes `reset` *after* inserting
    # the update's objects (live_link_system.h, scene_clear_objects), so setting
    # it here would wipe the very scene this payload delivers.
    Update.Start(builder)
    Update.AddObjects(builder, objects_fb)
    Update.AddMaterials(builder, materials_fb)
    Update.AddEditorCamera(builder, camera_fb)
    update_fb = Update.End(builder)

    # The engine reads the payload through GetSizePrefixedUpdate, so the size
    # prefix is mandatory - a plain Finish() produces a file it cannot parse.
    builder.FinishSizePrefixed(update_fb)
    return builder.Output()


# Struct constructors are module-level functions in generated code; these keep
# the call sites above readable.
def Vec3_(builder, x, y, z):
    return Vec3.CreateVec3(builder, x, y, z)


def Vec4_(builder, x, y, z, w):
    return Vec4.CreateVec4(builder, x, y, z, w)


def Quat_(builder, x, y, z, w):
    return Quat.CreateQuat(builder, x, y, z, w)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--count", type=int, default=10000, help="cube count (default 10000)")
    parser.add_argument("--variant", choices=["unique", "identical"], default="unique")
    parser.add_argument("--spacing", type=float, default=3.0)
    parser.add_argument("--half-extent", type=float, default=0.5)
    parser.add_argument("-o", "--output", default=None)
    args = parser.parse_args()

    output = args.output or os.path.join(
        SCRIPT_DIR, "scenes", "grid_%dk_%s.bin" % (args.count // 1000, args.variant))
    os.makedirs(os.path.dirname(output), exist_ok=True)

    payload = build_scene(args.count, args.variant, args.spacing, args.half_extent)
    with open(output, "wb") as f:
        f.write(payload)

    print("wrote %s (%d objects, %s, %.2f MB)"
          % (output, args.count, args.variant, len(payload) / (1024.0 * 1024.0)))


if __name__ == "__main__":
    main()
