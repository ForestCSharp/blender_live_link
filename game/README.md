# game

The Vulkan game runtime renders the Blender live-link scene with **Vulkan (via
MoltenVK on macOS) + Volk + VMA**, windowed with **GLFW 3.4**. It uses a plain
`build.sh` instead of CMake.

## Building & running

```sh
./build.sh Mac                                  # Debug: -O0 -g + validation
./build.sh Linux                                # Debug Linux/X11 build
GAME_BUILD_CONFIG=Develop ./build.sh Mac -norun # -O2 -g + validation
GAME_BUILD_CONFIG=Release ./build.sh Mac -norun # -O3, validation off
WITH_DEBUG_UI=0 ./build.sh Mac -norun            # compile without ImGui/timings
./build.sh Mac -full                            # rebuild dependencies + shaders
./build.sh Mac -full -norun                     # full rebuild without launching
```

`GAME_ENABLE_VALIDATION=0|1` overrides the configuration default. GLFW, VMA,
and Jolt use content manifests under `bin/build/<OS>/<config>` and rebuild only
when their vendored directory changes. Shader caching remains configuration-aware.
`-full` clears the selected dependency cache and compiled shaders before building;
`-full` and `-norun` may be passed in either order.

Every build prints `[timing]` lines for shader processing, each dependency
manifest scan, dependency rebuilds, the main game compile, and the final link.
Each phase is announced before it starts. The shader step also reports how many
shader sources it scanned and compiled, making cache misses visible without
additional flags.

The standalone tonemapping and display-output tests can be run without launching
a window:

```sh
clang++ -std=c++20 -O2 tests/auto_adaptation_tests.cpp -I src \
  -o /tmp/auto_adaptation_tests && /tmp/auto_adaptation_tests
clang++ -std=c++20 -O2 tests/bloom_profile_tests.cpp -I src \
  -o /tmp/bloom_profile_tests && /tmp/bloom_profile_tests
clang++ -std=c++20 -O2 tests/gt7_tonemapping_tests.cpp -I src -I extern \
  -o /tmp/gt7_tonemapping_tests && /tmp/gt7_tonemapping_tests
clang++ -std=c++20 -O2 tests/aces2_tonemapping_tests.cpp -I src -I extern \
  -o /tmp/aces2_tonemapping_tests && /tmp/aces2_tonemapping_tests
clang++ -std=c++20 -O2 tests/agx_tonemapping_tests.cpp -I src -I extern \
  -o /tmp/agx_tonemapping_tests && /tmp/agx_tonemapping_tests
clang++ -std=c++20 -O2 tests/pbr_neutral_tonemapping_tests.cpp \
  -o /tmp/pbr_neutral_tonemapping_tests && /tmp/pbr_neutral_tonemapping_tests
clang++ -std=c++20 -O2 tests/display_encoding_tests.cpp \
  -o /tmp/display_encoding_tests && /tmp/display_encoding_tests
clang++ -std=c++20 -O2 tests/output_selection_tests.cpp -I src -I /usr/local/include \
  -o /tmp/output_selection_tests && /tmp/output_selection_tests
clang++ -std=c++20 -O2 tests/sky_aware_tonemapping_tests.cpp \
  -o /tmp/sky_aware_tonemapping_tests && /tmp/sky_aware_tonemapping_tests
clang++ -std=c++20 -O2 tests/cloud_math_tests.cpp -I src \
  -o /tmp/cloud_math_tests && /tmp/cloud_math_tests
python3 tests/cloud_protocol_tests.py
```

These check auto-exposure/AWB histogram reduction and frame-rate-independent
temporal response, normalized bloom-band reconstruction, the published GT7 vectors,
ACES 2 and Blender AgX asset headers and CRCs, SDR/EDR/HDR10 LUT reference
vectors and interpolation invariants, Khronos PBR Neutral reference behavior,
the 203/1000-nit HDR calibration, Rec.2020/PQ encoding anchors, EDR scaling, and
synthetic output-format negotiation.

