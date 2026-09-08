"""Local renderer handoff; never terminate an unrelated port owner."""
import hashlib
import http.client
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import tempfile
import time


def state_path(root):
    key = hashlib.sha256(str(root.resolve()).encode()).hexdigest()[:20]
    return Path(tempfile.gettempdir()) / ('blender-live-link-web-' + key + '.json')


def request_shutdown(root):
    """Ask a managed bridge to exit using its per-launch capability token."""
    try:
        state = json.loads(state_path(root).read_text())
        connection = http.client.HTTPConnection('127.0.0.1', int(state['port']), timeout=2)
        try:
            connection.request('POST', '/api/shutdown', body=b'',
                               headers={'X-Renderer-Token': state['token']})
            response = connection.getresponse()
            response.read()
            return response.status == 200
        finally:
            connection.close()
    except (OSError, ValueError, KeyError, TypeError, http.client.HTTPException):
        return False


def remember(root, port, token):
    path = state_path(root)
    fd, temporary = tempfile.mkstemp(prefix=path.name, dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as output:
            json.dump({'port': port, 'token': token}, output)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def forget(root, token):
    path = state_path(root)
    try:
        if json.loads(path.read_text()).get('token') == token:
            path.unlink()
    except (OSError, ValueError):
        pass


def is_project_renderer(command, cwd, root):
    """Recognize only this checkout's bridge or native executable."""
    try:
        args = shlex.split(command)
        base = Path(cwd) if cwd else None
        def resolve(value):
            path = Path(value)
            if not path.is_absolute():
                if base is None:
                    return None
                path = base / path
            return path.resolve()
        if not args:
            return False
        if resolve(args[0]) == (root.parent / 'game/bin/game').resolve():
            return True
        executable = Path(args[0]).name.lower()
        if executable.startswith('python'):
            # Python options precede the script. Never match text inside -c code.
            for arg in args[1:]:
                if arg in ('-c', '-m'):
                    return False
                if arg.startswith('-'):
                    continue
                return resolve(arg) == (root / 'bridge.py').resolve()
    except (ValueError, OSError):
        pass
    return False


def stop_legacy_listener(root, port):
    """Migrate pre-handoff bridges / native game on systems providing lsof+ps.

    Normal web relaunch uses HTTP handoff and needs no process inspection tools.
    """
    if os.name == 'nt' or not shutil.which('lsof') or not shutil.which('ps'):
        return False
    result = subprocess.run(['lsof', '-nP', '-t', '-iTCP:' + str(port), '-sTCP:LISTEN'],
                            capture_output=True, text=True)
    stopped = False
    for value in set(result.stdout.split()):
        if not value.isdigit() or int(value) == os.getpid():
            continue
        command = subprocess.run(['ps', '-p', value, '-o', 'command='], capture_output=True, text=True).stdout.strip()
        cwd_output = subprocess.run(['lsof', '-a', '-p', value, '-d', 'cwd', '-Fn'],
                                    capture_output=True, text=True).stdout
        cwd = next((line[1:] for line in cwd_output.splitlines() if line.startswith('n')), None)
        if is_project_renderer(command, cwd, root):
            try:
                os.kill(int(value), signal.SIGTERM)
                print(f'Stopping previous project renderer (pid {value}).', flush=True)
                stopped = True
            except ProcessLookupError:
                pass
    return stopped


DEFAULT_PORT = 65432
PORT_ENV_VAR = 'BLENDER_LIVE_LINK_PORT'


def resolve_port(environ=None):
    """Live link TCP port: $BLENDER_LIVE_LINK_PORT, else DEFAULT_PORT.

    Mirrors resolve_port() in the repository-root live_link_transport.py, which
    game_web deliberately does not import: the web renderer stays runnable from
    this directory alone. A malformed value raises rather than silently binding
    a port Blender is not dialing.
    """
    value = (os.environ if environ is None else environ).get(PORT_ENV_VAR, '').strip()
    if not value:
        return DEFAULT_PORT
    try:
        port = int(value)
    except ValueError:
        port = -1
    if not 1 <= port <= 65535:
        raise ValueError(
            f'{PORT_ENV_VAR} must be a TCP port between 1 and 65535, got {value!r}')
    return port


def bind_live_link(factory, root, port=None):
    if port is None:
        port = resolve_port()
    deadline = time.monotonic() + 5
    inspected = False
    while True:
        try:
            return factory(('127.0.0.1', port))
        except OSError as exc:
            import errno
            if exc.errno != errno.EADDRINUSE:
                raise
            if not inspected:
                inspected = True
                stop_legacy_listener(root, port)
            if time.monotonic() >= deadline:
                raise OSError(f'Blender TCP port {port} is still owned by another process; '
                              'it could not be identified as this project\'s renderer.') from exc
            time.sleep(0.1)
