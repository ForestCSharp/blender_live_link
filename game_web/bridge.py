#!/usr/bin/env python3
"""Local Live Link TCP receiver and dependency-free HTTP scene bridge."""
import argparse
from collections import deque, OrderedDict
import functools
import errno
import secrets
import select
import hashlib
import json
import math
from pathlib import Path
import socketserver
import struct
import sys
import threading
import time
import uuid
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
import webbrowser

from lifecycle import bind_live_link, request_shutdown, remember, forget, resolve_port

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
try:
    from compiled_schemas.python.Blender.LiveLink.Update import Update
except ImportError as exc:
    raise SystemExit('Missing generated Python schemas. Run the project-root ./build.sh first.') from exc

from gameplay_decode import decode_gameplay

MAX_FRAME = 128 * 1024 * 1024
# Seconds to wait after the last viewer disconnects before stopping. Long
# enough to ride out a page reload, short enough that the Blender TCP port is
# free by the time you are back at the terminal.
VIEWER_GRACE_SECONDS = 5.0
# A page that has never been opened gets longer: the browser may still be
# starting, or the URL may be opened by hand after --no-browser.
STARTUP_GRACE_SECONDS = 600.0
# Server -> viewer keepalive. Writing is what surfaces a socket whose peer is
# gone; a silent connection would look alive forever.
VIEWER_PING_SECONDS = 5.0


