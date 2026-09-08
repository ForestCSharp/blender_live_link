"""Run with python3 -S -m unittest discover -s game_web/tests -v."""
import functools
import http.client
from pathlib import Path
import socket
import struct
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bridge import Scene, TCPServer, Receiver, Handler, ThreadingHTTPServer, decode, snapshot
from compiled_schemas.python import flatbuffers
from compiled_schemas.python.Blender.LiveLink import Update, Object, Mesh, Vec3, Quat, Material, Vec4


def frame(*, uid=7, mesh=True, visible=True, x=3, reset=False, deleted=(), empty=False, bad_index=False):
    b = flatbuffers.Builder(256)
    def vector(values, scalar):
        b.StartVector(4, len(values), 4)
        for value in reversed(values):
            getattr(b, 'Prepend' + scalar)(value)
        return b.EndVector()
    positions = vector([0, 0, 0, 2, 0, 0, 0, 1, 1], 'Float32')
    indices = vector([0, 1, 99 if bad_index else 2], 'Uint32')
    materials = vector([11], 'Int32')
    Mesh.Start(b)
    Mesh.AddPositions(b, positions)
    Mesh.AddIndices(b, indices)
    Mesh.AddMaterialIds(b, materials)
    geometry = Mesh.End(b)
    name = b.CreateString('Asymmetric triangle')
    Object.Start(b)
    Object.AddUniqueId(b, uid)
    Object.AddName(b, name)
    Object.AddVisibility(b, visible)
    Object.AddLocation(b, Vec3.CreateVec3(b, x, -2, 1))
    Object.AddScale(b, Vec3.CreateVec3(b, 1, 2, 1))
    Object.AddRotation(b, Quat.CreateQuat(b, 0, 0, 0, 1))
    if mesh:
        Object.AddMesh(b, geometry)
    obj = Object.End(b)
    objects = vector([] if empty else [obj], 'UOffsetTRelative')
    Material.Start(b)
    Material.AddUniqueId(b, 11)
    Material.AddBaseColor(b, Vec4.CreateVec4(b, 0.8, 0.2, 0.1, 1))
    material = Material.End(b)
    material_vector = vector([material], 'UOffsetTRelative')
    deletions = vector(deleted, 'Int32')
    Update.Start(b)
    Update.AddObjects(b, objects)
    Update.AddMaterials(b, material_vector)
    Update.AddDeletedObjectUids(b, deletions)
    Update.AddReset(b, reset)
    root = Update.End(b)
    b.FinishSizePrefixed(root)
    return bytes(b.Output())


def wait_for(predicate):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError('Timed out waiting for scene')


class BridgeTests(unittest.TestCase):
    def test_scene_lifecycle(self):
        s = Scene()
        s.apply(decode(frame()[4:]))
        mesh = s.objects[7]['mesh']
        self.assertEqual(mesh['materialIds'], [11])
        self.assertEqual(s.materials[11]['color'][3], 1)
        s.apply(decode(frame(mesh=False, visible=False, x=9)[4:]))
        self.assertIs(s.objects[7]['mesh'], mesh)
        self.assertFalse(s.objects[7]['visible'])
        self.assertEqual(s.objects[7]['position'], [9, -2, 1])
        s.apply(decode(frame()[4:]))
        self.assertIs(s.objects[7]['mesh'], mesh)
        changed = decode(frame()[4:])
        changed['objects'][0]['mesh']['positions'][0] = 0.5
        s.apply(changed)
        self.assertIsNot(s.objects[7]['mesh'], mesh)
        s.apply(decode(frame(deleted=[7], empty=True)[4:]))
        self.assertFalse(s.objects)
        s.apply(decode(frame()[4:]))
        s.apply(decode(frame(reset=True, empty=True)[4:]))
        self.assertFalse(s.objects)
        self.assertNotIn('objects', s.view(s.revision))

    def test_real_blender_snapshot(self):
        data = snapshot(Path(__file__).with_name('blender_snapshot.bin').read_bytes())
        self.assertEqual(len(data['objects']), 1)
        self.assertEqual(data['objects'][0]['position'], [2, -1, 0.5])
        self.assertEqual(len(data['objects'][0]['mesh']['indices']), 36)

    def test_snapshot_and_bad_payloads(self):
        self.assertEqual(snapshot(frame())['objects'][0]['id'], 7)
        for data in (b'', frame()[:-1], frame() + b'0', frame(bad_index=True), b'\0' * 16):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                snapshot(data)

    def test_tcp_frames_and_reconnect(self):
        server = TCPServer(('127.0.0.1', 0), Receiver)
        server.scene = Scene()
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with socket.create_connection(server.server_address) as peer:
                data = frame()
                peer.sendall(data[:2])
                peer.sendall(data[2:17])
                peer.sendall(data[17:] + frame(mesh=False, x=8))
                wait_for(lambda: server.scene.objects.get(7, {}).get('position') == [8, -2, 1])
                self.assertIn('mesh', server.scene.objects[7])
            wait_for(lambda: not server.scene.connected)
            with socket.create_connection(server.server_address) as peer:
                wait_for(lambda: server.scene.connected)
                self.assertFalse(server.scene.objects)
                peer.sendall(frame(uid=8))
                wait_for(lambda: 8 in server.scene.objects)
                peer.sendall(struct.pack('<I', 0xFFFFFFFF))
                wait_for(lambda: bool(server.scene.error))
                self.assertIn(8, server.scene.objects)
            with socket.create_connection(server.server_address) as peer:
                peer.sendall(frame()[:11])
            wait_for(lambda: server.scene.error == 'Truncated TCP frame')
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

    def test_http_snapshot_isolation_and_origin(self):
        server = ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(Path(__file__).parents[1])))
        server.scene = Scene()
        server.scene.apply(decode(frame(uid=99)[4:]))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            connection = http.client.HTTPConnection(*server.server_address)
            connection.request('POST', '/api/snapshot', frame())
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertIn(b'Asymmetric triangle', response.read())
            self.assertEqual(list(server.scene.objects), [99])
            connection.request('POST', '/api/snapshot', b'bad')
            response = connection.getresponse()
            self.assertEqual(response.status, 400)
            response.read()
            self.assertEqual(list(server.scene.objects), [99])
            session = server.scene.session
            revision = server.scene.revision
            connection.request('GET', f'/api/scene?since={revision}&session={session}')
            response = connection.getresponse()
            self.assertNotIn(b'"objects"', response.read())
            server.scene = Scene()
            server.scene.apply(decode(frame(uid=100)[4:]))
            connection.request('GET', f'/api/scene?since={revision}&session={session}')
            response = connection.getresponse()
            self.assertIn(b'"objects"', response.read())
            connection.request('GET', '/bridge.py')
            response = connection.getresponse()
            self.assertEqual(response.status, 404)
            response.read()
            connection.request('GET', '/api/scene', headers={'Origin': 'https://example.com'})
            response = connection.getresponse()
            self.assertEqual(response.status, 403)
            response.read()
            connection.close()
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main()
