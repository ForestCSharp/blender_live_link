"""Test the built game's TCP framing and reconnects (requires a graphics session).

python3 game/tests/live_link_network_smoke.py --payload /tmp/scene_update.bin
The payload can be produced by tools/ci_blender_smoke.py.
"""
import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


def main():
    game_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=game_root / 'bin/game')
    parser.add_argument('--payload', type=Path, required=True)
    args = parser.parse_args()
    payload = args.payload.read_bytes()
    assert struct.unpack_from('<I', payload)[0] == len(payload) - 4
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]

    # FlatBuffers permits unused trailing bytes. Increasing frame sizes makes
    # each receive observable through the game's largest-payload capture hook.
    def frame(padding):
        return struct.pack('<I', len(payload) - 4 + padding) + payload[4:] + bytes(padding)

    with tempfile.TemporaryDirectory(prefix='live-link-network-') as directory:
        capture = Path(directory) / 'capture.bin'
        log_path = Path(directory) / 'game.log'
        with log_path.open('w') as log:
            game = subprocess.Popen(
                [str(args.game.resolve()), '--port', str(port)], cwd=game_root,
                env=dict(os.environ, GAME2_LIVE_LINK_CAPTURE=str(capture)),
                stdout=log, stderr=subprocess.STDOUT,
            )
            def connect():
                deadline = time.monotonic() + 30
                while game.poll() is None:
                    try:
                        return socket.create_connection(('127.0.0.1', port), timeout=1)
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.05)
                raise RuntimeError(f'Game exited {game.returncode}')

            def expect(expected):
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if capture.exists() and capture.read_bytes() == expected:
                        return
                    if game.poll() is not None:
                        break
                    time.sleep(0.02)
                raise AssertionError('Receiver did not capture the expected complete frame')

            try:
                with connect() as peer:
                    peer.sendall(payload[:2])  # Disconnect inside the prefix.
                with connect() as peer:
                    peer.sendall(payload[:1])
                    time.sleep(0.03)
                    peer.sendall(payload[1:])
                    expect(payload)
                    peer.sendall(frame(16) + frame(32))
                    expect(frame(32))  # Both coalesced frames must be consumed.
                    peer.sendall(frame(48)[:10])  # Disconnect inside the body.
                with connect() as peer:
                    peer.sendall(frame(64))
                    expect(frame(64))
                print('LIVE_LINK_GAME_NETWORK_OK')
            except Exception:
                print(log_path.read_text()[-8000:])
                raise
            finally:
                if game.poll() is None:
                    game.terminate()
                    try:
                        game.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        game.kill()
                        game.wait()


if __name__ == '__main__':
    main()
