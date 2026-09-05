"""Run with: python3 -m unittest discover -s tools -p 'test_live_link_transport.py'."""
import errno
from pathlib import Path
import socket
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from live_link_transport import LiveLinkTransport


class Clock:
    now = 0.0
    def __call__(self):
        return self.now


class FakeSocket:
    result = errno.EINPROGRESS
    error = 0
    closed = False
    readable = False
    writable = False
    block = False
    chunk = 1000000
    eof = False

    def __init__(self):
        self.written = bytearray()

    def setblocking(self, value):
        assert value is False

    def setsockopt(self, *args):
        pass

    def connect_ex(self, address):
        return self.result

    def getsockopt(self, *args):
        return self.error

    def recv(self, *args):
        return b'' if self.eof else b'x'

    def send(self, data):
        if self.block:
            raise BlockingIOError()
        size = min(len(data), self.chunk)
        self.written.extend(data[:size])
        return size

    def close(self):
        self.closed = True


def select_fake(read, write, exceptional, timeout):
    assert timeout == 0
    return ([s for s in read if s.readable], [s for s in write if s.writable], [])


class TransportTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.sockets = []
        def factory(*args):
            sock = FakeSocket()
            self.sockets.append(sock)
            return sock
        self.transport = LiveLinkTransport(socket_factory=factory, clock=self.clock, select_fn=select_fake)

    def connect(self):
        self.transport.poll()
        self.sockets[-1].writable = True
        self.transport.poll()
        self.assertTrue(self.transport.ready)
        return self.sockets[-1]

    def test_pending_connection_and_timeout(self):
        self.transport.poll()
        self.assertEqual(self.transport.state, 'connecting')
        self.clock.now = 4.9
        self.transport.poll()
        self.assertEqual(len(self.sockets), 1)
        self.clock.now = 5.0
        self.transport.poll()
        self.assertEqual(self.transport.state, 'disconnected')
        self.assertTrue(self.sockets[0].closed)
        self.clock.now = 5.9
        self.transport.poll()
        self.assertEqual(len(self.sockets), 1)
        self.clock.now = 6.0
        self.transport.poll()
        self.assertEqual(len(self.sockets), 2)

    def test_refused_connection(self):
        self.transport.poll()
        self.sockets[0].writable = True
        self.sockets[0].error = errno.ECONNREFUSED
        self.transport.poll()
        self.assertEqual(self.transport.state, 'disconnected')
        self.assertFalse(self.transport.submit(b'x'))

    def test_immediate_refusal_is_rate_limited(self):
        sock = FakeSocket()
        sock.result = errno.ECONNREFUSED
        self.transport.socket_factory = lambda *args: sock
        self.transport.poll()
        self.assertEqual(self.transport.next_attempt, 1.0)
        self.assertTrue(sock.closed)

    def test_partial_sends_byte_budget_and_completion(self):
        sock = self.connect()
        sock.chunk = 1000
        done = []
        payload = bytes(range(256)) * 3000
        self.assertTrue(self.transport.submit(payload, lambda: done.append(True)))
        self.assertFalse(self.transport.submit(b'other'))
        self.transport.poll()
        self.assertEqual(len(sock.written), self.transport.TICK_BYTES)
        self.assertEqual(done, [])
        self.transport.poll()
        self.transport.poll()
        self.assertEqual(sock.written, payload)
        self.assertEqual(done, [True])
        self.assertTrue(self.transport.ready)

    def test_time_budget(self):
        sock = self.connect()
        send = sock.send
        def slow_send(data):
            self.clock.now += 0.001
            return send(data[:1])
        sock.send = slow_send
        self.transport.submit(b'abcdef')
        self.transport.poll()
        self.assertEqual(sock.written, b'ab')

    def test_backpressure_and_stall(self):
        sock = self.connect()
        sock.block = True
        done = []
        self.transport.submit(b'payload', lambda: done.append(True))
        self.transport.poll()
        self.assertEqual(self.transport.offset, 0)
        self.clock.now = 5
        self.transport.poll()
        self.assertTrue(sock.closed)
        self.assertIsNone(self.transport.payload)
        self.assertEqual(done, [])

    def test_disconnect_mid_payload_and_reconnect(self):
        sock = self.connect()
        done = []
        self.transport.submit(b'x' * 300000, lambda: done.append(True))
        self.transport.poll()
        sock.readable = sock.eof = True
        self.transport.poll()
        self.assertEqual(self.transport.state, 'disconnected')
        self.clock.now = 1
        new_sock = self.connect()
        self.assertEqual(new_sock.written, b'')
        self.assertEqual(done, [])
        self.assertEqual(self.transport.generation, 2)

    def test_real_local_socket(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listener.settimeout(1)
            transport = LiveLinkTransport(listener.getsockname())
            try:
                transport.poll()
                with listener.accept()[0] as peer:
                    for _ in range(100):
                        transport.poll()
                        if transport.ready:
                            break
                    self.assertTrue(transport.ready)
                    completed = []
                    transport.submit(b'hello', lambda: completed.append(True))
                    for _ in range(100):
                        transport.poll()
                        if completed:
                            break
                    peer.settimeout(1)
                    self.assertEqual(peer.recv(5), b'hello')
                    self.assertEqual(completed, [True])
            finally:
                transport.close()


if __name__ == '__main__':
    unittest.main()
