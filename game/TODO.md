# game Vulkan porting record

Implementation record for bringing `game/` (Vulkan + Volk + VMA + GLFW) to
parity with `game_old/` (sokol Metal). Items reference the `game_old/` source to port
from. The completed phases are retained as architectural and validation history.

## Vulkan-native hardening — checkpoint 1 ✅ (2026-07-12)

- [x] Added default Debug, Develop, and Release configurations with optimized
      C++/GLSL, configuration-specific GLFW/VMA/Jolt caches, source freshness,
      and configuration-controlled validation.
- [x] Added deterministic offline benchmark controls (`--no-live-link`,
      `--warmup-frames`, `--benchmark-frames`, `--benchmark-output`) and JSON
      reporting for wall/CPU/GPU/pass timings, draws/dispatches, descriptor
      writes, uploads, idle waits, pipeline creation, and VMA memory usage.
- [x] Added Vulkan pass labels and object names for frame/swapchain resources,
      render targets, buffers, core descriptor resources, and pipelines. Added
      Vulkan/VMA counters to the Stats UI.
- [x] Added a persistent pipeline cache guarded by device/driver UUID and the
      compiled shader hash. Empty-scene MoltenVK verification loaded 518659
      cached bytes on the second run and reduced measured pipeline creation
      from 2059.3 ms to 5.5 ms.
- [x] Compile gates passed for Debug, Develop, Release, and
      `WITH_DEBUG_UI=0`; offline benchmark runtime passed on Apple M2 Max /
      MoltenVK 1.4.1. Existing unused-vertex-attribute best-practice warnings
      remain for a later renderer cleanup checkpoint.

## Vulkan-native hardening — checkpoint 2 ✅ (2026-07-16)

- [x] Added `VulkanCapabilities` and `QueueSelection`: enumerate and score all
      devices, require Vulkan 1.3/dynamic rendering/synchronization2/demote,
      exact bindless descriptor features and limits, swapchain support, queues,
      surface support, and required image-format capabilities. Rejections now
      report all missing requirements.
- [x] Enable only the features/extensions used by the renderer. Portability
      enumeration/subset and debug-utils are conditional on advertisement;
      removed unused variable descriptor count, runtime array, and buffer
      device-address enablement.
- [x] Mark bindless material texture indexes `nonuniformEXT` and require
      `shaderSampledImageArrayNonUniformIndexing` while retaining the fixed
      128-entry partially-bound array.
- [x] Negotiate preferred/fallback scene, depth, G-buffer, shadow, and SSAO
      formats and route the selected formats through main and GI capture passes.
- [x] Flush every project-owned VMA mapped write and invalidate screenshot and
      tessellation readbacks before host access.
- [x] Complete WSI negotiation: clamp variable extents, bound image count,
      select supported transform/composite alpha/usage, support separate
      graphics and present queues, and select FIFO/Mailbox/Immediate/FIFO
      Relaxed via `GAME_PRESENT_MODE` with safe FIFO fallback. Screenshot usage
      disables cleanly on unsupported surfaces/formats.
- [x] Added a GLFW error callback and exposed selected GPU, queues, present mode,
      screenshot support, and negotiated formats in the Vulkan/VMA Stats panel.
- [x] Synchronization validation found and verified fixes for discard barriers
      losing prior access scopes and presentation being signaled before the
      final layout transition. Standard + synchronization validation now report
      no hazards on Apple M2 Max / MoltenVK 1.4.1.
- [x] Debug, Develop, Release, and `WITH_DEBUG_UI=0` compile gates passed;
      FIFO and Immediate runtime, unsupported Mailbox fallback, resize, offline
      benchmark, and screenshot readback were exercised. Best-practices-only
      warnings for unused vertex attributes, small dedicated MoltenVK image
      allocations, and deliberately uninitialized gated textures remain.

The remaining Vulkan-native hardening checkpoints (upload/lifetime scheduling,
descriptor allocation, resource-aware synchronization refactoring, and compact
G-buffer evaluation) are intentionally deferred until explicitly resumed.

## Phase 0 — small gaps in already-ported systems  ✅ (2026-07-08)

- [x] `--file` init file loading: read the file and feed it through
      `parse_flatbuffer_data` at startup (`game_old/src/main.cpp:1666-1684`).