The cloud tests cover spherical shell intersections and altitude ordering,
periodic deterministic noise checksums, coverage monotonicity, finite analytic
step integration, all four protocol profiles, and boundary-value round trips.
`tests/cloud_export_parity_blender.py` is the native-Blender background smoke
test used to compare every Cloud System and Cloud Layer field against the Python
exporter.

For an interactive GPU smoke test, build the game and run:

```sh
python3 tests/run_cloud_runtime_smoke.py --timings
```

The launcher fake-sends a Sun with Sky Atmosphere, the default Cumulus/Cirrus
Cloud System, and a 40 km opaque ground receiver. It starts fullscreen with a
camera looking straight up and leaves `GAME2_RENDER_SCALE` unset so the normal
dynamic-resolution default remains active. Pass `--ground-view` to aim at the
opaque receiver and inspect moving cloud shadows, or `--windowed` when needed.
Pass `--fog` to include a height-fog controller and exercise the combined
cloud/fog compositing path.
The Cloud System debug panel exposes render scale, view-step count, dense/empty
step scales, sun-cone sample count, temporal history weight, and depth rejection;
edits invalidate cloud history, and render-scale edits safely recreate targets.

The sky-aware local-tonemapping test covers geometry coverage, bright-sky
silhouettes, thin geometry, boundary suppression, and recovery-range clamping.

The formal suite adds a 110,604-sample deterministic corpus, upstream OCIO
regeneration checks, direct production-GLSL compute readback (including synthetic
framebuffer metering and adaptation updates), all 24
method/local/output renderer combinations, isolated constant-field checks, and
an HTML visual-review report:

```sh
./validate_tonemapping.sh
```

The wrapper selects Python 3.12 when available, creates the reusable ignored
environment `bin/tonemapping-validation-venv`, and installs the pinned NumPy
and PyOpenColorIO dependencies without modifying the system Python. Set
`GAME2_TONEMAP_VALIDATION_PYTHON` to select another Python 3.9–3.12
interpreter. The initial setup requires network access; later runs reuse the
environment. Arguments are forwarded to `tools/validate_tonemapping.py`, which
may still be invoked directly for advanced or dependency-free narrow runs.

The full command requires a Vulkan device for GPU/pipeline validation. Reports
and previews default to `bin/validation/tonemapping/`. Use `--skip-ocio`,
`--skip-gpu`, or `--skip-pipeline` only when deliberately running a narrower
tier. CPU and GPU results are hard conformance gates; chart images, clipping
metrics, neutral-axis error, and global/local luminance SSIM are review
artifacts rather than goldens.

Prerequisites:
- Vulkan SDK installed (headers in `/usr/local/include`, `glslc` on PATH,
  MoltenVK ICD at `/usr/local/share/vulkan/icd.d/MoltenVK_icd.json`)
- Linux: Clang, `ar`, X11 development headers, and a Vulkan loader/driver
- `../compiled_schemas/cpp/blender_live_link_generated.h` generated — run the
  repo root `./build.sh` once first
- Run the binary from this directory (`bin/shaders/*.spv` and
  `data/tonemapping/*.lutbin` paths are relative)

Third-party source dependencies are vendored under `extern/`, separate from
the first-party code in `src/`. The game owns a complete ImGui core and backend
tree.

Dependency objects, libraries, and manifests are cached under
`bin/build/<OS>/<config>`.
Keep the `JPH_*` define set empty and identical between the Jolt library and
main build — mismatched defines break the ABI.

Offline benchmark runs consume the same captured FlatBuffer update accepted by
`--file`, skip the socket thread, and exit automatically:

```sh
./bin/game --file scene_update.bin --no-live-link \
  --warmup-frames 300 --benchmark-frames 1000 \
  --benchmark-output benchmark.json
```

The JSON contains median/p95 wall, CPU, GPU, and per-pass timings plus command,
descriptor, upload, idle-wait, pipeline-creation, and VMA memory metrics. The
pipeline cache defaults to `bin/pipeline_cache.bin`; override it with
`GAME_PIPELINE_CACHE` (`GAME2_PIPELINE_CACHE` remains a compatibility alias).

