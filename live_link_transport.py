"""Blender-independent, single-payload nonblocking TCP transport."""
import errno
import select
import socket
import time


class LiveLinkTransport:
    RETRY_SECONDS = 1.0
    TIMEOUT_SECONDS = 5.0
    TICK_SECONDS = 0.002
    TICK_BYTES = 256 * 1024

    def __init__(self, address=('127.0.0.1', 65432), *, socket_factory=socket.socket,
                 clock=time.monotonic, select_fn=select.select):
        self.address = address
        self.socket_factory = socket_factory
        self.clock = clock
        self.select = select_fn
        self.socket = None
        self.state = 'disconnected'
        self.next_attempt = 0.0
        self.deadline = 0.0
        self.generation = 0
        self.payload = None
        self.offset = 0
        self.on_complete = None

    @property
    def ready(self):
        return self.state == 'connected' and self.payload is None

    def close(self, *, retry_delay=0.0):
        if self.socket is not None:
            self.socket.close()
        self.socket = None
        self.state = 'disconnected'
        self.payload = None
        self.offset = 0
        self.on_complete = None
        self.next_attempt = self.clock() + retry_delay

    def submit(self, data, on_complete=None):
        """Return acceptance, not delivery; completion runs from a later poll."""
        if not self.ready:
            return False
        self.payload = memoryview(data)
        self.offset = 0
        self.on_complete = on_complete
        self.deadline = self.clock() + self.TIMEOUT_SECONDS
        return True

    def _connected(self):
        self.state = 'connected'
        self.generation += 1

    def poll(self):
        started = self.clock()
        try:
            if self.state == 'disconnected':
                if started < self.next_attempt:
                    return
                self.socket = self.socket_factory(socket.AF_INET, socket.SOCK_STREAM)
                self.socket.setblocking(False)
                if hasattr(socket, 'SO_NOSIGPIPE'):
                    self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_NOSIGPIPE, 1)
                result = self.socket.connect_ex(self.address)
                if result in (0, errno.EISCONN):
                    self._connected()
                elif result in (errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EALREADY,
                                errno.EINTR):
                    self.state = 'connecting'
                    self.deadline = started + self.TIMEOUT_SECONDS
                else:
                    self.close(retry_delay=self.RETRY_SECONDS)
                return

            readable, writable, exceptional = self.select(
                [self.socket], [self.socket] if self.state == 'connecting' or self.payload is not None else [],
                [self.socket], 0)
            if exceptional:
                raise ConnectionError('Socket exception')
            if self.state == 'connecting':
                if readable or writable:
                    if self.socket.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR):
                        raise ConnectionError('Connection refused')
                    self._connected()
                elif started >= self.deadline:
                    raise TimeoutError('Connection timed out')
                return

            if readable:
                # The protocol is one-way; readability normally indicates EOF.
                if not self.socket.recv(1, socket.MSG_PEEK):
                    raise ConnectionError('Peer closed')
            if self.payload is None:
                return
            if started >= self.deadline:
                raise TimeoutError('Send stalled')
            sent_this_tick = 0
            while writable and sent_this_tick < self.TICK_BYTES and self.clock() - started < self.TICK_SECONDS:
                remaining = min(len(self.payload) - self.offset, self.TICK_BYTES - sent_this_tick)
                try:
                    sent = self.socket.send(self.payload[self.offset:self.offset + remaining])
                except (BlockingIOError, InterruptedError):
                    break
                if sent == 0:
                    raise ConnectionError('Send returned zero')
                self.offset += sent
                sent_this_tick += sent
                self.deadline = self.clock() + self.TIMEOUT_SECONDS
                if self.offset == len(self.payload):
                    callback = self.on_complete
                    self.payload = None
                    self.offset = 0
                    self.on_complete = None
                    if callback is not None:
                        callback()
                    break
        except (OSError, ValueError):
            self.close(retry_delay=self.RETRY_SECONDS)
