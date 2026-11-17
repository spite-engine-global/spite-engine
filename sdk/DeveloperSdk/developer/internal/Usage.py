"""Anonymous, locally rate-limited SDK CLI activity."""
import contextlib
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import urllib.request

ENDPOINT = 'https://spite-usage.falling-cherry-889a.workers.dev/v1/activity'


def state_dir():
    key = 'LOCALAPPDATA' if os.name == 'nt' else 'XDG_STATE_HOME'
    candidate = os.environ.get(key, '')
    if candidate and Path(candidate).is_absolute():
        base = Path(candidate)
    else:
        home = Path.home()
        if not home.is_absolute():
            raise OSError('No absolute profile directory')
        base = home / ('AppData/Local' if os.name == 'nt' else '.local/state')
    return base / ('SPITE' if os.name == 'nt' else 'spite') / 'usage'


def suppressed(env):
    return any(env.get(k, '').lower() not in ('', '0', 'false') for k in
               ('CI', 'DO_NOT_TRACK', 'SPITE_USAGE_DISABLE', 'SPITE_NO_USER_STATE', 'PYTEST_CURRENT_TEST'))


@contextlib.contextmanager
def locked(root):
    if not root.is_absolute():
        raise OSError('Usage state path must be absolute')
    root.mkdir(parents=True, exist_ok=True)
    with open(root / 'state.lock', 'a+b') as lock:
        if os.name == 'nt':
            import msvcrt
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            if os.name == 'nt':
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def read_state(root):
    path = root / 'state.json'
    if path.exists():
        state = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(state, dict) or not isinstance(state.get('enabled'), bool) or not isinstance(state.get('lastAttemptUtcDay'), str):
            raise ValueError('Invalid usage state')
        return state
    days = sorted(p.name[4:] for p in root.glob('day-????-??-??'))
    return {'enabled': not (root / 'disabled').exists(), 'lastAttemptUtcDay': days[-1] if days else ''}


def write_state(root, state):
    temp = root / 'state.tmp'
    with open(temp, 'w', encoding='utf-8') as f:
        json.dump(state, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(temp, root / 'state.json')
    for p in root.glob('day-????-??-??'):
        p.unlink(missing_ok=True)
    (root / 'disabled').unlink(missing_ok=True)


def enabled(env=None, root=None):
    if suppressed(os.environ if env is None else env):
        return False
    try:
        root = root or state_dir()
        if not root.is_absolute():
            return False
        return read_state(root)['enabled']
    except (OSError, ValueError, TypeError):
        return False


def set_enabled(value):
    root = state_dir()
    with locked(root):
        state = read_state(root)
        state['enabled'] = value
        write_state(root, state)


def qualifies(command, host_args, env=None):
    if command not in ('new', 'build', 'run') or not enabled(env):
        return False
    return command != 'run' or not any(a == '--frames' or a.startswith('--frames=') for a in host_args)


def launch(version):
    try:
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        subprocess.Popen([sys.executable, '-B', str(Path(__file__).resolve()), version],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         close_fds=True, creationflags=flags, start_new_session=os.name != 'nt')
    except (OSError, subprocess.SubprocessError):
        pass


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def report(version, root=None, now=None, opener=None):
    try:
        if suppressed(os.environ):
            return
        root = root or state_dir()
        with locked(root):
            state = read_state(root)
            day = (now or datetime.datetime.now(datetime.timezone.utc)).date().isoformat()
            if not state['enabled'] or day <= state['lastAttemptUtcDay']:
                return
            state['lastAttemptUtcDay'] = day
            write_state(root, state)
            platform = 'windows' if sys.platform == 'win32' else 'linux' if sys.platform.startswith('linux') else 'unknown'
            payload = json.dumps({'sdkVersion': version, 'platform': platform}).encode('utf-8')
            request = urllib.request.Request(ENDPOINT, data=payload, headers={'Content-Type': 'application/json'}, method='POST')
            send = opener or urllib.request.build_opener(NoRedirect()).open
            with send(request, timeout=2):
                pass
    except (OSError, ValueError, TypeError):
        pass


if __name__ == '__main__':
    watchdog = threading.Timer(5, os._exit, args=(0,))
    watchdog.daemon = True
    watchdog.start()
    report(sys.argv[1])
    watchdog.cancel()
