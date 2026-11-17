"""Resolve SDK tools without implicit downloads or changes to system installations."""
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

SDK = Path(__file__).resolve().parents[1]
PINS = json.loads((SDK / 'internal/Toolchain.json').read_text(encoding='utf-8-sig'))
TARGET = ('windows' if os.name == 'nt' else 'linux') + '-x86_64'


def cache_root():
    default = (Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData/Local')) / 'SpiteSdk/tools'
               if os.name == 'nt' else Path(os.environ.get('XDG_CACHE_HOME', Path.home() / '.cache')) / 'spite/tools')
    identity = hashlib.sha256(os.fsencode(SDK)).hexdigest()[:24]
    pointer = default / 'selections' / (identity + '.cache')
    selected = os.environ.get('SPITE_TOOLS_CACHE')
    if not selected and pointer.is_file():
        selected = pointer.read_text(encoding='utf-8').strip()
    return Path(selected or default).expanduser().resolve()


def version(value):
    return tuple(map(int, value.split('.'))) + (0,) * (3 - len(value.split('.')))


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def probe(name, path):
    path = Path(path).resolve(strict=True)
    command = ([path, '-I', '-c', 'import sys,struct; print(".".join(map(str,sys.version_info[:3]))); print(struct.calcsize("P")*8)']
               if name == 'python' else [path, '--version'])
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PYTHON', 'LD_'))}
    output = subprocess.run(command, capture_output=True, text=True, check=True, timeout=20, env=env).stdout
    found = re.search(r'\b(\d+\.\d+\.\d+)\b', output)
    pin = PINS[name]
    if not found or not version(pin['minimum']) <= version(found[1]) < version(pin['maximumExclusive']):
        found_version = found[1] if found else 'unknown version'
        remedy = (' Clear SPITE_CMAKE and run Setup.ps1 -Install -Profile build.'
                  if name == 'cmake' and os.name == 'nt' else '')
        raise ValueError(f'{path}: found {found_version}; requires {name} >= {pin["minimum"]}, '
                         f'< {pin["maximumExclusive"]}.{remedy}')
    if name == 'python' and output.splitlines()[-1] != '64':
        raise ValueError('Python must be 64-bit')
    return {'path': str(path), 'version': found[1], 'sha256': digest(path)}


def selection_file():
    identity = hashlib.sha256(os.fsencode(SDK)).hexdigest()[:24]
    return cache_root() / 'selections' / (identity + '.json')


def remember(name, entry):
    path = selection_file()
    state = json.loads(path.read_text()) if path.is_file() else {}
    previous = state.get(name)
    if (previous and previous['path'] == entry['path'] and previous['sha256'] != entry['sha256']
            and os.environ.get('SPITE_REFRESH_TOOLS') != '1'):
        raise ValueError(f'{name} changed since selection; rerun SDK setup to approve the new installation')
    state[name] = entry
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, delete=False) as temp:
        json.dump(state, temp, indent=2)
        temporary = Path(temp.name)
    try:
        temporary.replace(path)
        (path.with_suffix('.' + name)).write_text(entry['path'] + '\n', encoding='utf-8')
        path.with_suffix('.' + name + '-explicit').write_text('1' if entry.get('explicit') else '0', encoding='ascii')
    finally:
        temporary.unlink(missing_ok=True)
    return entry


def install(name, offline=None):
    pin = PINS[name]
    artifact = pin[TARGET]
    destination = cache_root() / name / pin['version']
    if destination.exists():
        raise ValueError(f'Cached {name} failed validation; inspect {destination}, do not overwrite it')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.install-', dir=destination.parent) as temporary:
        stage = Path(temporary)
        filename = Path(urllib.parse.unquote(urllib.parse.urlparse(artifact['url']).path)).name
        archive = stage / filename
        if offline:
            shutil.copy2(Path(offline) / filename, archive)
        else:
            with urllib.request.urlopen(artifact['url'], timeout=90) as response, archive.open('wb') as output:
                shutil.copyfileobj(response, output)
        if digest(archive) != artifact['sha256']:
            raise ValueError(f'{name} download checksum mismatch')
        extracted = stage / 'unpacked'
        if filename.endswith('.zip'):
            with zipfile.ZipFile(archive) as bundle:
                for item in bundle.infolist():
                    if not (extracted / item.filename).resolve().is_relative_to(extracted.resolve()):
                        raise ValueError('Tool archive path escapes extraction root')
                bundle.extractall(extracted)
        else:
            with tarfile.open(archive) as bundle:
                bundle.extractall(extracted, filter='data')
        payload = extracted / artifact['root']
        if os.name != 'nt':
            (payload / artifact['executable']).chmod(0o755)
        entry = probe(name, payload / artifact['executable'])
        if entry['version'] != pin['version']:
            raise ValueError(f'Expected pinned {name} {pin["version"]}')
        # Exclusive publication preserves another concurrent setup's cache.
        if os.name == 'nt':
            payload.rename(destination)
        else:
            sys.path.insert(0, str(Path(__file__).parent))
            from Sdk import publish_directory
            publish_directory(payload, destination)
    return destination / artifact['executable']


def resolve(name, install_missing=False, offline=None):
    if platform.machine().lower() not in ('amd64', 'x86_64'):
        raise ValueError('This SDK requires x86-64 tools')
    pin = PINS[name]
    explicit = os.environ.get('SPITE_' + name.upper())
    saved = selection_file()
    previous = json.loads(saved.read_text()).get(name, {}) if saved.is_file() else {}
    if not explicit and not install_missing and previous.get('explicit'):
        explicit = previous['path']
    cached = cache_root() / name / pin['version'] / pin[TARGET]['executable']
    candidates = [explicit] if explicit else [cached]
    if not explicit and os.environ.get('SPITE_MANAGED_TOOLS') != '1':
        if name == 'python':
            candidates.append(sys.executable)
        if previous.get('path'):
            candidates.append(previous['path'])
        candidates.extend(filter(None, (shutil.which(n) for n in
                          (('python3', 'python') if name == 'python' else (name,)))))
    for candidate in candidates:
        try:
            entry = probe(name, candidate)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            if explicit:
                raise ValueError(f'Explicit {name} is incompatible: {explicit}: {error}') from error
            continue
        entry['explicit'] = bool(explicit)
        return remember(name, entry)
    if install_missing:
        return remember(name, probe(name, install(name, offline)))
    raise ValueError(f'Compatible {name} is missing. Run SDK setup with its install option; no download performed.')


def tools(profile='editor', install_missing=False, offline=None):
    found = {name: resolve(name, install_missing, offline) for name in (('python', 'cmake') if profile == 'build' else ('python',))}
    if profile == 'build':
        # Ninja may also come from PATH or Visual Studio; Sdk.configure_arguments falls back to them.
        try:
            found['ninja'] = resolve('ninja', install_missing, offline)
        except ValueError:
            pass
    return found


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=('editor', 'build'), default='build')
    parser.add_argument('--install', action='store_true')
    parser.add_argument('--offline-tools', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(tools(args.profile, args.install, args.offline_tools), indent=2))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f'Tool setup: {error}\n')