## Live link

The game listens on `127.0.0.1:65432` (override with `--port`); the Blender
addon connects to it.

Sun energy in the game wire format/runtime is incident irradiance in W/m²
before atmospheric attenuation. Blender retains its familiar artistic Sun
strength; both exporters multiply that strength by `1361` while sending it to
the game (for example, Blender `2.0` becomes runtime `2722 W/m²`). `1361 W/m²`
is the Earth top-of-atmosphere reference used by the Bruneton sky; the active
atmosphere-controller Sun is additionally attenuated from each shaded or
captured world position. Existing `.blend` files therefore need no migration.
Auto exposure meters the ordinary trimmed framebuffer histogram and then adds
an exposure-only, center-weighted analytic solar guard; the guard never changes
auto white balance or increases exposure.

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

On the first Live Link update after launch, the debug camera adopts the current
Blender 3D viewport position and orientation. Later Live Link updates and scene
resets do not overwrite camera movement performed in the game. If Blender does
not provide a valid 3D viewport transform, the built-in fallback view is used.

## Debug helpers

- `GAME_PRESENT_MODE=fifo|mailbox|immediate|fifo_relaxed` — request a present
  mode; unsupported requests fall back to FIFO (`GAME2_PRESENT_MODE` alias)
- `GAME2_SCREENSHOT=<path> [GAME2_SCREENSHOT_FRAME=<n>]` — dump frame n
  (default 60) to a PPM file when the surface supports transfer-source images
- `GAME2_SCREENSHOT_WAIT_FOR_GI=1` — with `GAME2_SCREENSHOT`, wait for the
  first Live Link import and completed GI update, run a deterministic temporal
  settle, capture, and exit; timeout defaults to 600 seconds and can be changed
  with `GAME2_SCREENSHOT_TIMEOUT_SECONDS`
- `GAME2_TEST_RESIZE=1` — programmatically resize at frame 30 to exercise
  swapchain recreation
- `GAME2_RENDER_SCALE=<25..100>` — internal render resolution percentage
  (the float presentation composite upsamples to the window before UI)
- `GAME2_TONEMAP_MODE=local|gt7|agx|aces|neutral` — choose the tone method;
  `aces` selects ACES 2.0.
  The legacy `local` value means GT7 with local tonemapping enabled; named
  methods retain their historical global behavior unless explicitly overridden.
- `GAME2_LOCAL_TONEMAP=0|1` — independently disable or enable exposure-fusion
  local tonemapping for the selected method. This explicit setting takes
  precedence over the behavior implied by `GAME2_TONEMAP_MODE`.
- `GAME2_AUTO_EXPOSURE=0|1` and `GAME2_AUTO_WHITE_BALANCE=0|1` — independently
  disable or enable GPU framebuffer adaptation; both default to enabled. With
  auto exposure active, the UI exposure control is compensation in EV.
- `GAME2_TONEMAP_VALIDATION_CHART=1|constant`,
  `GAME2_TONEMAP_VALIDATION_OUTPUT_MODE=sdr|edr|hdr10`, and
  `GAME2_TONEMAP_VALIDATION_CAPTURE=<prefix>` — validation-only controls used
  by `tools/validate_tonemapping.py`; they exercise HDR profiles through an
  SDR-compatible surface and write deterministic pre-presentation PFM captures.
- `GAME2_OUTPUT_MODE=auto|sdr|edr|hdr10` — unset or `auto` prefers HDR10 and
  safely falls back to SDR. Explicit `sdr` is the compatibility/testing path;
  `edr` requires `R16G16B16A16_SFLOAT + EXTENDED_SRGB_LINEAR`, and `hdr10`
  requires an advertised `HDR10_ST2084` pair. All modes present the normal
  scene. Unsupported requests fall back to SDR and log the reason; unknown
  values use SDR, and no format/color-space pair is invented.
