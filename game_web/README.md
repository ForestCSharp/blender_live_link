# Web renderer

A static-geometry Three.js viewer for Blender Live Link. It uses checked-in
Three.js r180 modules and Python 3.9+'s standard library, with the repository's
existing generated Python FlatBuffers bindings and bundled FlatBuffers runtime.
No npm, pip packages, compiler, bundler, or runtime downloads are needed for the
web path. The full project build still has its existing Blender/schema tooling
requirements. A browser with WebGL2 and ES modules is required.

## Run

From the project root:

```sh
./build.sh -web                 # Native Blender build + web viewer
./build.sh -python -web         # Installed Blender + web viewer
./build.sh -g -web              # Web only, using existing generated schemas
./build.sh -web -f cube.blend    # Choose the file Blender opens
```

The renderer serves http://127.0.0.1:8000 and opens your default browser. Enable
Live Link in Blender as with the native runtime. The bridge listens on
127.0.0.1:65432. Relaunching automatically replaces this checkout's previous
web bridge, including one using a fallback HTTP port. On systems with `lsof`
and `ps`, the launcher also recognizes and stops this checkout's native game
or older web bridge when it holds the Live Link port. It does not terminate
unrelated processes. If another app owns HTTP port 8000, the viewer uses an
available port and prints/opens its actual URL. Blender's TCP port defaults to
65432 and follows `$BLENDER_LIVE_LINK_PORT`, the same variable Blender and the
native game read; an unidentified owner of that port is reported rather than
terminated.

Closing the last viewer tab stops the renderer and releases the Blender TCP
port, so a native game run started afterwards can bind it. Each open page holds
an `/api/alive` connection; the bridge stops a few seconds after the last one
drops, and a reload reconnects well inside that window. Pass
`--idle-timeout 0` to keep it running until Ctrl+C instead.

Only one Blender producer is accepted at a time. Ctrl+C stops the bridge.
Closing the browser tab does not stop the bridge. A subsequent launch restarts
it automatically; existing tabs on a fallback port may need the new printed URL.
Handoff uses a private temporary state file and a per-launch shutdown token;
normal web relaunch requires only the Python standard library.

Standalone validation / launch (use Linux or Windows in place of Mac as needed):

```sh
./game_web/build.sh Mac -norun
./game_web/build.sh Mac
python3 game_web/bridge.py --no-browser
```

`-full` repeats the complete asset validation; there is no compilation step.
Missing generated schemas require running the root build first. `-web` does not
change `-native`, `-python`, `--package-only`, or the meaning of `-f` (a Blender
file). Automated native screenshot mode cannot be combined with `-web`.

## Camera and scene controls

The first exported editor camera seeds the debug camera. The bridge retains that
pose even if the browser connects after several updates. Missing or invalid
camera data uses the native default `(2.5, -15, 3)`, looking along normalized
`(0, 1, -0.5)`. Navigation uses Z-up and a 60° vertical FOV; vertical views retain
Blender's up vector until the user looks around. Blender viewport roll is not
imported into ordinary Z-up navigation, matching `game/`.

Click the canvas to capture the pointer. Mouse movement looks around the camera's
current position. WASD/arrows move horizontally, Q/E move vertically, and Shift
increases the 10 units/second speed by 5×. Escape releases the pointer. Losing
focus or pointer capture clears held keys. **Frame scene** (F) fits visible
geometry only when explicitly requested; **Reset camera** restores the initial
pose. Scene edits, resets, reconnects, and returning from file mode to live mode
do not overwrite navigation. Explicitly opening a snapshot seeds its own camera
(or the native fallback).

**Preview lights** has Auto, On, and Off modes. Auto enables neutral inspection
lighting only when there are no supported authored lights. Hidden authored
lights still count as authored lights, so hiding all lights can produce darkness.
On and Off let you explicitly override the preview lighting.

## Snapshot files and incremental updates

Use Blender's **Live Link: Save To File**, then **Open snapshot**. The file is one
little-endian size-prefixed `Blender.LiveLink.Update` FlatBuffer, exactly as the
exporter writes it; its extension is immaterial. `.blend`, GLB, and glTF are not
read directly. Uploads are limited to 128 MiB. Invalid files preserve the current
scene. File mode is isolated from incoming Blender edits; **Live Blender** returns
to the latest live state. Disconnecting retains the visible scene with a waiting
status; reconnecting clears stale live resources, preserving the debug camera.

`GET /api/scene?session=...&since=...` returns `kind: full`, `delta`, or `unchanged`:

- Full responses contain `objects`, `materials`, and image metadata. They are used
  initially, after session changes/reset, or when change history expires.
- Delta responses contain ordered `batches` with revisions, object/material/image
  upserts and `deleted` object IDs. Omitted mesh/light fields preserve prior data.
