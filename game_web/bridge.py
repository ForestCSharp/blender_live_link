#!/usr/bin/env python3
"""Local Live Link TCP receiver and dependency-free HTTP scene bridge."""
import argparse
import functools
import hashlib
import json
import math
from pathlib import Path
import socketserver
import struct
import sys
import threading
import uuid
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit
import webbrowser

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
try:
    from compiled_schemas.python.Blender.LiveLink.Update import Update
except ImportError as exc:
    raise SystemExit('Missing generated Python schemas. Run the project-root ./build.sh first.') from exc

MAX_FRAME = 128 * 1024 * 1024


def decode(payload):
    """Decode only V1 fields; reject invalid batches before changing scene state."""
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
            mesh = obj.Mesh()
            if mesh is not None:
                table_bounds(mesh)
                positions = vector(mesh, 'Positions')
                normals = vector(mesh, 'Normals')
                indices = vector(mesh, 'Indices')
                if (len(positions) % 3 or len(indices) % 3 or
                    (normals and len(normals) != len(positions)) or
                    any(i >= len(positions) // 3 for i in indices) or
                    not all(math.isfinite(x) for x in positions + normals)):
                    raise ValueError('Invalid mesh vectors')
                item['mesh'] = dict(positions=positions, normals=normals, indices=indices,
                                    materialIds=vector(mesh, 'MaterialIds'))

            objects.append(item)
        materials = []
        for m in vector(update, 'Materials'):
            table_bounds(m)
            materials.append(dict(id=m.UniqueId(), color=components(m.BaseColor(), 'XYZW')))
        return dict(reset=update.Reset(), objects=objects, materials=materials,
                    deleted=vector(update, 'DeletedObjectUids'))
    except (IndexError, TypeError, struct.error, OverflowError) as exc:
        raise ValueError('Malformed FlatBuffer') from exc


def snapshot(data):
    if len(data) < 4 or struct.unpack_from('<I', data)[0] != len(data) - 4:
        raise ValueError('Expected one size-prefixed Live Link snapshot')
    state = Scene()
    state.apply(decode(data[4:]))
    return state.view()


class Scene:
    def __init__(self):
        self.objects = {}
        self.materials = {}
        self.revision = 0
        self.session = uuid.uuid4().hex
        self.connected = False
        self.error = ''
        self.lock = threading.RLock()

    def connection(self, connected, error=''):
        with self.lock:
            if connected:
                self.objects.clear()
                self.materials.clear()
                self.revision += 1
            self.connected, self.error = connected, error

    def apply(self, batch):
        with self.lock:
            if batch['reset']:
                self.objects.clear()
                self.materials.clear()
            for uid in batch['deleted']:
                self.objects.pop(uid, None)
            for item in batch['objects']:
                # Absent mesh means a transform/visibility update, not removal.
                self.objects[item['id']] = {**self.objects.get(item['id'], {}), **item}
            for material in batch['materials']:
                self.materials[material['id']] = material
            self.revision += 1
            self.error = ''

    def view(self, since=-1):
        with self.lock:
            result = dict(revision=self.revision, session=self.session, connected=self.connected, error=self.error)
            if since != self.revision:
                result.update(objects=list(self.objects.values()), materials=list(self.materials.values()))
            return result


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

    def do_GET(self):
        if not self.allowed():
            return self.reply({'error': 'Local origin required'}, 403)
        url = urlsplit(self.path)
        if url.path == '/api/scene':
            try:
                query = parse_qs(url.query)
                since = int(query.get('since', ['-1'])[0])
                if query.get('session', [''])[0] != self.server.scene.session:
                    since = -1
            except ValueError:
                return self.reply({'error': 'Invalid revision'}, 400)
            return self.reply(self.server.scene.view(since))
        # Serve only the public viewer assets, never repository or Python files.
        if url.path not in PUBLIC_FILES:
            return self.reply({'error': 'Not found'}, 404)
        return super().do_GET()

    def do_POST(self):
        if not self.allowed():
            return self.reply({'error': 'Local origin required'}, 403)
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
            self.reply(snapshot(data))
        except (ValueError, OSError) as exc:
            self.reply({'error': str(exc)}, 400)


PUBLIC_FILES = {'/', '/index.html', '/app.js', '/style.css',
                '/vendor/three/three.module.js', '/vendor/three/three.core.js',
                '/vendor/three/OrbitControls.js'}


def validate():
    for name in PUBLIC_FILES - {'/'}:
        if not (HERE / name.lstrip('/')).is_file():
            raise SystemExit('Missing web asset: ' + name)
    for line in (HERE / 'vendor/three/SHA256SUMS').read_text().splitlines():
        digest, name = line.split()
        if hashlib.sha256((HERE / 'vendor/three' / name).read_bytes()).hexdigest() != digest:
            raise SystemExit('Vendored asset checksum mismatch: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--no-browser', action='store_true')
    args = parser.parse_args()
    validate()
    if args.check:
        print('Web assets and generated Python bindings ready (Three.js r180).')
        return
    scene = Scene()
    try:
        tcp = TCPServer(('127.0.0.1', 65432), Receiver)
        http = ThreadingHTTPServer(('127.0.0.1', 8000), functools.partial(Handler, directory=str(HERE)))
    except OSError as exc:
        raise SystemExit(f'Cannot start web renderer: {exc}. Stop any native game or process using ports 65432/8000.')
    tcp.scene = http.scene = scene
    threading.Thread(target=tcp.serve_forever, daemon=True).start()
    print('Web renderer: http://127.0.0.1:8000 (Blender TCP: 65432). Ctrl+C to stop.', flush=True)
    if not args.no_browser:
        webbrowser.open('http://127.0.0.1:8000')
    try:
        http.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        http.server_close()
        # Receiver thread is daemonized: an idle Blender connection must not block exit.
        tcp.server_close()


if __name__ == '__main__':
    main()
