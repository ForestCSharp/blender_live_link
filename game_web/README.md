# Web renderer

A static-geometry Three.js viewer for Blender Live Link. It uses checked-in
Three.js r180 modules and Python 3's standard library, with the repository's
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
available port and prints/opens its actual URL. Blender's TCP port stays 65432;
an unidentified owner of that port is reported rather than terminated.

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

## View and load

Drag to orbit, right-drag to pan, and scroll to zoom. **Frame scene** or F fits
visible geometry. The viewer uses Blender's Z-up coordinates, world transforms,
visibility, indexed positions/normals, and linear base colors. Missing normals
are computed; missing materials use a neutral fallback.

Use Blender's existing **Live Link: Save To File**, then **Open snapshot** in the
viewer. Files contain one little-endian size-prefixed `Blender.LiveLink.Update`
FlatBuffer, exactly as written by the existing exporter. File extension is not
significant. This does not read `.blend`, GLB, or glTF directly. Snapshot uploads
are limited to 128 MiB and are decoded in memory without writing to disk.
Invalid files retain the displayed scene and show an error.

File mode remains unchanged while Blender updates arrive. **Live Blender**
returns to the latest live scene. The bridge applies reset, deletion, transform,
visibility, material, and mesh updates and clears the old scene on reconnect.
A disconnected Blender leaves the last received scene visible with a waiting
status. An omitted mesh in an object update retains its previous geometry.

V1 uses simple neutral lighting and the **first material slot**, matching the
native renderer. The protocol contains material slots but no per-triangle slot
indices, so faithful multi-material face assignments cannot be reconstructed.
Textures, authored lights, animation, skinning, gameplay, and physics are not
rendered. Exported positions are displayed as static geometry. Incoming batches
are accumulated in Python; changed revisions return a complete JSON scene and
replace browser meshes, disposing their previous GPU resources. This favors
simplicity over large-scene streaming performance.

## Validation

```sh
python3 -S -m unittest discover -s game_web/tests -v
bash -n build.sh game_web/build.sh
```

`-S` disables Python site packages to verify no NumPy or installed FlatBuffers
package is required. Tests cover real/synthetic snapshots, TCP framing and
reconnects, scene lifecycle, HTTP isolation, stubbed root-build routing,
automatic relaunch, and HTTP-port fallback.

With the bridge running, generate and transmit a real Blender fixture:

```sh
blender --background --factory-startup --python game_web/tests/blender_smoke.py
```

This creates an asymmetric cube in a separate background process, writes
`/tmp/game_web_blender_snapshot.bin`, and transmits it with the existing
`LiveLinkTransport`. The checked-in `tests/blender_snapshot.bin` is this fixture
from Blender 5.1.2. `tests/browser_smoke.js` is a Playwright tool function for
checking offline assets, source switching, invalid files, resize, and resource
cleanup against a running bridge containing the fixture. Playwright is optional
verification tooling, never a build or runtime dependency.

Vendored source: https://github.com/mrdoob/three.js/tree/r180. Required modules,
MIT license, and SHA-256 checksums are in `vendor/three/`; normal builds verify
these checksums and never fetch assets.