- [x] Player camera-control follow update (`update_camera_control` in
      `src/main.cpp`, port of `game_old/src/main.cpp:2725-2763`). Note: compile-
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
- [x] Windows branch in `game/build.sh` (drafted from game_old/'s Windows branch
      + the reference build.bat — **untested on Windows**). Linux remains a
      stub, same as game_old/.

## Phase 1 — renderer foundations (prerequisites for the pass chain)  ✅ (2026-07-09)

- [x] Per-frame UBO via descriptor set 0 (`src/render/frame_data.h` +
      `data/shaders/shader_common.h`): camera matrices/position/forward +
      sun direction/color from the scene's primary `SunLight`
      (`refresh_primary_sun_id`), with the scaffold's original hardcoded sun
      as the sunless fallback. Descriptor sets are per-frame-in-flight and
      rewritten each frame after the fence wait.
- [x] Per-object ObjectData SSBO (144-byte stride matching game_old/'s
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
      game_old/-style `object_data_array` name). `fullscreen_vs.h`/`brdf.h` port
      with their passes in Phase 3.
- [x] Staging upload path: `GpuBufferUsage.prefer_device_local` +
      device-local default off-Apple; `vulkan_context_immediate_submit` +
      staging copy for non-host-visible allocations.
      `GAME2_FORCE_DEVICE_LOCAL=1` verified on MoltenVK (UMA lands
      host-visible so it exercises the map branch; the vkCmdCopyBuffer
      branch needs a discrete GPU to run).
- [x] ~~Back-face culling~~ Resolved as no-change: game_old/ itself uses
      `SG_CULLMODE_NONE` everywhere (`geometry_pass.h:57,134,181`), so
      game keeps `VK_CULL_MODE_NONE` for parity.
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
      thread via the composite `Channel<SceneUpdate>` (deviation from game_old/'s
      live-link-thread registration — Vulkan queue submits are main-thread);
      mesh material ids stay raw until `resolve_mesh_material_indices` runs
      at drain. Forward shading consumes base color + emission; metallic/
      roughness stored for Phase 3. Missing material renders grey, not
      game_old/'s magenta (keeps pre-material goldens valid).
- [x] Images/textures: `gpu_image_create_from_data` (staging upload via
      `immediate_submit`), **bindless** `texture2D scene_textures[128]`
      (set 0 binding 4, PARTIALLY_BOUND, rewritten each frame) + immutable
      sampler (binding 5). R8G8B8A8_UNORM (addon sends linear-encoded
      bytes). 128-image cap from MoltenVK's non-update-after-bind limits
      (maxPerStageDescriptorSampledImages 256 / maxPerStageResources 287);
      raising it needs UPDATE_AFTER_BIND on the binding + pool.
- [x] Armatures + animations parsing + `update_skinned_animations` with a
      **skin-matrix arena** (triple-buffered ring like the snapshot; no
      per-mesh skin buffers — game's mapped stream buffers would race
      frames in flight). In-shader skinning via `forward_skinned.vert`
      — renamed `geometry_skinned.vert` in Phase 3a —
      (vertex buffer 1 = SkinnedVertex, `get_skin_matrix` weighted blend,
      push-constant arena offset). Verified: identity clip renders
      byte-identical to a static mesh; translated clip offsets it.
      Ctrl+R rewinds animations (game_old/ only has an ImGui button).
- [x] ~~GPU skinning compute pass~~ **moved to Phase 3** (tessellation item):
      game_old/ only consumes the compute-baked vertex cache for tessellation /
      shaded wireframe; normal skinned rendering is the in-shader path that
      shipped here. The compute pass will read the same skin-matrix arena.
- [x] Jolt physics: vendored stock 5.2.1 tree, cached `libjolt.a` (Mac) /
      `jolt.lib` (Windows — with the corrected `extern/Jolt` path;
      game_old/'s Windows branch still points at a stale location). Empty
      `JPH_*` define set on both TUs (ABI parity). `physics_system.h`
      near-verbatim; convex-hull bodies, add-before-map-insert at drain,
      `update_physics_backed_object_transforms`, Space+Ctrl sim toggle,
      Ctrl+R transform restore + body reset. Verified: dynamic cube falls
      and rests on a static floor (two runs settle within 0.002% of pixels;
      not bit-deterministic — raw variable dt, game_old/ parity).
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

Port order follows game_old/'s frame order (`game_old/src/main.cpp:1469-1662`
registration; per-pass files in `game_old/src/render/`, shaders in
`game_old/data/shaders/*.glsl`). Each needs its GLSL rewritten as plain
Vulkan-style GLSL (descriptor bindings instead of sokol-shdc annotations).