def decode(payload):
    """Decode supported static scene fields before changing scene state."""
    if not 12 <= len(payload) <= MAX_FRAME:
        raise ValueError('Invalid FlatBuffer size')
    root = struct.unpack_from('<I', payload)[0]
    if not 4 <= root <= len(payload) - 4:
        raise ValueError('Invalid FlatBuffer root')

    def table_bounds(table):
        pos = table._tab.Pos
        if not 4 <= pos <= len(payload) - 4:
            raise ValueError('Invalid table offset')
        vpos = pos - struct.unpack_from('<i', payload, pos)[0]
        if not 0 <= vpos <= len(payload) - 4:
            raise ValueError('Invalid vtable offset')
        vsize, size = struct.unpack_from('<HH', payload, vpos)
        if vsize < 4 or vsize % 2 or vpos + vsize > len(payload) or size < 4 or pos + size > len(payload):
            raise ValueError('Invalid table bounds')

    def vector(table, name, width=4):
        count = getattr(table, name + 'Length')()
        if count > len(payload) // width:
            raise ValueError('Invalid vector length: ' + name)
        return [getattr(table, name)(i) for i in range(count)]

    def components(value, names):
        if value is None:
            raise ValueError('Missing transform or color')
        result = [getattr(value, n)() for n in names]
        if not all(math.isfinite(x) for x in result):
            raise ValueError('Non-finite transform or color')
        return result

    try:
        update = Update.GetRootAs(payload)
        table_bounds(update)
        objects = []
        for obj in vector(update, 'Objects'):
            table_bounds(obj)
            item = dict(id=obj.UniqueId(), name=(obj.Name() or b'').decode('utf-8', 'replace'),
                        visible=obj.Visibility(), position=components(obj.Location(), 'XYZ'),
                        scale=components(obj.Scale(), 'XYZ'), rotation=components(obj.Rotation(), 'XYZW'))
            rigid = obj.RigidBody()
            item['rigidBody'] = None
            if rigid is not None:
                mass = rigid.Mass()
                if not math.isfinite(mass):
                    raise ValueError('Invalid rigid-body mass')
                item['rigidBody'] = dict(isDynamic=rigid.IsDynamic(), mass=mass)
            mesh = obj.Mesh()
            if mesh is not None:
                table_bounds(mesh)
                positions = vector(mesh, 'Positions')
                normals = vector(mesh, 'Normals')
                indices = vector(mesh, 'Indices')
                uvs = vector(mesh, 'Texcoords')
                if (len(positions) % 3 or len(indices) % 3 or
                    (normals and len(normals) != len(positions)) or
                    (uvs and len(uvs) != len(positions) // 3 * 2) or
                    any(i >= len(positions) // 3 for i in indices) or
                    not all(math.isfinite(x) for x in positions + normals + uvs)):
                    raise ValueError('Invalid mesh vectors')
                item['mesh'] = dict(positions=positions, normals=normals, indices=indices, uvs=uvs,
                                    materialIds=vector(mesh, 'MaterialIds'))

            light = obj.Light()
            if light is not None:
                table_bounds(light)
                kind = light.Type()
                detail = light.PointLight() if kind == 0 else light.SpotLight() if kind == 1 else light.SunLight() if kind == 2 else None
                if detail is not None:
                    power = detail.Power()
                    if not math.isfinite(power):
                        raise ValueError('Invalid light power')
                    item['light'] = dict(type=kind, color=components(light.Color(), 'XYZ'),
                                         power=max(0, power), shadows=light.UseShadow())
                    if kind == 1:
                        angle, blend = detail.BeamAngle(), detail.EdgeBlend()
                        if not math.isfinite(angle) or not math.isfinite(blend):
                            raise ValueError('Invalid spotlight cone')
                        item['light'].update(angle=angle / 2, edgeBlend=blend)
                    if kind == 2:
                        item['light']['shadows'] = bool(light.UseShadow() and detail.CastShadows())
                else:
                    item['light'] = None
            decode_gameplay(obj, item, vector, table_bounds)
            objects.append(item)
        materials = []
        for m in vector(update, 'Materials'):
            table_bounds(m)
            metallic, roughness, strength = m.Metallic(), m.Roughness(), m.EmissionStrength()
            if not all(math.isfinite(v) for v in (metallic, roughness, strength)):
                raise ValueError('Invalid material scalar')
            materials.append(dict(id=m.UniqueId(), color=components(m.BaseColor(), 'XYZW'),
                                  metallic=metallic, roughness=roughness, emissionStrength=max(0, strength),
                                  emission=components(m.EmissionColor(), 'XYZW') if m.EmissionColor() else [0,0,0,1],
                                  colorImage=m.BaseColorImageId(), metallicImage=m.MetallicImageId(),
                                  roughnessImage=m.RoughnessImageId(), emissionImage=m.EmissionColorImageId()))
        images = []
        for image in vector(update, 'Images'):
            table_bounds(image)
            width, height = image.Width(), image.Height()
            if width <= 0 or height <= 0 or width * height * 4 != image.DataLength():
                raise ValueError('Invalid image dimensions or RGBA length')
            # A direct byte-vector slice avoids allocating one Python int per pixel.
            offset = image._tab.Offset(10)
            start = image._tab.Vector(offset)
            end = start + image.DataLength()
            if not offset or start < 0 or end > len(payload):
                raise ValueError('Truncated image bytes')
            pixels = bytes(payload[start:end])
            version = hashlib.sha256(struct.pack('<II', width, height) + pixels).hexdigest()
            images.append(dict(id=image.UniqueId(), width=width, height=height, version=version, pixels=pixels))
        camera = None
        editor = update.EditorCamera()
        if editor is not None:
            try:
                table_bounds(editor)
                position = components(editor.Location(), 'XYZ')
                forward = components(editor.Forward(), 'XYZ')
                up = components(editor.Up(), 'XYZ')
                fl = math.sqrt(sum(x*x for x in forward))
                ul = math.sqrt(sum(x*x for x in up))
                if fl > 1e-6 and ul > 1e-6:
                    forward = [x/fl for x in forward]
                    up = [x/ul for x in up]
                    if abs(sum(a*b for a, b in zip(forward, up))) < 0.999:
                        camera = dict(position=position, forward=forward, up=up)
            except (ValueError, struct.error, TypeError, IndexError):
                pass  # Invalid optional camera falls back without rejecting geometry.
        return dict(reset=update.Reset(), objects=objects, materials=materials,
                    camera=camera, images=images, deleted=vector(update, 'DeletedObjectUids'))
    except (IndexError, TypeError, struct.error, OverflowError) as exc:
        raise ValueError('Malformed FlatBuffer') from exc


def snapshot_scene(data):
    if len(data) < 4 or struct.unpack_from('<I', data)[0] != len(data) - 4:
        raise ValueError('Expected one size-prefixed Live Link snapshot')
    state = Scene()
    state.apply(decode(data[4:]))
    return state


def snapshot(data):
    return snapshot_scene(data).view()


class Scene:
    HISTORY_BATCHES = 128
    HISTORY_BYTES = 64 * 1024 * 1024

    def __init__(self):
        self.objects = {}
        self.materials = {}
        self.images = {}
        self.camera_ready = False
        self.initial_camera = None
        self.revision = 0
        self.active_camera_control_id = None
        self.generation = 0
        self.session = uuid.uuid4().hex
        self.connected = False
        self.error = ''
        self.lock = threading.RLock()
        self.history = deque()
        self.history_bytes = 0
        self.full_floor = 0

    def clear(self):
        self.active_camera_control_id = None
        self.generation += 1
        self.objects.clear()
        self.materials.clear()
        self.images.clear()
        self.history.clear()
        self.history_bytes = 0
        self.full_floor = self.revision

    def connection(self, connected, error=''):
        with self.lock:
            if connected:
                self.revision += 1
                self.clear()
            self.connected, self.error = connected, error

    def image_metadata(self, image):
        return {k: v for k, v in image.items() if k != 'pixels'} | {
            'url': f"/api/images/{self.session}/{image['id']}/{image['version']}"}

    def image_bytes(self, uid, version):
        with self.lock:
            image = self.images.get(uid)
            return image['pixels'] if image and image['version'] == version else None

    def apply(self, batch):
        with self.lock:
            if not self.camera_ready:
                self.camera_ready = True
                self.initial_camera = batch.get('camera')
            self.revision += 1
            if batch['reset']:
                self.clear()
            for uid in batch['deleted']:
                self.objects.pop(uid, None)
                if self.active_camera_control_id == uid:
                    self.active_camera_control_id = None
            changed_objects = []
            for item in batch['objects']:
                previous = self.objects.get(item['id'], {})
                if item.get('cameraControl'):
                    self.active_camera_control_id = item['id']
                elif item.get('cameraControl', False) is None and self.active_camera_control_id == item['id']:
                    self.active_camera_control_id = None
                # Exporters sometimes resend identical meshes; don't upload them again.
                change = dict(item)
                for field in ('mesh', 'light', 'armature', 'character', 'cameraControl', 'part', 'attachment'):
                    if field in change and change[field] == previous.get(field):
                        del change[field]
                self.objects[item['id']] = {**previous, **change}
                changed_objects.append(change)
            changed_materials = []
            for material in batch['materials']:
                if self.materials.get(material['id']) != material:
                    self.materials[material['id']] = material
                    changed_materials.append(material)
            changed_images = []
            for image in batch.get('images', []):
                if self.images.get(image['id'], {}).get('version') != image['version']:
                    self.images[image['id']] = image
                    changed_images.append(self.image_metadata(image))
            change = dict(revision=self.revision, objects=changed_objects, materials=changed_materials,
                          images=changed_images, deleted=batch['deleted'])
            size = len(json.dumps(change, separators=(',', ':')))
            self.history.append((change, size))
            self.history_bytes += size
            while len(self.history) > self.HISTORY_BATCHES or self.history_bytes > self.HISTORY_BYTES:
                expired, size = self.history.popleft()
                self.history_bytes -= size
                self.full_floor = max(self.full_floor, expired['revision'])
            self.error = ''

    def view(self, since=-1):
        with self.lock:
            result = dict(revision=self.revision, generation=self.generation, session=self.session, connected=self.connected, error=self.error,
                          cameraReady=self.camera_ready, initialCamera=self.initial_camera, activeCameraControlId=self.active_camera_control_id)
            if since == self.revision:
                return result | {'kind': 'unchanged'}
            if self.full_floor <= since < self.revision:
                return result | {'kind': 'delta', 'batches': [batch for batch, _ in self.history if batch['revision'] > since]}
            return result | dict(kind='full', objects=list(self.objects.values()), materials=list(self.materials.values()),
                                 images=[self.image_metadata(i) for i in self.images.values()])


# Snapshot images remain available during browser texture fetches. Bound retained
# files independently of the live scene; each browser keeps its loaded GPU copies.
SNAPSHOTS = OrderedDict()
SNAPSHOT_LOCK = threading.RLock()


def retain_snapshot(scene):
    with SNAPSHOT_LOCK:
        size = sum(len(i['pixels']) for i in scene.images.values())
        assets = Scene()
        assets.session = scene.session
        assets.images = scene.images
        SNAPSHOTS[scene.session] = (assets, size)
        while len(SNAPSHOTS) > 8 or sum(s for _, s in SNAPSHOTS.values()) > 256 * 1024 * 1024:
            SNAPSHOTS.popitem(last=False)


class Receiver(socketserver.BaseRequestHandler):
    def handle(self):
        scene = self.server.scene
        scene.connection(True)
        error = ''
        try:
            while True:
                prefix = self.read_exact(4, allow_eof=True)
                if prefix is None:
                    break
                size = struct.unpack('<I', prefix)[0]
                if not 12 <= size <= MAX_FRAME:
                    raise ValueError('Invalid TCP frame length')
                scene.apply(decode(self.read_exact(size)))
        except (ValueError, OSError) as exc:
            error = str(exc)
            print('Live Link:', error, flush=True)
        finally:
            scene.connection(False, error)

    def read_exact(self, size, allow_eof=False):
        data = bytearray()
        while len(data) < size:
            part = self.request.recv(min(size - len(data), 1024 * 1024))
            if not part:
                if not data and allow_eof:
                    return None
                raise ValueError('Truncated TCP frame')
            data.extend(part)
        return data


class TCPServer(socketserver.TCPServer):
    allow_reuse_address = True


class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm"}

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError):
            pass  # Browser navigation may cancel an in-flight response.

    def log_message(self, *_args):
        pass

    def reply(self, value, status=200):
        data = json.dumps(value, allow_nan=False, separators=(',', ':')).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def allowed(self):
        expected = '127.0.0.1:' + str(self.server.server_port)
        return (self.headers.get('Host') == expected and
                self.headers.get('Origin', 'http://' + expected) == 'http://' + expected)

    def stream_alive(self):
        """Hold one connection open for as long as this page is watching.

        Tab closed, browser quit, laptop shut: the socket dies with it, and that
        is what releases the Blender TCP port. A polled heartbeat cannot do this
        job -- browsers throttle timers in background tabs to roughly once a
        minute, so a backgrounded tab and a closed one look identical.
        """
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.server.viewers.arrived()
        try:
            while True:
                # EOF is what a closed tab looks like, and select reports it at
                # once. Writing is not enough on its own: a write to a
                # half-closed loopback socket can succeed indefinitely. The
                # viewer never sends on this connection, so any readability
                # here means it is finished with it.
                readable, _, _ = select.select([self.connection], [], [], VIEWER_PING_SECONDS)
                if readable:
                    break
                self.wfile.write(b': keepalive\n\n')
                self.wfile.flush()
        except (OSError, ValueError):
            pass  # The viewer is gone; that is the signal, not an error.
        finally:
            self.server.viewers.left()

    def do_GET(self):
        if not self.allowed():
            return self.reply({'error': 'Local origin required'}, 403)
        url = urlsplit(self.path)
        if url.path == '/api/alive':
            return self.stream_alive()
        if url.path == '/api/scene':
            try:
                query = parse_qs(url.query)
                since = int(query.get('since', ['-1'])[0])
                if query.get('session', [''])[0] != self.server.scene.session:
                    since = -1
            except ValueError:
                return self.reply({'error': 'Invalid revision'}, 400)
            return self.reply(self.server.scene.view(since))
        if url.path.startswith('/api/images/'):
            parts = url.path.split('/')
            if len(parts) != 6:
                return self.reply({'error': 'Not found'}, 404)
            _, _, _, session, uid, version = parts
            try:
                uid = int(uid)
            except ValueError:
                return self.reply({'error': 'Invalid image ID'}, 400)
            with SNAPSHOT_LOCK:
                state = self.server.scene if session == self.server.scene.session else SNAPSHOTS.get(session, (None, 0))[0]
                pixels = state.image_bytes(uid, version) if state else None
            if pixels is None:
                return self.reply({'error': 'Image revision expired'}, 404)
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(pixels)))
            self.send_header('Cache-Control', 'private, max-age=31536000, immutable')
            self.end_headers()
            self.wfile.write(pixels)
            return
        # Serve only the public viewer assets, never repository or Python files.
        if url.path not in PUBLIC_FILES:
            return self.reply({'error': 'Not found'}, 404)
        return super().do_GET()

    def do_POST(self):
        if not self.allowed():
            return self.reply({'error': 'Local origin required'}, 403)
        if self.path == '/api/shutdown':
            token = self.headers.get('X-Renderer-Token', '')
            expected = getattr(self.server, 'shutdown_token', '')
            if not expected or not secrets.compare_digest(token, expected):
                return self.reply({'error': 'Invalid renderer token'}, 403)
            self.reply({'stopping': True})
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return
        if self.path != '/api/snapshot':
            return self.reply({'error': 'Not found'}, 404)
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 16 <= length <= MAX_FRAME + 4:
                raise ValueError('Invalid snapshot size (maximum 128 MiB)')
            self.connection.settimeout(30)
            data = self.rfile.read(length)
            if len(data) != length:
                raise ValueError('Truncated snapshot')
            state = snapshot_scene(data)
            retain_snapshot(state)
            self.reply(state.view())
        except (ValueError, OSError) as exc:
            self.reply({'error': str(exc)}, 400)