- `GAME2_BLOOM=0|1` — disable or enable the default HDR bloom pass
- `GAME2_BLOOM_THRESHOLD=<0..10>` / `GAME2_BLOOM_SOFT_KNEE=<0..1>` — tune
  the exposure-aware highlight selection
- `GAME2_BLOOM_INTENSITY=<0..5>` / `GAME2_BLOOM_MIPS=<1..8>` — tune bloom
  strength and the active half-resolution pyramid depth. Active frequency bands
  use a normalized diffraction-inspired profile, so changing pyramid depth
  changes the glare radius without changing its total band weight. ImGui also
  exposes Bloom Auto-Exposure Influence; it defaults to 0%, keeping glare stable
  across automatic exposure changes, while 100% restores fully exposure-aware bloom.
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
- `GAME2_GI_SPECULAR=0|1` — disable or enable roughness-aware probe specular
  IBL for deterministic A/B captures

## Architecture notes

- The game uses a unity build: only `src/main.cpp` compiles; everything else is
  headers. `main.cpp` is intentionally limited to application configuration,
  simulation ordering, benchmarking, and the outer lifecycle loop.
- Runtime orchestration is split into focused header systems:
  `live_link/live_link_system.h` owns FlatBuffer parsing, socket transport, and
  main-thread import draining; `input/input_system.h` owns GLFW callbacks and
  camera/player controls; `animation/animation_system.h` owns CPU animation
  playback and skin-matrix packing; and `render/render_system.h` owns renderer
  initialization, resizing, GI state, pass execution, and shutdown.
  `scene/scene_system.h` refreshes scene-derived sun and fog-controller
  selections, while `ui/debug_ui_system.h` samples and aggregates the frame
  timings presented by the debug UI.
- Runtime environment variables are read and parsed once by
  `core/runtime_config.h`. `core/runtime_state_overrides.h` maps the
  application-level values onto `State`; screenshot, buffer, and Vulkan code
  consume the same immutable configuration snapshot for their private options.
- Live Link message ownership types live in `live_link/live_link_types.h`,
  separate from the global runtime state that queues them. `input/input_system.h`
  exposes its mouse-lock API directly; its callback bridge forward-declares the
  ImGui initialization flag to avoid a cyclic header dependency.
- `GpuBuffer` is lazy: the live-link thread only *describes*
  buffers; the first draw on the main thread creates them (VMA, host-visible
  + persistently mapped — fine on Apple Silicon UMA).
