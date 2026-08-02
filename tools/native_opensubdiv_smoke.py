"""Verify that a native Blender build can evaluate OpenSubdiv geometry."""

from __future__ import annotations

import bpy


def main() -> None:
    if not bpy.app.build_options.opensubdiv:
        raise AssertionError("bpy.app.build_options reports OpenSubdiv is disabled")
    if not bpy.app.opensubdiv.supported:
        raise AssertionError("bpy.app.opensubdiv reports OpenSubdiv is unsupported")

    version = tuple(bpy.app.opensubdiv.version)
    if version == (0, 0, 0):
        raise AssertionError("Blender reported an invalid OpenSubdiv version")

    mesh = bpy.data.meshes.new("OpenSubdiv Smoke Quad")
    mesh.from_pydata(
        [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)],
        [],
        [(0, 1, 2, 3)],
    )
    mesh.update()

    mesh_object = bpy.data.objects.new("OpenSubdiv Smoke Object", mesh)
    bpy.context.scene.collection.objects.link(mesh_object)
    modifier = mesh_object.modifiers.new("OpenSubdiv Smoke Subdivision", type="SUBSURF")
    modifier.subdivision_type = "CATMULL_CLARK"
    modifier.levels = 2

    depsgraph = bpy.context.evaluated_depsgraph_get()
    depsgraph.update()
    evaluated_object = mesh_object.evaluated_get(depsgraph)
    evaluated_mesh = evaluated_object.to_mesh()
    try:
        source_polygon_count = len(mesh.polygons)
        evaluated_polygon_count = len(evaluated_mesh.polygons)
        if evaluated_polygon_count <= source_polygon_count:
            raise AssertionError(
                "Subdivision Surface did not produce subdivided geometry: "
                f"{source_polygon_count} source polygons, "
                f"{evaluated_polygon_count} evaluated polygons"
            )
    finally:
        evaluated_object.to_mesh_clear()

    version_string = ".".join(str(component) for component in version)
    print(
        "OPEN_SUBDIV_SMOKE_OK "
        f"version={version_string} "
        f"polygons={source_polygon_count}->{evaluated_polygon_count}"
    )


if __name__ == "__main__":
    main()
