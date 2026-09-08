"""Startup conflicts and managed relaunch, without touching actual renderer ports."""
import errno
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from lifecycle import is_project_renderer, state_path, bind_live_link, resolve_port


class LifecycleTests(unittest.TestCase):
    def test_only_recognizes_this_projects_renderers(self):
        root = Path('/tmp/project/game_web')
        self.assertTrue(is_project_renderer('python3 -S -u /tmp/project/game_web/bridge.py', None, root))
        self.assertTrue(is_project_renderer('python3 bridge.py', str(root), root))
        self.assertTrue(is_project_renderer('./game', '/tmp/project/game/bin', root))
        self.assertFalse(is_project_renderer('python3 /tmp/other/game_web/bridge.py', None, root))
        self.assertFalse(is_project_renderer('python3 -c "/tmp/project/game_web/bridge.py"', None, root))
        self.assertFalse(is_project_renderer('/tmp/other/game/bin/game', None, root))
        self.assertFalse(is_project_renderer('node /tmp/project/game_web/bridge.py', None, root))

    def test_unknown_tcp_owner_is_not_killed(self):
        root = Path('/tmp/project/game_web')
        def occupied(_address):
            raise OSError(errno.EADDRINUSE, 'busy')
        # Pin the port: a developer with BLENDER_LIVE_LINK_PORT exported must
        # not change what this test asserts.
        with patch('lifecycle.stop_legacy_listener', return_value=False) as stop, \
             patch.dict('os.environ', {'BLENDER_LIVE_LINK_PORT': '65432'}), \
             patch('lifecycle.time.monotonic', side_effect=[0, 6]):
            with self.assertRaisesRegex(OSError, 'could not be identified'):
                bind_live_link(occupied, root)
            stop.assert_called_once_with(root, 65432)

    def test_resolve_port_reads_environment(self):
        self.assertEqual(resolve_port({}), 65432)
        self.assertEqual(resolve_port({'BLENDER_LIVE_LINK_PORT': ''}), 65432)
        self.assertEqual(resolve_port({'BLENDER_LIVE_LINK_PORT': ' 65433 '}), 65433)
        for bad in ('nope', '0', '65536', '-1', '65432.0'):
            with self.assertRaisesRegex(ValueError, 'BLENDER_LIVE_LINK_PORT'):
                resolve_port({'BLENDER_LIVE_LINK_PORT': bad})

    def test_http_fallback_and_repeat_launch(self):
        web_root = str(Path(__file__).resolve().parents[1])
        children = []
        with tempfile.TemporaryDirectory() as directory, socket.socket() as http_owner:
            root = Path(directory)
            http_owner.bind(('127.0.0.1', 0))
            http_owner.listen()
            busy_port = http_owner.getsockname()[1]
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                tcp_port = str(reservation.getsockname()[1])
            script = f'''
import os
import sys
sys.path.insert(0, {web_root!r})
# Drive the real port plumbing rather than stubbing the bind: this is the
# same channel Blender and the native game read.
os.environ['BLENDER_LIVE_LINK_PORT'] = {tcp_port!r}
import bridge
from pathlib import Path
bridge.HERE = Path({directory!r})
bridge.validate = lambda: None
original_http = bridge.ThreadingHTTPServer
bridge.ThreadingHTTPServer = lambda address, handler: original_http(('127.0.0.1', {busy_port} if address[1] == 8000 else address[1]), handler)
sys.argv = ['bridge.py', '--no-browser']
bridge.main()
'''
            def start(previous_token=None):
                process = subprocess.Popen([sys.executable, '-S', '-u', '-c', script],
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                children.append(process)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        self.fail(process.stdout.read())
                    try:
                        state = json.loads(state_path(root).read_text())
                        if state['token'] != previous_token:
                            return process, state
                    except (OSError, ValueError):
                        pass
                    time.sleep(0.02)
                self.fail('Renderer did not start')
            try:
                first, state = start()
                self.assertNotEqual(state['port'], busy_port)
                connection = http.client.HTTPConnection('127.0.0.1', state['port'], timeout=2)
                connection.request('POST', '/api/shutdown', body=b'', headers={'X-Renderer-Token': 'wrong'})
                response = connection.getresponse()
                self.assertEqual(response.status, 403)
                response.read()
                connection.close()
                second, replacement = start(state['token'])
                self.assertEqual(first.wait(timeout=5), 0)
                self.assertIn('HTTP port 8000 is busy', first.stdout.read())
                connection = http.client.HTTPConnection('127.0.0.1', replacement['port'], timeout=2)
                connection.request('GET', '/api/scene')
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                self.assertIn('objects', json.loads(response.read()))
                connection.request('POST', '/api/shutdown', body=b'',
                                   headers={'X-Renderer-Token': replacement['token']})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                response.read()
                connection.close()
                self.assertEqual(second.wait(timeout=5), 0)
                self.assertFalse(state_path(root).exists())
                # The unrelated HTTP listener is still ours and open.
                self.assertEqual(http_owner.getsockname()[1], busy_port)
            finally:
                for child in children:
                    if child.poll() is None:
                        child.terminate()
                    child.wait(timeout=5)
                    child.stdout.close()
                state_path(root).unlink(missing_ok=True)
