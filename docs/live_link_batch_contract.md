# Live Link Batch Transform Contract

## Transform

`Blender depsgraph objects -> Blender.LiveLink.Update -> C++ runtime resources`

The live-link batch is a size-prefixed FlatBuffer `Update`. Blender owns source
scene data until export completes. The FlatBuffer owns the serialized transfer
bytes while they cross the socket or are saved to disk. The C++ runtime copies
accepted payload data into owned CPU allocations and GPU resources, then releases
or replaces those resources on object update, delete, or reset.

## Input Layout

- `Update.objects`: zero or more Blender objects selected for live-link export.
- `Update.deleted_object_uids`: zero or more `session_uid` values removed from the Blender scene.
- `Update.materials`: materials referenced by exported meshes in the same batch.
- `Update.images`: image payloads referenced by exported materials in the same batch.
- `Update.reset`: when true, the runtime clears scene objects, materials, images, and cached scene indexes.

Mesh vectors are flat arrays:

- `positions`: `float`, 3 values per vertex.
- `normals`: `float`, 3 values per vertex.
- `texcoords`: `float`, 2 values per vertex.
- `indices`: `uint`, 3 values per triangle.
- `joint_indices` and `joint_weights`: optional, 4 values per vertex.

Animation matrices are frame-major, then bone-major, with 16 column-major floats
per matrix.

## Output Layout

The runtime keeps `state.scene.objects` as the owning object table keyed by
Blender `session_uid`. Hot paths consume derived category ID lists:

- `mesh_object_ids`
- `light_object_ids`
- `armature_object_ids`
- `skinned_mesh_object_ids`

Materials and images already use ID-to-index maps plus contiguous resource
buffers. Culling returns a contiguous list of visible object IDs, not pointers.

## Valid Ranges And Error Policy

- Missing object transform fields reject the whole object for that update.
- Malformed mesh vertex, index, or skinning vectors drop that optional mesh or
skinning payload and increment the malformed import counter.
- Missing materials resolve to `-1` in material index slots; render paths still
expect valid material data for visible meshes.
- Light, material, image, and animation payloads are accepted as sent; unsupported
light variants are ignored or handled by the existing runtime switch.
- Reset is explicit and destructive for runtime-owned live-link state only.

## Measurement

The Blender exporter prints one `Live Link Export Stats` line per emitted batch.
The C++ importer records the last import in `state.data_oriented.last_import`.
The debug UI shows previous-frame access counters for scene indexes, live-link
mutations, object scans, culling, drawing, lighting, skinning, and tessellation.