### Phase 3a — deferred core  ✅ (2026-07-10)

- [x] Reverse-Z (game_old/ `USE_INVERSE_DEPTH` parity): `mat4_perspective`
      near/far swap, GREATER_OR_EQUAL, depth clear 0.0, fullscreen far
      quads at z=0, shadow ortho far/near swap. Visual no-op vs goldens.
- [x] Geometry pass (G-buffer) — 4× RGBA32F MRTs (base/emission color,
      world pos w=1-valid, world normal vec4(0)=sky sentinel,
      roughness/metallic/emission-strength) + D32, bindless texture reads
      baked in. Missing material renders LIT grey (deviation from game_old/'s
      magenta+sentinel). Replaced the scaffold `forward_pass.h` (deleted).
- [x] Lighting pass — cook-torrance `brdf.h` port, point/spot/sun light
      SSBOs as **3-deep ring buffers** (`state.lighting`, rebuilt when
      dirty, uploaded every frame), descriptor layout C (fs_params UBO +
      4 G-buffer CIS + shadow moments array CIS + 3 light SSBOs).
      fs_params keeps game_old/'s full `lighting_fs_params_t` byte layout
      (GI/SSAO/SSS toggles forced 0 until 3b/3c).
- [x] Tonemapping pass — Reinhard + `exposure_bias` push constant,
      BGRA8_SRGB offscreen target sampled by the copy pass.
- [x] Sky — `sky_atmosphere.h` + `octahedral_helpers.h` ports, 256²
      RGBA32F octahedral bake cached on sun-direction change, composite
      drawn inside the geometry pass at the far plane (4-MRT sentinel
      write). Lighting passes sky color through via the normal sentinel;
      tonemapping applies (deviation: game_old/ composites sky after tonemap).
- [x] Cascaded EVSM shadow maps — `shadow_depth_pass.h` port (Frustum
      placement mode, exponential-doubling splits, sphere-bounded frustum
      fit, no texel snapping), RenderPass **Array** type (layered
      2048²×4 RGBA16F moments + per-layer attachment views),
      `compute_cascade_matrices` on the CPU before the fs_params upload,
      EVSM4 warp/chebyshev sampling in `lighting.frag` (always compiled
      with `-DSHADOWS_ENABLED`).
- [x] CPU frustum culling — `culling.h` port wired into geometry + shadow
      cascade draws (skinned meshes bypass the frustum test, game_old/ parity).
- [x] Shadow moments separable blur — `shadow_blur_pass.h` port (21-tap
      gaussian, horizontal intermediate + vertical final Array passes,
      `state.shadow.blur_enable` default true like game_old/). This is what
      makes EVSM shadows soft — chebyshev over raw moments is near-binary.
      Note: game re-renders + re-blurs every frame (~4.5ms of GPU on
      M2 Max at 2048²×3 cascades); game_old/ caches the blur until the shadow
      map re-renders — port that caching with the 3b shadow polish.

### Phase 3b — post effects & shadow polish  ✅ (2026-07-11)

- [x] Shadow polish — `depth_freeze`/`force_recapture` semantics (frozen maps
      keep their matrices + blur; `compute_cascade_matrices` returns whether
      the map should re-render), CenteredSquares cascade placement mode +
      per-square shader-side cascade selection, cascade-selection debug tint
      (`GAME2_SHADOW_PLACEMENT` / `GAME2_SHADOW_CASCADE_DEBUG` envs until
      ImGui). Corrected a 3a note: game_old/ also re-renders + re-blurs every
      frame unless frozen, so game already matched — no caching item left.
      the cascade tint/layer viewer landed with Phase 4;
      `radial_depth.glsl` belongs to GI's lighting_capture (3c).
- [x] SSAO + SSAO blur — `ssao_pass.h` (half res, R8, 48-sample hemisphere
      kernel + 8x8 RGBA32F noise, view-space via G-buffer position/normal),
      generic `blur_pass.h` (2D counterpart of the shadow blur, push-constant
      driven), lighting samples the blurred term (`GAME2_SSAO` env).
