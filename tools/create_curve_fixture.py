import bpy


def make_material(name, color):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = color
    material.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value = 0.6
    return material


def make_curve_object(name, material, location, points):
    curve = bpy.data.curves.new(name, "CURVE")
    curve.dimensions = "3D"
    curve.resolution_u = 2
    curve.bevel_depth = 0.035
    curve.bevel_resolution = 3
    curve.materials.append(material)

    spline = curve.splines.new("POLY")
    spline.points.add(len(points) - 1)
    for point, co in zip(spline.points, points):
        point.co = (co[0], co[1], co[2], 1.0)

    obj = bpy.data.objects.new(name, curve)
    bpy.context.collection.objects.link(obj)
    obj.location = location
    return obj


def main():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()

    red = make_material("Curve_Red", (1.0, 0.08, 0.04, 1.0))
    blue = make_material("Curve_Blue", (0.05, 0.25, 1.0, 1.0))

    make_curve_object(
        "Baked GP Curve Red",
        red,
        (-0.6, 0.0, 0.0),
        [(-0.4, -0.2, 0.0), (-0.1, 0.25, 0.0), (0.25, 0.1, 0.15), (0.45, 0.35, 0.0)],
    )
    make_curve_object(
        "Baked GP Curve Blue",
        blue,
        (0.55, 0.0, 0.0),
        [(-0.35, 0.25, 0.0), (-0.1, -0.25, 0.05), (0.2, -0.1, 0.0), (0.45, -0.35, 0.2)],
    )

    bpy.ops.object.light_add(type="POINT", location=(0.0, -3.0, 3.0))
    bpy.context.object.name = "Curve Fixture Light"

    bpy.ops.wm.save_as_mainfile(
        filepath="/Users/forestsharp/Desktop/blender_live_link/blend_files/curve_fixture.blend"
    )


if __name__ == "__main__":
    main()
