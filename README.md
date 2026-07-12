# Blender Live Link

Blender Live Link is an experiment in using Blender as the primary editor for a
3D game. It is inspired by Santa Monica Studio's 2024 REAC presentation
[Maya as Editor: The game development approach of Santa Monica Studio](https://www.youtube.com/watch?v=ZwPogOhbNWw).

The repository has two main pieces:

1. A Blender extension that exports scene state through FlatBuffers.
2. A C++ game/runtime that listens for those updates and applies them live.

The project is still a research-and-development workspace, not a polished engine
distribution. The README is meant to help contributors build it, run the live
link loop, and understand what is currently wired together.

## Current Capabilities

The live link protocol currently covers:

- Object creation, updates, deletion, visibility, transforms, and reset batches.
- Mesh geometry, normals, UVs, indices, skinning data, and material IDs.
- Materials and image payloads used by exported meshes.
- Point, spot, and sun lights.
- Armatures and baked animation matrices.
- Rigid body metadata.
- Gameplay component payloads for character and camera-control experiments.
- Python exporter and native Blender exporter parity checks.

## How It Works

The data path is:

```text
Blender scene data
  -> Blender.LiveLink.Update size-prefixed FlatBuffer
  -> TCP socket on 127.0.0.1:65432
  -> C++ game/runtime scene resources
```

The Blender extension provides the UI, operators, connection management, and
packaging in both modes. The build mode controls where the
`Blender.LiveLink.Update` FlatBuffer bytes are created: either by native C++
compiled into a local Blender build, or by the Python exporter code bundled with
the extension. The runtime owns accepted payload data after import and updates
its object, material, image, lighting, skinning, and debug-stat resources from
each batch.

For a deeper protocol and runtime ownership contract, see
[docs/live_link_batch_contract.md](docs/live_link_batch_contract.md).

## Project Layout

- `__init__.py`, `extension_main.py`, `blender_manifest.toml`: Blender extension
  entrypoints, UI, operators, exporter logic, and extension metadata.
- `blender_live_link.fbs`: FlatBuffers schema for the live link payload.
- `compiled_schemas/`: generated C++ and Python schema code. This is rebuilt by
  the build script.
- `flatbuffers/`: vendored FlatBuffers dependency.
- `blend_patches/`: patches and native source files used to integrate Live Link
  into a locally built Blender.
- `blend_src/`: local Blender source/build workspace used by the native path.
- `game/`: golden-path Vulkan/GLFW C++ runtime, shaders, data, third-party
  engine libraries, and the game build script.
- `game_old/`: legacy Sokol-based runtime retained for comparison and fallback.
- `blend_files/`: sample Blender files for development and testing.
- `docs/`: protocol notes and rendering/engine explanations.
- `test_live_link_parity.sh`: native-vs-Python export parity test runner.

## Platform Notes

- macOS is the primary native Blender build path.
- Windows is supported by the main build script for the game and Python
  extension path. Use `./build.sh -python` from a shell environment capable of
  running `bash` scripts, such as Git Bash.
- Linux game build support is currently marked TODO in `game/build.sh`.
- The Blender extension requires Blender `4.2.0` or newer.
- The native Blender source build defaults to Blender `5.1.2`.

## Prerequisites

Common requirements:

- `bash`
- `clang`
- `cmake`
- `zip` on macOS/Linux, or `7z` on Windows
- Blender installed in the default system location when using `./build.sh -python`

Native Blender builds on macOS also require Blender build tooling used by
`build_blend_src.sh`, including:

- Xcode command line tools / `xcodebuild`
- `cmake`, `make`, `ninja`
- `python3`
- `autoconf`, `automake`, `bison`, `dos2unix`, `flex`, `glibtoolize`,
  `pkg-config`, `tclsh`, and `yasm`

`build_blend_src.sh` will use a private CMake 3.x install when the host CMake is
missing or too new for Blender's dependency build.

## Build And Run

Run commands from the repository root.

### Default Native Serialization Path

```sh
./build.sh
```

On macOS, this builds FlatBuffers schemas, builds or updates the local native
Blender integration, packages and installs the Blender extension into the local
Blender profile, and launches Blender with `blend_files/test_file.blend`. After
FlatBuffers schemas are generated, the Blender-side build/install/launch branch
and the C++ game build/run branch execute in parallel. In this mode the
extension calls the native
`bpy.app.live_link_make_update` hook, so FlatBuffer creation happens in C++.

### Python Serialization Path

```sh
./build.sh -python
```

This packages the same Blender extension, installs it into the system Blender,
and launches Blender in parallel with the game build/run after FlatBuffers
schemas are generated. In this mode there is no native
`bpy.app.live_link_make_update` hook, so FlatBuffer creation happens in the
extension's Python exporter code. That means the extension can be used without
building Blender from source, unlike the native serialization path.

On macOS the script expects Blender at:

```text
/Applications/Blender.app/Contents/MacOS/Blender
```

On Windows the script expects `blender.exe` to be available on `PATH`.

### Game Only

```sh
./build.sh -g
./build.sh --game
```

Either form rebuilds and runs only the C++ game. This assumes generated schemas
and required dependencies are already present from a previous full build.

The Vulkan/GLFW runtime in `game/` is the default. To build and run the legacy
Sokol runtime instead:

```sh
./build.sh -g -game_old
```

`-game_old` also applies to full native or Python serialization builds.

### Choose A Blend File

```sh
./build.sh -f test_file.blend
./build.sh -f blend_files/cornell_box.blend
./build.sh -f /absolute/path/to/file.blend
```

The `-f` / `--file` option accepts:

- A filename under `blend_files/`.
- A path relative to the repository root.
- An absolute path.

### Blender-Side Build Only

```sh
BLENDER_LIVE_LINK_SKIP_GAME=1 ./build.sh -native -f test_file.blend
```

This builds and installs the Blender-side native serialization path without
building or running the game. It is useful before running exporter-focused tests.

## Testing

Run the native/Python exporter parity check with a sample file:

```sh
./test_live_link_parity.sh -f test_file.blend
```

The test script:

1. Builds and installs the native Live Link path.
2. Opens the requested `.blend` file in the local native Blender build.
3. Runs the `live_link.compare_native_python_export` operator in background mode.
4. Writes a timestamped log under `blend_src/parity_logs/`.

Use the same `-f` path forms supported by `build.sh`.

## Native Blender Source Options

`build_blend_src.sh` manages the local Blender source tree used by the native
exporter path.

```sh
./build_blend_src.sh
./build_blend_src.sh --version 5.1.2
./build_blend_src.sh --latest
```

The default is the pinned Blender source version `5.1.2`. The script refuses to
silently replace an existing `blend_src/blender` checkout with a different
version; move that directory aside first if you intentionally want to rebuild
against another Blender release.

## More Documentation

- [docs/live_link_batch_contract.md](docs/live_link_batch_contract.md): live link
  batch format, ownership model, import policy, and measurement points.
- `docs/mesh_pipeline_explanation.html`: mesh processing notes.
- `docs/shadow_mapping_explanation.html`: shadow mapping notes.
- `docs/probe_octree_explanation.html`,
  `docs/probe_radiance_projection_explanation.html`, and
  `docs/compute_tessellation_explanation.html`: rendering experiments and engine
  implementation notes.
- `docs/sokol_gfx_local_changes.html`: local Sokol graphics notes.

## Development Notes

- The game listens on `127.0.0.1:65432`; the Blender extension connects to the
  same address.
- The extension has UI operators for sending a full update, sending a reset,
  resetting the connection, saving an update to file, and comparing native vs.
  Python export output when native support is available.
- Native export is available when the local Blender build exposes
  `bpy.app.live_link_make_update`.
- The Python export fallback can be enabled from the Blender scene setting added
  by the extension.
