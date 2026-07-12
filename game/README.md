# game

Golden-path Vulkan runtime, ported from `game_old/`: renders the Blender live-link scene with
**Vulkan (via MoltenVK on macOS) + Volk + VMA**, windowed with **GLFW 3.4**.
No CMake — plain `build.sh`, mirroring `game_old/`.

## Building & running

```sh
./build.sh Mac      # compiles shaders + deps + game, then runs it
WITH_DEBUG_UI=0 ./build.sh Mac -norun  # compile without ImGui/timing tooling
```

Prerequisites:
- Vulkan SDK installed (headers in `/usr/local/include`, `glslc` on PATH,
  MoltenVK ICD at `/usr/local/share/vulkan/icd.d/MoltenVK_icd.json`)
- `../compiled_schemas/cpp/blender_live_link_generated.h` generated — run the
  repo root `./build.sh` once first
- Run the binary from this directory (`bin/shaders/*.spv` paths are relative)

`bin/libglfw.a`, `bin/libvma.a`, and `bin/libjolt.a` are cached; delete them
to force a rebuild. (Keep the `JPH_*` define set empty and identical between
the Jolt lib compile and the main build — mismatched defines break the ABI.)

## Live link

The game listens on `127.0.0.1:65432` (override with `--port`); the Blender
addon connects to it. **Don't run `game_old/` and `game/` at the same time** —
both bind the same port.

## Controls

- Left click: lock mouse for camera look (Shift+Escape to unlock)
- WASD/arrows + Q/E: fly camera (Shift for 5x speed)
- WASD + Space (jump) + Shift (sprint): player character, when a
  player-controlled character exists and the debug camera is off (Ctrl+D)
- Ctrl+Space: pause/resume simulation (physics + animation playback)
- Ctrl+R: reset — restore initial transforms, reset physics bodies, rewind
  animations
- Ctrl+D: toggle debug camera
- Ctrl+I: toggle the Dear ImGui debug/tooling window
- Escape: quit

## Debug helpers

- `GAME2_SCREENSHOT=<path> [GAME2_SCREENSHOT_FRAME=<n>]` — dump frame n
  (default 60) to a PPM file
- `GAME2_TEST_RESIZE=1` — programmatically resize at frame 30 to exercise
  swapchain recreation
- `GAME2_RENDER_SCALE=<25..100>` — internal render resolution percentage
  (the copy pass upsamples to the window)
- `GAME2_PRINT_GPU_TIMINGS=1` — print GPU frame + per-pass times every 120
  frames (the same timestamp history drives the ImGui profiler timeline)
- `GAME2_FORCE_DEVICE_LOCAL=1` — route static buffers through the
  device-local/staging path even on Apple Silicon
- `GAME2_TESSELLATION=1` — enable compute tessellation
- `GAME2_TESSELLATION_MODE=<0..2>` — fixed, adaptive-per-mesh, or
  adaptive-per-triangle tessellation
- `GAME2_TESSELLATION_FACTOR=<1..31>` — fixed tessellation factor
- `GAME2_GI_PROBES=1` — render the GI probe visualization
- `GAME2_GI_RADIANCE_MODE=<0..2>` / `GAME2_GI_OCCLUSION_MODE=<0..1>` —
  select probe radiance and visibility representations for headless tests

## Architecture notes (vs game_old/ legacy runtime)

- Same unity build: only `src/main.cpp` compiles; everything else is headers.
- Same lazy `GpuBuffer` contract: the live-link thread only *describes*
  buffers; the first draw on the main thread creates them (VMA, host-visible
  + persistently mapped — fine on Apple Silicon UMA).
- GPU buffer destruction goes through a deletion queue in `VulkanContext`
  (freed once the owning frame's fence has been waited), since live-link
  updates can replace meshes mid-flight.
- Vulkan 1.3 dynamic rendering + synchronization2; no render passes or
  framebuffers. MoltenVK portability extensions always enabled.
- Volk loads the Vulkan loader at runtime — nothing links `libvulkan`;
  `build.sh` exports `VK_ICD_FILENAMES` to select MoltenVK.
- Negative-height viewport flips Y so all HMM math from `game_old/` is unchanged
  (the `RenderPass` framework applies it uniformly in every pass).
- Shaders are plain GLSL 450 compiled by `glslc` with `#include` support
  (`-I data/shaders`); `shader_common.h` is shared between GLSL and C++.
- Rendering goes through a `RenderPass` framework (port of game_old/'s) on
  dynamic rendering, running game_old/'s full post chain (Phases 3a+3b,
  reverse-Z): cascaded EVSM shadow maps (2048²×4 layered Array pass + 21-tap
  separable moments blur, Frustum/CenteredSquares placement) → G-buffer
  geometry (4× RGBA32F + D32, sky composited at the far plane from a 256²
  octahedral bake cached on sun movement) → half-res SSAO + blur → half-res
  screen-space contact shadows (trace + edge-aware filter) → cook-torrance
  lighting (point/spot/sun SSBO rings + EVSM cascade sampling) → height fog →
  DOF combine → optional shaded wireframe → temporal AA (jittered projection,
  ping-pong history) → tonemapping (exposure 1.5 + Reinhard) → FXAA →
  copy-to-swapchain, all at render scale with CPU frustum culling. Camera +
  sun live in a per-frame UBO; per-object transforms in a triple-buffered
  ObjectData SSBO indexed by a push-constant `object_index` (game_old/'s snapshot
  pattern). GPU timestamps feed the GpuTimings system.
- Content systems (Phase 2): materials + **bindless** textures (128-slot
  sampled-image array, PARTIALLY_BOUND, rewritten per frame), armatures +
  in-shader skinning (shared per-frame skin-matrix arena ring), Jolt 5.2.1
  physics (convex-hull bodies), JPH::Character controller, fog-controller
  data. Live-link registration all happens on the main thread through one
  composite `SceneUpdate` channel message per flatbuffer update.
- Phase 3c GI uses a sparse scene octree, four-probes-per-frame cubemap
  capture, padded octahedral lighting/depth atlases, and optional SH9/SG9
  projection. Compute tessellation supports fixed and both adaptive modes,
  two rotating output/readback slots, virtual patches, Phong projection,
  skinned inputs, and shared render views across geometry/shadows/GI/wires.
- Dear ImGui uses the official GLFW + Vulkan backends with Vulkan 1.3 dynamic
  rendering. Ctrl+I exposes live-import stats, CPU/GPU timings, render and
  simulation controls, GI/tessellation controls, probe picking, render-target
  viewers, and overlay status text. The `GAME2_*` toggles remain available for
  automated/headless verification.

See [TODO.md](TODO.md) for the full catalog of remaining porting work.
