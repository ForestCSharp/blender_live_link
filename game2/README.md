# game2

Vulkan port of `game/`: renders the Blender live-link scene with
**Vulkan (via MoltenVK on macOS) + Volk + VMA**, windowed with **GLFW 3.4**.
No CMake — plain `build.sh`, mirroring `game/`.

## Building & running

```sh
./build.sh Mac      # compiles shaders + deps + game, then runs it
```

Prerequisites:
- Vulkan SDK installed (headers in `/usr/local/include`, `glslc` on PATH,
  MoltenVK ICD at `/usr/local/share/vulkan/icd.d/MoltenVK_icd.json`)
- `../compiled_schemas/cpp/blender_live_link_generated.h` generated — run the
  repo root `./build.sh` once first
- Run the binary from this directory (`bin/shaders/*.spv` paths are relative)

`bin/libglfw.a` and `bin/libvma.a` are cached; delete them to force a rebuild.

## Live link

The game listens on `127.0.0.1:65432` (override with `--port`); the Blender
addon connects to it. **Don't run `game/` and `game2/` at the same time** —
both bind the same port.

## Controls

- Left click: lock mouse for camera look (Shift+Escape to unlock)
- WASD/arrows + Q/E: fly camera (Shift for 5x speed)
- Escape: quit

## Debug helpers

- `GAME2_SCREENSHOT=<path> [GAME2_SCREENSHOT_FRAME=<n>]` — dump frame n
  (default 60) to a PPM file
- `GAME2_TEST_RESIZE=1` — programmatically resize at frame 30 to exercise
  swapchain recreation

## Architecture notes (vs game/)

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
- Negative-height viewport flips Y so all HMM math from `game/` is unchanged.
- Scaffold scope: forward pass with push constants + hardcoded sun. Upgrade
  slots: per-frame UBO (camera/sun), then ports of `game/src/render/*_pass.h`.
  Jolt physics, ImGui, materials/images, skinning are not ported yet.

See [TODO.md](TODO.md) for the full catalog of remaining porting work.