- GPU buffer destruction goes through a deletion queue in `VulkanContext`
  (freed once the owning frame's fence has been waited), since live-link
  updates can replace meshes mid-flight.
- Vulkan 1.3 dynamic rendering + synchronization2; no render passes or
  framebuffers. Required features, extensions, queues, descriptor limits, and
  image formats are queried before device creation. Portability/debug-utils
  extensions are enabled only when advertised.
- Devices are scored after compatibility checks. Graphics and presentation may
  use separate queue families; swapchain extent, count, transforms, alpha,
  usage, and presentation mode are negotiated from surface capabilities.
- Preferred render formats are RGBA16F for scene/shadows, RGBA32F for the
  G-buffer, D32 for depth, and R8 for SSAO, with validated
  higher/lower-precision fallbacks where a preferred format is unavailable.
- Persistently mapped VMA writes are flushed through VMA, which is a no-op on
  coherent heaps and supplies the required aligned cache operation elsewhere.
- Volk loads the Vulkan loader at runtime — nothing links `libvulkan`;
  `build.sh` exports `VK_ICD_FILENAMES` to select MoltenVK.
- A negative-height viewport flips Y; the `RenderPass` framework applies the
  convention uniformly in every pass.
- Shaders are plain GLSL 450 compiled by `glslc` with `#include` support
  (`-I data/shaders`); `shader_common.h` is shared between GLSL and C++.
- Rendering goes through a `RenderPass` framework on dynamic rendering. The
  reverse-Z render chain uses cascaded EVSM shadow maps (2048²×4 layered Array
  pass + 21-tap
  separable moments blur, Frustum/CenteredSquares placement) → G-buffer
  geometry (preferably 4× RGBA32F + D32, with the far-plane sky sampled directly
  from precomputed Bruneton atmosphere LUTs) → half-res SSAO + blur → half-res
  screen-space contact shadows (trace + edge-aware filter) → cook-torrance
  lighting (point/spot/sun SSBO rings + EVSM cascade sampling) → height fog →
  DOF combine → optional shaded wireframe → temporal AA (jittered projection,
  ping-pong history) → fixed-grid auto-exposure/AWB metering → exposure-aware HDR bloom (13-tap half-resolution
  downsample pyramid + normalized, weighted additive tent reconstruction) →
  selected GT7/AgX/ACES 2.0/
  Khronos PBR Neutral method with optional exposure-fusion local tonemapping
  (GT7 + local is the default) → FXAA →
  copy-to-swapchain, all at render scale with CPU frustum culling. Camera +
  sun live in a per-frame UBO; per-object transforms in a triple-buffered
  ObjectData SSBO indexed by a push-constant `object_index`. GPU timestamps feed
  the GpuTimings system.
- Content systems (Phase 2): materials + **bindless** textures (128-slot
  sampled-image array, PARTIALLY_BOUND, rewritten per frame), armatures +
  in-shader skinning (shared per-frame skin-matrix arena ring), Jolt 5.2.1
  physics (convex-hull bodies), JPH::Character controller, fog-controller
  data. Live-link registration all happens on the main thread through one
  composite `SceneUpdate` channel message per flatbuffer update.
- Phase 3c GI uses a sparse scene octree, four-probes-per-frame cubemap
  capture, padded octahedral lighting/depth atlases, and optional SH9/SG9
  projection. The same captures feed a fixed-quality 48-pixel, four-level
  GGX-prefiltered specular atlas and split-sum BRDF LUT; nearby probes blend
  without parallax correction. Compute tessellation supports fixed and both adaptive modes,
  two rotating output/readback slots, virtual patches, Phong projection,
  skinned inputs, and shared render views across geometry/shadows/GI/wires.
- Dear ImGui uses the official GLFW + Vulkan backends with Vulkan 1.3 dynamic
  rendering. Ctrl+I exposes live-import stats, CPU/GPU timings, render and
  simulation controls, independent tone-method selection, local exposure-fusion
  and bloom tuning,
  GI/tessellation controls, probe picking, render-target viewers, and overlay
  status text. The `GAME2_*` toggles remain available for automated/headless
  verification.

### Framebuffer auto-exposure and white balance

Auto-exposure and auto-white-balance are enabled by default and run entirely on
the GPU. After TAA, a compute pass samples the unexposed, un-white-balanced
scene on a fixed 256×144 grid. It accumulates 256 integer bins over −16…+16 EV,
trims the darkest and brightest 2%, and derives exposure from geometric-mean
luminance. The same accepted bins produce a luminance-weighted framebuffer
white estimate; a small D65 virtual reference stabilizes dark frames. Bradford
LMS corrections are limited to ±1 EV.

Bloom, the local-tonemapping proxy, and the final tone pass consume the previous
adaptation state. The new measurement is applied only after final tone mapping,
so Frame N metering affects Frame N+1 and never meters its own correction.
Exposure defaults to a −8…+8 EV range, a 0.35-second darkening response, and a
1.0-second brightening response. White balance defaults to a 1.0-second response
and full correction strength. Delta time is clamped to 0.1 seconds. The final
pass applies white balance and total exposure once, while bloom thresholding
uses corrected luminance but retains raw bloom color. Bloom Auto-Exposure
Influence independently scales the automatic EV used by both thresholding and
the final glare composite. It defaults to 0%; manual exposure compensation and
white balance continue to affect bloom fully.

Ctrl+I exposes independent enable switches, minimum/maximum automatic EV,
brightening and darkening response times, white-balance response and strength,
Exposure Compensation (EV), Reset Adaptation, and delayed non-blocking
diagnostics for current/target EV, measured white xy, and current Bradford gains.
Feature-specific controls remain visible but disabled when their automatic
feature is off. Startup, re-enabling a feature, live-link scene reset,
active-camera changes, and debug/game-camera changes request a snap on the next
measured frame. Resize, ordinary camera motion, and TAA history invalidation do
not. Validation charts bypass both automatic systems to retain their established
conformance output.

This milestone is framebuffer-only gray-world metering. Strongly colored scenes
can bias the white estimate; sky, sunlight, probes, color-temperature/tint
controls, dual physiological adaptation, and replay serialization are not yet
inputs. No Live Link schema or tone-method selection changes are involved.

### Tonemapping and macOS HDR output

The selected GT7 profile is baked once at startup into the first 64 layers of a
192-layer `R16G16B16A16_SFLOAT` 2D-array LUT. Its power-of-four input shaper
covers scene-linear `[0,64]`; hardware bilinear filtering handles red/green and
the shader manually interpolates adjacent blue layers. GT7 contains the full
linear-sRGB → Rec.2020 → GT7/ICtCp → linear-sRGB transform. A fixed 1.575 EV
SDR integration calibration matches the renderer's EV-0 18% gray to the former
AgX-backed local path; it is not a claim that scene units are physical nits.
The port retains Polyphony Digital's MIT notice in
`src/render/gt7_tonemapping.h`.

AgX uses the official Blender 5.2 view transforms instead of the former compact
GLSL approximation. The checked-in SDR asset bakes `AgX Base Rec.1886`; EDR and
HDR10 share the `AgX Rec.2100-HLG - HDR 1000 nits (P3 D65)` formation. Both
include Blender's `AgX - Medium High Contrast` look. The normalized-ACEScct
64³ LUT occupies layers 128–191, stores SDR in bounded linear Rec.709, and
stores HDR as P3-limited color in the extended linear-sRGB composite basis.
Integration scales `1.1601751` and `5.1099494` map EV-0 18% gray to
approximately `0.214519` for SDR and `0.203` (203 nits) for HDR.

AgX generation is pinned to Blender `v5.2.0` commit
`fbe6228777e7d9afefcd61a413844e790ae75db7` and PyOpenColorIO 2.5.0. The
generator downloads the pinned OCIO configuration and source LUTs into a
temporary cache, verifies their SHA-256 hashes, and writes both production
assets plus `data/tonemapping/agx_manifest.json`:

```sh
python3 tools/generate_agx_luts.py
```

Normal builds have no OCIO or network dependency. See
`LICENSES/Blender-AgX-GPL-2.0-or-later.txt` for source attribution and terms.

ACES 2.0 replaces the former fitted approximation at method index 2. Its
checked-in 64³ RGBA16F LUT occupies packed array layers 64–127 and uses a
normalized ACEScct shaper over scene-linear `[0,64]`. The assets bake
linear-sRGB → ACES2065-1 → ACES 2 output rendering → normalized target-linear
color for Rec.709-D65/100-nit SDR, Rec.709-D65/1000-nit EDR, and
Rec.2100-D65/1000-nit HDR10. Deterministic integration scales `2.0548065` for
SDR and `10.9398375` for EDR/HDR10 map EV-0 18% gray to approximately
`0.214519` and `0.203`, respectively. Runtime loading validates a versioned
little-endian header, target, payload size, and CRC32 before the packed image is
uploaded; a missing or corrupt active asset fails startup with its path and a
regeneration command.

The LUTs are pinned to ACES `v2.0.0+2025.04.04` and OpenColorIO 2.5.0's
official ACES 2 fixed function. Normal builds have no OpenColorIO dependency.
To regenerate all three assets and `data/tonemapping/aces2_manifest.json`, use
a Python environment containing NumPy and exactly PyOpenColorIO 2.5.0:

```sh
python3 tools/generate_aces2_luts.py
```

The manifest records the ACES release/core/output commits, OCIO configuration,
target parameters, integration scales, checksums, reference vectors, and sampled
error metrics. See `LICENSES/ACES-v2.0.0-License.txt`,
`LICENSES/ACES-Apache-2.0.txt`, and
`LICENSES/OpenColorIO-BSD-3-Clause.txt` for attribution.

Tone-method selection and exposure fusion are independent. When local
tonemapping is enabled, the selected method evaluates the three synthetic
exposures, defines their perceptual-lightness weights, and produces the final
guided reconstruction. Disabling local tonemapping skips the proxy pyramid and
applies the same selected method globally.

Local exposure fusion is geometry-only. The G-buffer position alpha classifies
sky with an exact nearest-sampled `position.w == 0` test. Geometry coverage is
carried in the existing RGBA16F pyramid alpha channels, and positive filters
operate on premultiplied exposure and weight values so atmospheric luminance
cannot ring across silhouettes. Sky and the analytic solar disc always receive
the selected global tone operator. Geometry within a mixed 3x3 sky boundary
fades back to that global result; fully supported geometry keeps local recovery,
bounded by the authored shadow/highlight EV limits. This mask does not affect
global auto-exposure or white-balance metering, and bloom is still added after
the local multiplier. Debug views expose pyramid coverage, boundary strength,
and the applied local EV.

Khronos PBR Neutral is the pinned Apache-2.0 reference implementation for
non-negative linear Rec.709 input. It preserves well-exposed diffuse PBR colors,
starts smooth highlight compression at `0.76`, and gradually desaturates only
the compressed highlights. Exact Khronos exposure behavior is the default;
building affected shaders with
`PBR_NEUTRAL_MATCH_EXISTING_MIDDLE_GRAY=1` applies a `1.4139944` integration
scale so neutral 18% gray maps from `0.14` to approximately `0.214519`. The
official transform targets sRGB output. Its use with the normalized EDR/HDR10
display paths is an engine-specific experimental interpretation, not Khronos
HDR conformance. Enable the optional path with
`SHADER_OPT_FLAGS="-O -g -DPBR_NEUTRAL_MATCH_EXISTING_MIDDLE_GRAY=1"` when
building. See `LICENSES/Khronos-ToneMapping-Apache-2.0.txt`.

Formal conformance preserves each operator's intended identity rather than
forcing them to resemble one another: GT7 retains its ICtCp highlight-chroma
behavior, AgX includes Blender's Medium High Contrast look, ACES 2 uses its
target-specific rendering gamut, and PBR Neutral retains its dielectric offset
and deliberate highlight desaturation. A conformant result can still be a
subjective mismatch for a particular scene; the generated visual report keeps
that approval separate from numerical correctness.

HDR10 and EDR use a dedicated GT7 profile with a 1000-nit peak, 203-nit
diffuse white, and an integration scale of `11.2777778`, mapping EV-0 18% gray
to 203 nits. SDR keeps its original LUT and calibration. Tonemapped scene color
is upscaled into a full-output-resolution float composite, where ImGui white is
placed at 203 nits for HDR or 1.0 for SDR. The final copy is the only display
encoding boundary: SDR gets code-space 8-bit dithering, EDR receives extended
linear sRGB, and HDR10 converts the possibly extended linear-sRGB composite to
Rec.2020 before gamut clamping and ST-2084 PQ encoding. This lets ACES preserve
wide-gamut intermediates through bloom, FXAA, and ImGui compositing; AgX HDR
uses the same representation for its P3-limited result. HDR/EDR are undithered.

When available, `VK_EXT_hdr_metadata` submits BT.2020/D65, a 1000-nit mastering
peak, 1000-nit MaxCLL, and 400-nit MaxFALL on every HDR10 swapchain creation.
Startup logs and the Stats UI report requested/active modes, the selected pair,
metadata support, paper white, and fallback reason. Test windowed/fullscreen,
resizing, render scale, FXAA, and movement between displays manually on the
target Mac. HDR screenshots and automatic display switching remain unsupported.

See [TODO.md](../TODO.md) for the full catalog of known implementation work.
