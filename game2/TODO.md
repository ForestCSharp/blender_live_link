# game2 porting TODO

Catalog of remaining work to bring `game2/` (Vulkan + Volk + VMA + GLFW) to
parity with `game/` (sokol Metal). Items reference the `game/` source to port
from. Ordered roughly by suggested sequence — early items unblock later ones.

## Phase 0 — small gaps in already-ported systems  ✅ (2026-07-08)

- [x] `--file` init file loading: read the file and feed it through
      `parse_flatbuffer_data` at startup (`game/src/main.cpp:1666-1684`).
- [x] Player camera-control follow update (`update_camera_control` in
      `src/main.cpp`, port of `game/src/main.cpp:2725-2763`). Note: compile-
      verified; needs a scene with a camera-control component to exercise.
- [x] Debug camera toggle (Ctrl+D) + `DEFINE_EVENT_*`/`DEFINE_TOGGLE_*` key
      macros (GLFW keycodes, `KEYCODE_NONE` sentinel instead of
      `SAPP_KEYCODE_INVALID`).
- [x] `core/timings.h` port: `sokol_time` → `std::chrono` nanosecond ticks
      (`timings_now_ticks`/`timings_ticks_to_ms`/`timings_ticks_diff`),
      `CPU_TIMING_SCOPE` API unchanged; `WITH_DEBUG_UI` gate defined in
      `main.cpp`; frame/live-link/camera/rendering scopes annotated.
- [x] Wire indices on `Mesh` (generation in `make_mesh`, cleanup in
      `object_cleanup`; wire buffer stays CPU-only until the wire overlay
      pass draws it).
- [x] Windows branch in `game2/build.sh` (drafted from game/'s Windows branch
      + the reference build.bat — **untested on Windows**). Linux remains a
      stub, same as game/.

## Phase 1 — renderer foundations (prerequisites for the pass chain)

- [ ] Per-frame UBO via descriptor set 0 (camera view/proj, camera position,
      sun direction/color from the scene's `SunLight`) replacing the
      hardcoded sun in `forward.frag` and per-object 128-byte push constants
      as the only data path. First real descriptor-set architecture decision.
- [ ] Per-object storage buffer equivalent of `geometry_ObjectData_t`
      (model/rotation matrix + material index,
      `game/src/game_object/game_object.h:303-336`) and the render-object
      snapshot (`build_render_object_snapshot`, `game/src/main.cpp:2665`).
- [ ] Render pass framework: port `game/src/render/render_pass.h`
      (`ERenderPass` enum, `RenderPassDesc`, color/depth output images,
      resize handling) onto dynamic rendering. This is what the whole chain
      in Phase 3 hangs off.
- [ ] Offscreen render targets: extend `gpu_image.h` (sampled usage,
      mip levels, formats, samplers) + resolution percentage / render scale
      (`update_render_resolution`, `game/src/main.cpp:1356`;
      `state.window.render_width/height`).
- [ ] Shader infrastructure: shared GLSL headers via `glslc -I` includes
      (game/ preprocesses with `clang -E` then sokol-shdc; game2 can pass
      `data/shaders` as an include dir to glslc). Port
      `shader_common.h`, `fullscreen_vs.h`, `brdf.h` first.
- [ ] Staging-buffer upload path in `gpu_buffer.h` for device-local memory
      (currently everything is host-visible mapped; fine on Apple Silicon,
      wrong default for discrete GPUs on Windows/Linux).
- [ ] Back-face culling: verify Blender mesh winding against a real scene,
      then enable `VK_CULL_MODE_BACK_BIT` in `forward_pass.h` (currently
      CULL_NONE).
- [ ] GPU profiler/timestamps: port `gpu_profiler_resources.h` +
      `gpu_frame_timings` onto `vkCmdWriteTimestamp2`.

## Phase 2 — content systems

- [ ] Materials: `register_material`, `state.materials` (id→index map,
      materials storage buffer, `update_materials_buffer`,
      `game/src/main.cpp` + `game/src/state/state.h`), and
      `Mesh.material_ids` parsing (dropped in game2's
      `parse_flatbuffer_data`).
- [ ] Images/textures: `register_image` (`game/src/main.cpp:693`), image
      GPU upload (needs staging + layout transitions + samplers +
      descriptor indexing — the 1.2 features are already enabled).
- [ ] Armatures + animations parsing (`game/src/main.cpp:936-1017`,
      `Armature`/`AnimationClip`/`ArmatureBone` structs stripped from
      `game_object.h`) and `update_skinned_animations`
      (`game/src/main.cpp:2670`).
- [ ] GPU skinning compute pass (`game/src/render/gpu_skinning.h`,
      `data/shaders/gpu_skinning.glsl`; `SkinnedVertex` + skinned vertex
      cache buffers stripped from `mesh.h`).
- [ ] Jolt physics: vendor `src/extern/Jolt` (copy from game/), cached
      static lib in `build.sh` like game/'s, port
      `physics/physics_system.h`, `RigidBody` on `Object`, jolt body
      add/remove/reset (`game/src/game_object/game_object.h:175-277`),
      `update_physics_backed_object_transforms`, and rigid-body parsing.
- [ ] Character controller: `game_object/character.h`,
      `GameplayComponentCharacter` parsing (`game/src/main.cpp:1097-1120`),
      player movement (`game/src/main.cpp:1813+`). Depends on Jolt.
- [ ] Fog controller component parsing (`game/src/main.cpp:1152-1182`,
      `FogController` struct) — data side of the fog pass below.

## Phase 3 — the render pass chain

Port order follows game/'s frame order (`game/src/main.cpp:1469-1662`
registration; per-pass files in `game/src/render/`, shaders in
`game/data/shaders/*.glsl`). Each needs its GLSL rewritten as plain
Vulkan-style GLSL (descriptor bindings instead of sokol-shdc annotations).