- [x] Screen-space shadows — `screen_space_shadows_pass.h` (half-res G-buffer
      ray march toward the shadow sun + edge-aware filter); lighting
      multiplies the mask into sun shadow visibility.
- [x] Height fog pass — `fog_pass.h` + `fog.frag` (exponential height fog,
      ceiling option, Henyey-Greenstein sun in-scatter) driven by the Phase 2
      fog-controller data; downstream passes chain off its output.
- [x] DOF combine — `dof_combine_pass.h` (signed-CoC 16-tap poisson gather,
      cross-depth bleed suppression, `GAME2_DOF*` envs incl. CoC debug view).
- [x] Wire overlay — `wire_overlay_pass.h`: copy + alpha-blended barycentric
      wires via manual vertex pulling (per-mesh SSBO sets from a per-frame
      reset pool), G-buffer visibility test, `GAME2_WIREFRAME` env. Skinned/
      tessellated wires wait on the 3c GPU vertex cache. Needed
      `shaderDemoteToHelperInvocation` enabled (glslc's vulkan1.3 `discard`).
- [x] Temporal AA — `temporal_aa_pass.h`: Decima 2-phase jitter, jittered
      projection, previous-VP reprojection with neighborhood clamp/rejection
      + axis sharpen. Ping-pong via the entry's intermediate/final passes
      (each MRT [resolved, history]); history invalidated on resize
      (`GAME2_TAA` env).
- [x] FXAA — `fxaa_pass.h` (luma FXAA over the tonemapped LDR target,
      push-constant thresholds, `GAME2_FXAA` env).
- [x] Exposure parity — `exposure_bias` default corrected to game_old/'s 1.5.
- [x] Live-link robustness — getaddrinfo results are now checked + retried
      (transient resolver failures used to segfault the socket thread).

### Phase 3c — GI & tessellation  ✅ (2026-07-11)

- [x] GI system — sparse static-geometry octree, shared corner probes,
      four-probe-per-frame cubemap capture, radial-depth moments, padded
      octahedral atlases, SH9/SG9 compute projection, deferred-lighting
      sampling, layout invalidation on live updates, probe visualization,
      and camera-ray isolation picking. RenderPass Multi/Cubemap and cube
      image/view support landed with it.
- [x] GPU tessellation — five Vulkan compute stages, fixed/adaptive-per-mesh/
      adaptive-per-triangle modes, virtual patches, Phong projection, two
      rotating GPU slots, fence-safe counter readbacks, overflow fallback,
      compute-skinned inputs, and `MeshRenderView` routing for geometry,
      shadows, GI capture, and wire overlay.

## Phase 4 — debug UI & tooling  🚧 parity audit (2026-07-11)

The Vulkan/GLFW ImGui integration is functional, but the menus are not yet at
1:1 feature parity with game_old/. Extra game/ diagnostics may remain, but they do
not replace the source controls and behavior catalogued below.

- [x] Dear ImGui foundation — vendored 1.92.3-WIP core plus matching official
      GLFW and Vulkan backends, Volk, dynamic rendering, Ctrl+I, callback
      forwarding, and mouse/keyboard capture gating.
- [x] Restore a real `WITH_DEBUG_UI=0` compile-out path, default-enabled and
      selectable through the build environment.
- [x] Replace project-owned `std::unordered_map` usage with
      `ankerl::unordered_dense::map`; GI corner-to-probe lookup now matches
      game_old/. Third-party containers inside cxxopts are out of scope.
- [x] Complete the project-wide dense-map preference for scene objects,
      material/image ID registries, and ImGui texture descriptor registration;
      deterministic lowest-ID selection is explicit rather than iteration-based.
- [x] Match game_old/'s DEBUG window hierarchy and default-open sections: Stats,
      Animation, Rendering Features (Tessellation, Wireframe, Image Effects,
      Antialiasing, Depth-of-Field, Shadows, Screen Space Shadows, Lighting,
      Global Illumination), and the inline Render Texture Viewer.

### Stats and animation parity

- [x] Stats header: show separate window/render resolutions, restore the
      render-resolution percentage slider and resize path, and restore the
      immediate-vs-smoothed Frame/FPS/CPU/GPU timing summary including
      pending/unavailable states.
- [x] Port `draw_stats_ui` parity: selectable live-link import history and its
      complete metrics, last-frame update/access counters, scene object/light
      counts, and culling counts.
- [x] Animation: restore distinct Play, Pause, and Rewind actions; Skinning
      Debug View; and per-armature animation/clip selectors with the same
      playback-time reset behavior.

### Rendering-control parity

- [x] Tessellation: add Max Patches, Max Vertices, and Max Indices capacity
      controls; source-triangle count, maximum observed factor, readback
      support, and readback-age diagnostics. Match game_old/'s shadow-cache
      invalidation when settings change, with all controls participating in
      the change/invalidation path.
- [x] Wireframe: add Wire Softness and invalidate TAA history when shaded
      wireframe is toggled.
- [x] Antialiasing: restore TAA History Blend, Sharpen, Rejection, and TAA
      Debug mode (Off/History Acceptance/Previous UV), disabled-state behavior
      when TAA is off, and TAA-history invalidation for every relevant change.
- [x] Depth-of-Field: restore Focus Distance, Focus Range, Max CoC Radius,
      Foreground Scale, Background Scale, Show CoC Debug, disabled-state
      behavior, and TAA-history invalidation when DOF is toggled.
- [x] Shadows: restore Cascade Distance Scale, Centered Square Lookahead,
      Centered Square Center (editable only while frozen), and the matching
      shadow-cache invalidation behavior for cascade count/placement/center.
- [x] Screen-space shadows: restore Contact Ray Length, Thickness, Jitter
      Strength, Max Steps, Intensity, Filter Radius, Show Screen Space Shadow
      Mask, and the inline aspect-correct mask preview.

### GI and debug-view parity

- [x] GI diagnostics: show octree depth/node/payload/non-fallback-probe counts,
      atlas usage/capacity, bounds, min/max cell extents, and maximum radial
      depth rather than only the current nodes/cells/probes line.
- [x] Match GI control side effects: radiance mode, occlusion mode, sky capture,
      SH9/SG9 visualization, and constant-white debug changes must request the
      same probe recomputation; octree-depth changes must mark layout/update
      state consistently.
- [x] Restore Debug Constant White Probes and full probe-isolation UX: enabling
      isolation also shows probes and releases mouse lock, disabling clears the
      selected probe, and the panel provides Clear plus selected-probe status.
      Disable/annotate Update GI Probes while an update is already active.
- [x] Render Texture Viewer: restore shadow cascade metadata and distances,
      Debug Cascade selection, Moments-vs-Depth selection, and the dedicated
      cascade-debug render. Match the source G-buffer grouping/background and
      size previews from actual image aspect ratios instead of a fixed 16:9.
- [x] Imported-image debugger: restore the scene-backed Debug Image selector
      and Fullscreen presentation mode.

### Profiler and overlays

- [x] Port the full CPU/GPU profiler UI with the source frame-count control,
      freeze/history semantics, unaccounted-time
      toggle, resettable zoom, synchronized horizontal scrolling, nested CPU
      flame graph, GPU timeline/lanes, frame boundaries, hover tooltips,
      resource/view names, dependency details, and pending/unavailable states.
- [x] Debug text replacement — ImGui foreground status for connection, debug
      camera, and paused simulation.
- [ ] Re-run the Phase 4 acceptance matrix after parity work: Ctrl+I and input
      capture, resize/render scale, every control and its cache/history side
      effect, debug-texture lifetime, import history, profiler freeze/zoom,
      probe isolation, and the fullscreen imported-image viewer.
      Empty-scene MoltenVK validation and native live-link imports of
      `test_file.blend` and `anim_test.blend` pass; the complete interactive
      six-scene matrix remains outstanding.

## Deliberately different from game_old/ (not TODO, by design)

- GLFW callbacks + `main()` loop instead of sokol_app callbacks.
- Vulkan 1.3 dynamic rendering (no render-pass/framebuffer objects).
- Volk runtime loading — nothing links `libvulkan`; MoltenVK selected via
  `VK_ICD_FILENAMES` in `build.sh`.
- Deletion queue for GPU buffer destruction (sokol handled deferred
  destruction internally).
- Negative-height viewport for Y-flip; `HMM_Perspective_RH_ZO` kept.
- `glslc` → SPIR-V instead of sokol-shdc; shaders are plain GLSL 450.
- Debug helpers: `GAME2_SCREENSHOT[_FRAME]` frame dump, `GAME2_TEST_RESIZE`.
