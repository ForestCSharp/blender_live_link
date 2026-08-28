# Live Link Batch Transform Contract

## Transform

`Blender depsgraph objects -> Blender.LiveLink.Update -> C++ runtime resources`

The live-link batch is a size-prefixed FlatBuffer `Update`. Blender owns source
scene data until export completes. The FlatBuffer owns the serialized transfer
bytes while they cross the socket or are saved to disk. The C++ runtime copies
accepted payload data into owned CPU allocations and GPU resources, then releases
or replaces those resources on object update, delete, or reset.

## Input Layout

- `Update.objects`: zero or more direct objects or expanded collection-object
  occurrences selected for live-link export.
- `Update.deleted_object_uids`: zero or more direct or occurrence UIDs removed
  from the current export scope.
- `Update.materials`: materials referenced by exported meshes in the same batch.
- `Update.images`: image payloads referenced by exported materials in the same batch.
- `Update.reset`: when true, the runtime clears scene objects, materials, images, and cached scene indexes.

Mesh vectors are flat arrays. Blender `MESH` objects and supported baked curve
objects are both exported as evaluated mesh payloads:

- `positions`: `float`, 3 values per vertex.
- `normals`: `float`, 3 values per vertex.
- `texcoords`: `float`, 2 values per vertex.
- `indices`: `uint`, 3 values per triangle.
- `joint_indices` and `joint_weights`: optional, 4 values per vertex.

Animation matrices are frame-major, then bone-major, with 16 column-major floats
per matrix.

Native Blender grease-pencil objects are not a separate wire type in this
contract. Assets that originate as grease pencil but are baked to curves or
meshes are sent through the mesh path.

## Collection Occurrences

A local or library-linked collection-instance Empty is exported as an ordinary
Empty and also expands its collection contents. Each expanded object is an
independent `Update.objects` entry using this transform:

```text
instance world × translation(-collection instance offset) × source world
```

Ordinary child collections and nested collection-instance Empties are expanded
recursively. Cyclic instance references are skipped with a warning. This scope
does not include Geometry Nodes, particles, or other depsgraph-generated
instances.

Direct objects retain Blender's positive `session_uid`. Expanded occurrences
receive positive 31-bit IDs in `0x40000000..0x7fffffff`, derived from the root
and nested instancer UID path plus the source-object UID. Allocation is stable
for the loaded Blender session and probes around direct/occurrence collisions.
If a direct object later claims an allocated value, the old occurrence UID is
deleted and its replacement is sent. Expanded diagnostic names contain the
placement path, for example `RootInstance/NestedInstance/SourceObject`.

The instance Empty's `enable_live_link` gates its complete subtree; each source
object retains its own gate. Enabled hidden occurrences remain in the update
with `visibility=false` when any source or instancer on the path is hidden.
Character **Hide Mesh in Game** is applied afterward. Per-occurrence reference
maps keep mesh armature IDs and Attachment Point owner/armature IDs within the
matching placement. Material and image IDs remain shared between placements.

Collection sources are evaluated in a temporary scene at the active frame and
frame-rate settings. Each unique source collection is linked directly into that
scene so a hidden instance root still exports evaluated modifiers, armatures,
and animation. The temporary scene is removed after each export while Live Link
depsgraph callbacks are suspended.

## Output Layout

The runtime keeps `state.scene.objects` as the owning object table keyed by the
exported direct/occurrence UID. Hot paths consume derived category ID lists:

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

## Incremental Lifecycle

Blender rebuilds a cheap occurrence index for each timer send and diffs it
against the last successfully transmitted snapshot. New, removed, disabled,
transformed, or visibility-changed occurrences produce object updates or UID
deletions. Object, collection, geometry, armature, material, image, and action
dependency changes re-export the occurrences that recorded those IDs;
instancer changes select the Empty and its complete subtree. Unknown relevant
dependency types conservatively select all enabled occurrences without setting
`Update.reset`.

The snapshot advances only after the socket send succeeds. Failed sends retain
both the previous occurrence snapshot and queued dependency state for retry.
Native/Python comparison consumes the same collected occurrence sequence, so
UIDs, ordering, transforms, visibility, and reference remapping are identical
at the exporter boundary.

## Measurement

The Blender exporter prints one `Live Link Export Stats` line per emitted batch.
The C++ importer records the last import in `state.data_oriented.last_import`.
The debug UI shows previous-frame access counters for scene indexes, live-link
mutations, object scans, culling, drawing, lighting, skinning, and tessellation.
