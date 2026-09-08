import functools
import http.client
import json
from pathlib import Path
import threading
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bridge import decode, Scene, snapshot, Handler, ThreadingHTTPServer
from fixtures import encode, CAMERA, QUAD, PIXELS, textured_scene


class ParityTests(unittest.TestCase):
    def test_camera_initial_update_survives_reset_reconnect(self):
        scene = Scene()
        scene.connection(True)
        self.assertFalse(scene.view()['cameraReady'])
        scene.apply(decode(encode(camera=CAMERA)[4:]))
        initial = scene.view()['initialCamera']
        self.assertEqual(initial['position'], CAMERA['position'])
        scene.apply(decode(encode(camera={**CAMERA, 'position':[4,5,6]},reset=True)[4:]))
        scene.connection(False)
        scene.connection(True)
        self.assertEqual(scene.view()['initialCamera'], initial)
        scene.apply(decode(encode(camera=CAMERA)[4:]))
        self.assertEqual(scene.view()['initialCamera'], initial)

    def test_missing_and_invalid_camera_fallback(self):
        for camera in (None, {**CAMERA,'forward':[0,0,0]}, {**CAMERA,'up':CAMERA['forward']},
                       {**CAMERA,'position':[float('nan'),0,0]}):
            self.assertIsNone(snapshot(encode(camera=camera))['initialCamera'])
        scene = Scene()
        scene.apply(decode(encode()[4:]))
        scene.apply(decode(encode(camera=CAMERA)[4:]))
        self.assertIsNone(scene.view()['initialCamera'])
        vertical = snapshot(encode(camera={**CAMERA,'forward':[0,0,1],'up':[0,1,0]}))
        self.assertEqual(vertical['initialCamera']['up'], [0,1,0])

    def test_delta_order_and_history_recovery(self):
        scene = Scene()
        scene.HISTORY_BATCHES = 2
        scene.apply(decode(encode(objects=[{'id':1,'mesh':QUAD}])[4:]))
        first = scene.revision
        scene.apply(decode(encode(objects=[{'id':1,'position':[2,0,0]}])[4:]))
        scene.apply(decode(encode(objects=[{'id':1,'visible':False}])[4:]))
        delta = scene.view(first)
        self.assertEqual(delta['kind'], 'delta')
        self.assertEqual([b['revision'] for b in delta['batches']], [first+1, first+2])
        self.assertNotIn('mesh', delta['batches'][0]['objects'][0])
        scene.apply(decode(encode(deleted=[1])[4:]))
        self.assertEqual(scene.view(first)['kind'], 'full')
        self.assertEqual(scene.view()['objects'], [])
        scene.apply(decode(encode(reset=True)[4:]))
        self.assertEqual(scene.view(scene.revision-1)['kind'], 'full')
        scene.HISTORY_BYTES = 1
        scene.apply(decode(encode(objects=[{'id':1,'mesh':QUAD}])[4:]))
        self.assertFalse(scene.history)
        self.assertEqual(scene.view(scene.revision-1)['kind'], 'full')

    def test_uv_material_image_and_light_decode(self):
        batch = decode(textured_scene()[4:])
        self.assertEqual(batch['objects'][0]['mesh']['uvs'], QUAD['uvs'])
        self.assertEqual(batch['materials'][0]['colorImage'],21)
        self.assertEqual(batch['images'][0]['pixels'],PIXELS)
        self.assertEqual(batch['objects'][1]['light']['power'],100)
        for kind in range(3):
            batch = decode(encode(objects=[{'id':2,'light':{'type':kind,'power':1361,'angle':0.4,'shadows':False}}])[4:])
            self.assertEqual(batch['objects'][0]['light']['type'],kind)
            self.assertFalse(batch['objects'][0]['light']['shadows'])
            if kind == 1: self.assertAlmostEqual(batch['objects'][0]['light']['angle'],0.4)
        with self.assertRaises(ValueError):
            decode(encode(images=[{'id':21,'width':2,'height':2,'pixels':b'bad'}])[4:])
        with self.assertRaises(ValueError):
            decode(encode(objects=[{'id':1,'mesh':{**QUAD,'uvs':[0,1]}}])[4:])

    def test_versioned_image_endpoint_and_snapshot_isolation(self):
        server = ThreadingHTTPServer(('127.0.0.1',0),functools.partial(Handler,directory=str(Path(__file__).parents[1])))
        server.scene = Scene()
        server.scene.apply(decode(textured_scene()[4:]))
        thread = threading.Thread(target=server.serve_forever,daemon=True)
        thread.start()
        connection = http.client.HTTPConnection(*server.server_address,timeout=2)
        try:
            url = server.scene.view()['images'][0]['url']
            self.assertNotIn('pixels', server.scene.view()['images'][0])
            connection.request('GET',url)
            response = connection.getresponse()
            self.assertEqual(response.read(),PIXELS)
            replacement = encode(images=[{'id':21,'width':1,'height':1,'pixels':b'\xff'*4}])
            server.scene.apply(decode(replacement[4:]))
            connection.request('GET',url)
            response = connection.getresponse()
            self.assertEqual(response.status,404)
            response.read()
            connection.request('POST','/api/snapshot',textured_scene())
            response = connection.getresponse()
            data = json.loads(response.read())
            connection.request('GET',data['images'][0]['url'])
            response = connection.getresponse()
            self.assertEqual(response.read(),PIXELS)
            self.assertEqual(server.scene.view()['images'][0]['width'],1)
        finally:
            connection.close(); server.shutdown(); server.server_close(); thread.join()
