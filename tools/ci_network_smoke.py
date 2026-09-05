"""Blender lifecycle tests; run with --background --factory-startup --python."""
import importlib
from pathlib import Path
import sys
import socket
import time
from unittest.mock import patch

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_live_link_transport import Clock, FakeSocket, select_fake

package = importlib.import_module(Path(__file__).resolve().parents[1].name)
extension = importlib.import_module(package.__name__ + '.extension_main')


def run():
    extension.register()
    clock = Clock()
    sockets = []
    def factory(*args):
        sock = FakeSocket()
        sockets.append(sock)
        return sock
    connection = extension.live_link_connection
    connection.transport = extension.LiveLinkTransport(
        socket_factory=factory, clock=clock, select_fn=select_fake)
    exports = []
    completions = []

    def serialize_scene(dirty, reason, force_full=False):
        exports.append((dirty, force_full))
        def complete():
            completions.append(True)
            connection.export_snapshot = {'committed': True}
        return connection.send(b'scene', on_complete=complete)

    try:
        # Real exporter entry points must short-circuit before touching bpy.
        with patch.object(connection, 'export_evaluation_context', side_effect=AssertionError('offline export')):
            assert not connection.send_scene_changes(set(), 'offline', True)
            assert not connection.send_object_list([], [])
            assert not connection.send_reset()

        with patch.object(connection, 'send_scene_changes', side_effect=serialize_scene), \
             patch.object(connection, 'make_update', return_value=b'reset') as reset_export, \
             patch.object(extension.time, 'monotonic', clock):
            for _ in range(10):
                extension.batched_dirty_ids.add(1)
                extension.schedule_send()
                extension.live_link_timer()
            assert not exports and not reset_export.called
            assert not extension.batched_dirty_ids
            sockets[-1].writable = True
            extension.live_link_timer()  # connection -> reset queued
            assert connection.transport.payload is not None and not exports
            extension.live_link_timer()  # reset complete -> full queued
            assert exports == [(set(), True)] and not completions
            sockets[-1].block = True
            extension.batched_dirty_ids.add(7)
            extension.schedule_send()
            clock.now = 0.5
            extension.live_link_timer()
            assert len(exports) == 1 and not completions
            assert extension.batched_dirty_ids == {7}
            sockets[-1].block = False
            extension.live_link_timer()  # full completes; accumulated edit queued
            assert len(completions) == 1
            assert exports[-1] == ({7}, False)
            extension.send_full_scene_update()  # preserve manual request in flight
            extension.live_link_timer()
            assert exports[-1] == (set(), True)
            assert len(exports) == 3
            # Reset must wait behind current payload.
            assert connection.send_reset()
            extension.live_link_timer()
            assert bytes(connection.transport.payload) == b'reset'
            extension.live_link_timer()
            assert connection.export_snapshot == {}
            # Peer loss clears snapshots and causes reset/full on reconnect.
            sockets[-1].readable = sockets[-1].eof = True
            extension.live_link_timer()
            assert connection.needs_full_sync and not connection.export_snapshot
            assert not connection.send_reset()
            clock.now += 1
            extension.live_link_timer()
            sockets[-1].writable = True
            extension.live_link_timer()
            extension.live_link_timer()
            assert exports[-1] == (set(), True)
            # File load must discard old-scene bytes and unregister-safe state.
            extension.automatic_initial_full_update_load_post(None)
            assert connection.transport.payload is None
            assert connection.needs_full_sync
            assert bpy.app.timers.is_registered(extension.live_link_timer)
            extension.live_link_timer()
            bpy.ops.live_link.reset_connection()
            assert extension.live_link_connection is connection
            assert connection.transport.socket is None
    finally:
        extension.unregister()
    assert not bpy.app.timers.is_registered(extension.live_link_timer)
    assert connection.transport.socket is None
    assert extension.automatic_initial_full_update_load_post not in bpy.app.handlers.load_post
    extension.register()
    assert bpy.app.timers.is_registered(extension.live_link_timer)
    extension.unregister()
    print('BLENDER_LIVE_LINK_NETWORK_OK')


def real_socket_checks():
    extension.register()
    connection = extension.live_link_connection
    longest_tick = 0.0
    try:
        # Reserve a non-listening port: real refused connections, with edits.
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1', 0))
            connection.transport.address = reserved.getsockname()
            with patch.object(connection, 'send_scene_changes', side_effect=AssertionError('offline export')):
                deadline = time.monotonic() + 1.2
                while time.monotonic() < deadline:
                    extension.batched_dirty_ids.add(1)
                    extension.schedule_send()
                    started = time.monotonic()
                    extension.live_link_timer()
                    longest_tick = max(longest_tick, time.monotonic() - started)
                    time.sleep(0.01)
        connection.close_socket()
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listener.settimeout(1)
            connection.transport.address = listener.getsockname()
            connection.transport.poll()
            with listener.accept()[0] as peer:
                deadline = time.monotonic() + 1
                while not connection.transport.ready and time.monotonic() < deadline:
                    connection.transport.poll()
                assert connection.transport.ready
                connection.transport_generation = connection.transport.generation
                connection.needs_full_sync = False
                connection.transport.socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
                peer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                assert connection.send(b'x' * (16 * 1024 * 1024))
                # Deliberately never read from the peer; Blender must keep
                # returning from its callback until the stall deadline expires.
                with patch.object(connection, 'send_scene_changes', side_effect=AssertionError('busy export')):
                    deadline = time.monotonic() + 8
                    while connection.is_connected() and time.monotonic() < deadline:
                        started = time.monotonic()
                        extension.live_link_timer()
                        longest_tick = max(longest_tick, time.monotonic() - started)
                        time.sleep(0.01)
                assert not connection.is_connected(), 'Stalled connection was not closed'
                assert connection.transport.payload is None
                assert connection.needs_full_sync
        assert longest_tick < 0.1, f'Network callback stalled for {longest_tick:.3f}s'
        print(f'BLENDER_LIVE_LINK_REAL_SOCKET_OK max_callback_seconds={longest_tick:.6f}')
    finally:
        extension.unregister()


run()
real_socket_checks()