PUBLIC_FILES = {'/', '/index.html', '/app.js', '/camera.js', '/resources.js', '/lighting.js', '/style.css',
                '/vendor/three/three.module.js', '/vendor/three/three.core.js',
                '/vendor/three/OrbitControls.js', '/physics.js', '/gameplay.js', '/animation.js', '/character_controls.js', '/gameplay_ui.js',
                '/vendor/jolt/jolt-physics.wasm.js', '/vendor/jolt/jolt-physics.wasm.wasm'}


def validate():
    for name in PUBLIC_FILES - {'/'}:
        if not (HERE / name.lstrip('/')).is_file():
            raise SystemExit('Missing web asset: ' + name)
    for vendor in ('three', 'jolt'):
        for line in (HERE / 'vendor' / vendor / 'SHA256SUMS').read_text().splitlines():
            digest, name = line.split()
            if hashlib.sha256((HERE / 'vendor' / vendor / name).read_bytes()).hexdigest() != digest:
                raise SystemExit('Vendored asset checksum mismatch: ' + name)


class Viewers:
    """Open viewer connections. Zero of them means nobody is watching."""

    def __init__(self):
        self.lock = threading.Lock()
        self.count = 0
        self.ever_connected = False
        self.empty_since = time.monotonic()

    def arrived(self):
        with self.lock:
            self.count += 1
            self.ever_connected = True

    def left(self):
        with self.lock:
            self.count = max(0, self.count - 1)
            if self.count == 0:
                self.empty_since = time.monotonic()

    def idle_seconds(self):
        with self.lock:
            return 0.0 if self.count else time.monotonic() - self.empty_since


