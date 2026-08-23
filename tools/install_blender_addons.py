import os
import sys
import zipfile
from pathlib import PurePosixPath

import bpy


def addon_paths_from_argv():
    try:
        separator_index = sys.argv.index("--")
    except ValueError as error:
        raise RuntimeError("Expected add-on ZIP paths after '--'") from error

    addon_paths = sys.argv[separator_index + 1 :]
    if not addon_paths:
        raise RuntimeError("No add-on ZIP paths were provided")
    return addon_paths


def module_names_from_zip(addon_path):
    module_names = set()
    with zipfile.ZipFile(addon_path) as addon_zip:
        for member_name in addon_zip.namelist():
            member_path = PurePosixPath(member_name)
            if len(member_path.parts) == 2 and member_path.name == "__init__.py":
                module_names.add(member_path.parts[0])
            elif len(member_path.parts) == 1 and member_path.suffix == ".py":
                module_names.add(member_path.stem)
    return sorted(module_names)


for addon_path in addon_paths_from_argv():
    if not os.path.isfile(addon_path):
        raise FileNotFoundError(f"Add-on ZIP was not found: {addon_path}")

    print(f"Installing and enabling add-on: {addon_path}")
    result = bpy.ops.preferences.addon_install(
        filepath=addon_path,
        overwrite=True,
        enable_on_install=False,
    )
    if "FINISHED" not in result:
        raise RuntimeError(f"Blender could not install add-on {addon_path}: {result}")

    module_names = module_names_from_zip(addon_path)
    if not module_names:
        raise RuntimeError(f"No installable add-on modules were found in {addon_path}")
    for module_name in module_names:
        result = bpy.ops.preferences.addon_enable(module=module_name)
        if "FINISHED" not in result:
            raise RuntimeError(
                f"Blender could not enable add-on module {module_name}: {result}"
            )

result = bpy.ops.wm.save_userpref()
if "FINISHED" not in result:
    raise RuntimeError(f"Blender could not save add-on preferences: {result}")