- All responses include current session/revision, connection/error status,
  `cameraReady`, and `initialCamera`. Camera readiness is separate from an empty
  initial connection, so the browser does not initialize prematurely.

History retains at most 128 batches / 64 MiB of serialized changes. Identical
resent meshes, materials, and images are deduplicated. The browser mutates
transforms and visibility in place and shares materials/textures by ID. Changed
meshes replace only their geometry; replaced/deleted/unreferenced GPU resources
are disposed. Full resynchronization rebuilds resources without moving the camera.

Image metadata contains dimensions, a content version, and a URL of the form
`/api/images/<session>/<id>/<version>`. That endpoint returns raw RGBA8 bytes,
with immutable caching for successful responses. Pixel arrays never enter scene
JSON. An expired live image revision returns 404; later image deltas restore the
latest version. Missing/failed images use scalar fallbacks; failed fetches retry
every two seconds. Snapshot image stores retain up to eight files / 256 MiB;
loaded browser textures remain independent of those stores.

## Materials and lights

Meshes use exported positions, normals, UVs, visibility, and world transforms.
Missing normals are computed; missing UVs sample `(0,0)`. Only the first material
slot is rendered because the protocol has no per-triangle material assignments.

PBR materials support base color, metallic, roughness, emission, and their image
references. Maps **replace** scalars. Metallic and roughness maps read red, as in
`game/data/shaders/geometry_common.h`. Exported RGBA8 pixels are linear, including
color/emission, and are not sRGB-decoded again. Blender's bottom-row-first pixels
map to UV v=0 without flipping. Textures repeat and use linear/mipmap filtering.
Positive emission renders unlit color × strength, matching the native emission
branch. Geometry remains opaque as in the native material pass.

Point and spot intensities use `power/(4π)` with inverse-square distance falloff.
Spot half-angle is half the exported beam angle; the cone is hard edged because
native `edge_blend` is currently unused. Sun colors use the native reference
solar spectrum and scene photometric scale, multiplied by `power/1361`.
Conversions are centralized in `lighting.js`; there is no per-scene calibration.

Point, spot, and sun lights honor visibility and shadow flags. Shadows use 1024²
PCF maps (six faces for point lights). Directional shadow coverage fits visible
scene bounds; geometry/light edits refresh the maps. Disabling shadows or deleting
lights disposes the maps. This is not native cascaded/EVRP shadow filtering.

## Remaining parity limits

Three.js's BRDF is still different from the native Cook–Torrance implementation.
The controlled head-on rough-gray direct-light fixture at EV 0 produces linear
`0.194108` versus the native shader reference `0.186530` (about +4.06%). This is a
BRDF difference, not a light-power conversion. The GPU comparison isolates direct
lighting against native equations, not the native application's final image.
The web renderer has no tone mapping or automatic exposure; bright values clip
at display output. Full native image parity is therefore not claimed.

GI, sky/atmosphere/clouds, area lights, bloom, native tone mapping/exposure, normal
maps, arbitrary Blender nodes, per-face material slots, animation/skinning,
physics, and gameplay remain future work.

## Validation

```sh
python3 -S -m unittest discover -s game_web/tests -v
bash -n build.sh game_web/build.sh
```

`-S` disables Python site packages. Tests cover protocol framing, real/synthetic
snapshots, initial-camera retention, bounded delta history, versioned image
endpoints, material/light decode, build routing, and lifecycle restart behavior.

With the bridge running:

```sh
blender --background --factory-startup --python game_web/tests/blender_parity_smoke.py
```

This exports textured geometry and three light types, sends a follow-up edit,
and writes `/tmp/game_web_parity_blender.bin`. Background Blender has no viewport,
so that test supplies a deterministic editor pose through the existing exporter.
The original `blender_smoke.py` / `blender_snapshot.bin` fixture remains supported.

`tests/browser_parity.js` is an optional Playwright tool function. It creates its
own tab, blocks external requests, loads `parity_snapshot.bin`, exercises pointer
capture/navigation, and checks resource lifetime. Float GPU readbacks verify UV
corners, linear bytes, scalar-map channels, point falloff, native direct-light
reference values, spot cones, sun conversion, and actual occlusion for all three
shadow types. Playwright is test tooling only, never a runtime dependency.

Regenerate the deterministic wire fixture with:

```sh
python3 -S -c "import sys; from pathlib import Path; sys.path.insert(0, 'game_web/tests'); from fixtures import textured_scene; Path('game_web/tests/parity_snapshot.bin').write_bytes(textured_scene())"
```

Vendored source: https://github.com/mrdoob/three.js/tree/r180. Required modules,
MIT license, and SHA-256 checksums are in `vendor/three/`; normal builds verify
these checksums and never fetch assets.