def watch_for_closed_viewers(http, grace, startup_grace=STARTUP_GRACE_SECONDS, interval=1.0):
    """Stop the renderer once the last page has gone.

    The bridge outlives its browser tab: closing the page used to leave this
    process holding the Blender TCP port, so the next native game run bound
    nothing and silently received no updates.
    """
    while True:
        time.sleep(interval)
        idle = http.viewers.idle_seconds()
        if idle >= (grace if http.viewers.ever_connected else startup_grace):
            print('No page is watching; stopping the web renderer and releasing '
                  'the Blender TCP port.', flush=True)
            threading.Thread(target=http.shutdown, daemon=True).start()
            return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--no-browser', action='store_true')
    parser.add_argument('--idle-timeout', type=float, default=VIEWER_GRACE_SECONDS,
                        metavar='SECONDS',
                        help='stop this long after the last page closes (0 to run until '
                             f'Ctrl+C; default {VIEWER_GRACE_SECONDS:.0f})')
    args = parser.parse_args()
    validate()
    try:
        port = resolve_port()
    except ValueError as exc:
        raise SystemExit(str(exc))
    if args.check:
        print('Web assets and generated Python bindings ready (Three.js r180).')
        return
    if request_shutdown(HERE):
        print('Replacing previous web renderer.', flush=True)
    scene = Scene()
    tcp = None
    http = None
    token = secrets.token_hex(32)
    try:
        tcp = bind_live_link(functools.partial(TCPServer, RequestHandlerClass=Receiver), HERE, port)
        handler = functools.partial(Handler, directory=str(HERE))
        try:
            http = ThreadingHTTPServer(('127.0.0.1', 8000), handler)
        except OSError as exc:
            if exc.errno != errno.EADDRINUSE:
                raise
            http = ThreadingHTTPServer(('127.0.0.1', 0), handler)
            print(f'HTTP port 8000 is busy; using {http.server_port}.', flush=True)
        tcp.scene = http.scene = scene
        http.shutdown_token = token
        http.viewers = Viewers()
        if args.idle_timeout > 0:
            threading.Thread(target=watch_for_closed_viewers,
                             args=(http, args.idle_timeout), daemon=True).start()
        remember(HERE, http.server_port, token)
        threading.Thread(target=tcp.serve_forever, daemon=True).start()
        url = f'http://127.0.0.1:{http.server_port}'
        stopping = ('stops when the page closes' if args.idle_timeout > 0
                    else 'runs until Ctrl+C')
        print(f'Web renderer: {url} (Blender TCP: {port}); {stopping}.', flush=True)
        if not args.no_browser:
            webbrowser.open(url)
        http.serve_forever()
    except KeyboardInterrupt:
        pass
    except OSError as exc:
        raise SystemExit(f'Cannot start web renderer: {exc}')
    finally:
        forget(HERE, token)
        if http is not None:
            http.server_close()
        # The receiver thread is daemonized: idle Blender must not block exit.
        if tcp is not None:
            tcp.server_close()


if __name__ == '__main__':
    main()
