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

## Phase 1 — renderer foundations (prerequisites for the pass chain)  ✅ (2026-07-09)

- [x] Per-frame UBO via descriptor set 0 (`src/render/frame_data.h` +
      `data/shaders/shader_common.h`): camera matrices/position/forward +
      sun direction/color from the scene's primary `SunLight`
      (`refresh_primary_sun_id`), with the scaffold's original hardcoded sun
      as the sunless fallback. Descriptor sets are per-frame-in-flight and
      rewritten each frame after the fence wait.
- [x] Per-object ObjectData SSBO (144-byte stride matching game/'s
      `geometry_ObjectData_t`), triple-buffered snapshot ring +
      `build_render_object_snapshot` + scene indexes in `state.h`; draws
      push only a 4-byte `object_index`. Verified through >64-object growth.
- [x] Render pass framework (`src/render/render_pass.h`) on dynamic
      rendering: Single + Swapchain types, framework-owned targets/resize/
      barriers/Y-flip viewport/timing scopes; Multi/Array/Cubemap assert
      until Phase 3 needs them. `GpuImage` gained tracked layouts +
      `gpu_image_transition`.
- [x] Offscreen conversion: forward renders to R16G16B16A16_SFLOAT + D32 at
      `render_width/height` (`update_render_resolution`, `handle_resize`,
      `GAME2_RENDER_SCALE` env); `copy_to_swapchain_pass` presents via
      fullscreen triangle. Only image cost: 1-LSB FP16 quantization on
      0.02% of pixels vs the direct-to-swapchain scaffold.
- [x] Shader infrastructure: `glslc -I data/shaders --target-env=vulkan1.3`;
      dual-mode C++/GLSL `shader_common.h` (vec4-only members; note
      "object_data" is a reserved Metal keyword — MoltenVK requires the
      game/-style `object_data_array` name). `fullscreen_vs.h`/`brdf.h` port
      with their passes in Phase 3.
- [x] Staging upload path: `GpuBufferUsage.prefer_device_local` +
      device-local default off-Apple; `vulkan_context_immediate_submit` +
      staging copy for non-host-visible allocations.
      `GAME2_FORCE_DEVICE_LOCAL=1` verified on MoltenVK (UMA lands
      host-visible so it exercises the map branch; the vkCmdCopyBuffer
      branch needs a discrete GPU to run).
- [x] ~~Back-face culling~~ Resolved as no-change: game/ itself uses
      `SG_CULLMODE_NONE` everywhere (`geometry_pass.h:57,134,181`), so
      game2 keeps `VK_CULL_MODE_NONE` for parity.
- [x] GPU timestamps: per-frame-in-flight `VkQueryPool`s, frame +
      per-render-pass events feeding
      `gpu_timings_record_completed_frame_events` with CPU frame-index
      correlation; `GAME2_PRINT_GPU_TIMINGS=1` prints e.g.
      `GPU Frame 0.225ms | Forward 0.097ms | Copy To Swapchain 0.104ms`.
      `gpu_profiler_resources.h` (view-name registry) ports with the
      profiler UI in Phase 4.

## Phase 2 — content systems  ✅ (2026-07-09)

- [x] Materials: `register_material`/`reset_materials` +
      `MaterialState` (fixed 1024-slot SSBO, set 0 binding 2) and
      `Mesh.material_ids` parsing. Registration happens at drain on the main
      thread via the composite `Channel<SceneUpdate>` (deviation from game/'s
      live-link-thread registration — Vulkan queue submits are main-thread);
      mesh material ids stay raw until `resolve_mesh_material_indices` runs
      at drain. Forward shading consumes base color + emission; metallic/
      roughness stored for Phase 3. Missing material renders grey, not
      game/'s magenta (keeps pre-material goldens valid).
- [x] Images/textures: `gpu_image_create_from_data` (staging upload via
      `immediate_submit`), **bindless** `texture2D scene_textures[128]`
      (set 0 binding 4, PARTIALLY_BOUND, rewritten each frame) + immutable
      sampler (binding 5). R8G8B8A8_UNORM (addon sends linear-encoded
      bytes). 128-image cap from MoltenVK's non-update-after-bind limits
      (maxPerStageDescriptorSampledImages 256 / maxPerStageResources 287);
      raising it needs UPDATE_AFTER_BIND on the binding + pool.
- [x] Armatures + animations parsing + `update_skinned_animations` with a
      **skin-matrix arena** (triple-buffered ring like the snapshot; no
      per-mesh skin buffers — game2's mapped stream buffers would race
      frames in flight). In-shader skinning via `forward_skinned.vert`
      (vertex buffer 1 = SkinnedVertex, `get_skin_matrix` weighted blend,
      push-constant arena offset). Verified: identity clip renders
      byte-identical to a static mesh; translated clip offsets it.
      Ctrl+R rewinds animations (game/ only has an ImGui button).
- [x] ~~GPU skinning compute pass~~ **moved to Phase 3** (tessellation item):
      game/ only consumes the compute-baked vertex cache for tessellation /
      shaded wireframe; normal skinned rendering is the in-shader path that
      shipped here. The compute pass will read the same skin-matrix arena.
- [x] Jolt physics: vendored stock 5.2.1 tree, cached `libjolt.a` (Mac) /
      `jolt.lib` (Windows — with the corrected `src/extern/Jolt` path;
      game/'s Windows branch still points at a stale location). Empty
      `JPH_*` define set on both TUs (ABI parity). `physics_system.h`
      near-verbatim; convex-hull bodies, add-before-map-insert at drain,
      `update_physics_backed_object_transforms`, Space+Ctrl sim toggle,
      Ctrl+R transform restore + body reset. Verified: dynamic cube falls
      and rests on a static floor (two runs settle within 0.002% of pixels;
      not bit-deterministic — raw variable dt, game/ parity).
- [x] Character controller: `character.h` verbatim (JPH::Character capsule),
      component parse fills settings only, Jolt character created at drain
      (deviation: main-thread-only), `update_player_character_control`
      (camera-relative WASD/Shift/Space, gated on is_simulating + debug
      camera off). Verified: capsule falls + rests, transform follows.
      WASD path is compile-verified; needs interactive mouse-lock to drive.
- [x] Fog controller data: `FogController` parse, `FogState{debug_active,
      active, active_fog_controller_id}` (deviation: id lives in FogState,
      not SceneState), lowest-uid enabled+visible selection with
      change-logging. fs_params packing + fog render pass are Phase 3.

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