- [ ] Geometry pass (G-buffer) — `geometry_pass.h`, `geometry.glsl`,
      `geometry_mesh_draw.h`, `culling.h`. Replaces/absorbs the scaffold
      `forward_pass.h`.
- [ ] Lighting pass — `lighting_pass.h`, `lighting.glsl` (+ `brdf.h`);
      point/spot/sun light buffers (`state.lighting` in `state.h`).
- [ ] Tonemapping pass — `tonemapping.glsl` (scene HDR color → swapchain).
      Worth porting right after lighting so output looks right.
- [ ] Cascaded shadow maps — `shadow_depth_pass.h` (depth-only pass over
      shadow casters), `shadow_blur_pass.h` (separable blur),
      `shadow_cascade_debug_pass.h`, `radial_depth.glsl`.
- [ ] SSAO + SSAO blur — ssao pass desc (`game/src/main.cpp:1485-1560`),
      `ssao.glsl`, `ssao_constants.h`, shared `blur_pass.h`.
- [ ] Screen-space shadows — `screen_space_shadows_pass.h`,
      `screen_space_shadows.glsl`.
- [ ] Sky — `sky_pass.h`, `sky_pass.glsl`, `sky_bake.glsl`,
      `sky_atmosphere.h` (SkyBakePass renders sky to probes too).
- [ ] Height fog pass — `fog_pass.h`, `fog.glsl` (+ fog controller from
      Phase 2, sun direction plumbing `game/src/main.cpp:655-663`).
- [ ] DOF combine — pass desc at `game/src/main.cpp:1573-1594`,
      `dof_combine.glsl`.
- [ ] Wire overlay — `wire_overlay_pass.h`, `wire_overlay.glsl` (needs wire
      indices from Phase 0).
- [ ] Temporal AA — `temporal_aa_pass.h`, `temporal_aa.glsl`, projection
      jitter (`game/src/main.cpp:2975-2988`).
- [ ] FXAA — `fxaa_pass.h`, `fxaa.glsl`.
- [ ] Copy-to-swapchain pass (`game/src/main.cpp:1641-1662`,
      `overlay_texture.glsl`) — needed once the chain renders offscreen at
      render scale instead of straight to the swapchain.
- [ ] GI system — `gi.h` (probe grid, octahedral encoding:
      `octahedral_helpers.h`, `cubemap_to_octahedral.glsl`,
      `probe_radiance_projection.glsl`, `gi_helpers.h`),
      `lighting_capture.h` (cubemap captures), `gi_debug_pass.h` +
      `gi_debug.glsl` + probe picking (`pick_isolated_gi_probe`,
      `game/src/main.cpp:3755`). Biggest single item; depends on most of
      the chain above.
- [ ] GPU tessellation — `tessellation.h` (compute-based, GPU slots +
      counter readbacks), `tessellation.glsl`, `tessellation_common.h`,
      `TessellatedGeometry` on `Mesh`, `mesh_get_render_view` indirection.
      Needs a `sg_buffer_readback` equivalent (buffer → host readback).

## Phase 4 — debug UI & tooling

- [ ] Dear ImGui: vendor `imgui/` (copy from `game/src/extern`), use its
      official GLFW + Vulkan backends in place of `sokol_imgui`; `WITH_DEBUG_UI`
      gate, Ctrl+I toggle, mouse-capture interplay with click-to-lock
      (`game/src/main.cpp:3719-3768`).
- [ ] Stats UI — `game/src/ui/stats_ui.h` (import stats: also restore the
      `LiveLinkImportStats` bookkeeping stripped from
      `parse_flatbuffer_data`, `game/src/main.cpp:672-1234`).
- [ ] CPU profiler UI — `game/src/ui/cpu_profiler_ui.h` (needs timings.h
      from Phase 0).
- [ ] Debug text overlay (game/ uses `sokol_debugtext`; ImGui overlay text
      is probably the simplest replacement — `draw_debug_text`,
      `game/src/main.cpp:255`).
- [ ] Per-pass debug toggles/visualizations that live in the ImGui panels
      (shadow cascade debug view, GI probe view, SSAO view, etc. —
      `game/src/main.cpp:2580-2640`).

## Deliberately different from game/ (not TODO, by design)

- GLFW callbacks + `main()` loop instead of sokol_app callbacks.
- Vulkan 1.3 dynamic rendering (no render-pass/framebuffer objects).
- Volk runtime loading — nothing links `libvulkan`; MoltenVK selected via
  `VK_ICD_FILENAMES` in `build.sh`.
- Deletion queue for GPU buffer destruction (sokol handled deferred
  destruction internally).
- Negative-height viewport for Y-flip; `HMM_Perspective_RH_ZO` kept.
- `glslc` → SPIR-V instead of sokol-shdc; shaders are plain GLSL 450.
- Debug helpers: `GAME2_SCREENSHOT[_FRAME]` frame dump, `GAME2_TEST_RESIZE`.
