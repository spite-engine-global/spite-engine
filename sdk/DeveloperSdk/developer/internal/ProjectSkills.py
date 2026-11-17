import hashlib
import json
import os
import ntpath
from pathlib import Path
import re
import stat
import tempfile

MARKER = '.spite/sdk-managed-files.json'


def _linked(path):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    return (stat.S_ISLNK(info.st_mode) or
            bool(getattr(info, 'st_file_attributes', 0) & 0x400) or
            (stat.S_ISREG(info.st_mode) and info.st_nlink != 1))


def _safe(project, relative):
    path = project
    parts = relative.split('/')
    for index, part in enumerate(parts):
        path = path / part
        if _linked(path) or (index < len(parts) - 1 and path.exists() and not path.is_dir()):
            return False
    return True


def _valid_key(key):
    if not isinstance(key, str) or '\\' in key or ':' in key:
        return False
    parts = key.split('/')
    if any(not p or p in ('.', '..') or p.endswith((' ', '.')) or
           any(ord(c) < 32 or c in '<>"|?*' for c in p) or
           (hasattr(ntpath, 'isreserved') and ntpath.isreserved(p)) for p in parts):
        return False
    if key in ('AGENTS.md', 'CLAUDE.md'):
        return True
    return len(parts) >= 3 and parts[:2] in (['.agents', 'skills'], ['.claude', 'skills'])


def _hash(data):
    return hashlib.sha256(data).hexdigest()


def _valid_hash(value):
    return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None


def _write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=path.parent, prefix='.sdk-managed-')
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(data)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def read_manifest(project: Path) -> dict:
    project = Path(project)
    if _linked(project) or not _safe(project, MARKER):
        raise ValueError('unsafe marker location')
    marker = project / MARKER
    if not marker.exists():
        return {'format': 1, 'files': {}}
    try:
        manifest = json.loads(marker.read_text(encoding='utf-8'))
    except (OSError, ValueError) as exc:
        raise ValueError('invalid managed files marker') from exc
    if (not isinstance(manifest, dict) or type(manifest.get('format')) is not int
            or manifest['format'] != 1 or not isinstance(manifest.get('files'), dict)
            or ('projectName' in manifest and not isinstance(manifest['projectName'], str))
            or any(not _valid_key(k) or not _valid_hash(v) for k, v in manifest['files'].items())
            or not isinstance(manifest.get('pending', {}), dict)
            or any(not _valid_key(k) or (v is not None and not _valid_hash(v))
                   for k, v in manifest.get('pending', {}).items())):
        raise ValueError('invalid managed files marker')
    return manifest


def refresh(project: Path, source_skills: Path, instructions: str, *, project_name=None) -> dict:
    project = Path(project)
    source_skills = Path(source_skills)
    manifest = read_manifest(project)
    if project_name is not None and not isinstance(project_name, str):
        raise ValueError('invalid project name')
    if _linked(source_skills) or not source_skills.is_dir():
        raise ValueError('missing or unsafe SDK skills source')
    desired = {'AGENTS.md': instructions.encode('utf-8'), 'CLAUDE.md': instructions.encode('utf-8')}
    for root, dirs, files in os.walk(source_skills, followlinks=False):
        for name in dirs + files:
            path = Path(root) / name
            if _linked(path) or (not path.is_dir() and not path.is_file()):
                raise ValueError('SDK skills must contain plain files and directories')
        for name in files:
            path = Path(root) / name
            suffix = path.relative_to(source_skills).as_posix()
            for prefix in ('.agents/skills/', '.claude/skills/'):
                key = prefix + suffix
                if not _valid_key(key):
                    raise ValueError('invalid SDK skill path')
                desired[key] = path.read_bytes()
    old = dict(manifest['files'])
    for key, digest in manifest.pop('pending', {}).items():
        path = project / key
        if not _safe(project, key):
            continue
        if digest is None:
            if not path.exists():
                old.pop(key, None)
        elif path.is_file() and _hash(path.read_bytes()) == digest:
            old[key] = digest
    owned = dict(old)
    result = {'updated': [], 'preserved': [], 'removed': []}
    keys = sorted(set(old) | set(desired))
    pending = {}
    for key in keys:
        path = project / key
        if not _safe(project, key):
            continue
        if path.exists() and (not path.is_file() or key not in old or _hash(path.read_bytes()) != old[key]):
            continue
        pending[key] = _hash(desired[key]) if key in desired else None
    manifest['files'] = dict(sorted(old.items()))
    manifest['pending'] = pending
    if project_name is not None:
        manifest['projectName'] = project_name
    # Record intended hashes before replacing files, so retry can recover even if the final marker save fails.
    _write(project / MARKER, (json.dumps(manifest, indent=2, ensure_ascii=False) + '\n').encode('utf-8'))
    try:
        for key in keys:
            path = project / key
            if not _safe(project, key):
                result['preserved'].append(key)
                continue
            exists = path.exists()
            if exists and (not path.is_file() or key not in old or _hash(path.read_bytes()) != old[key]):
                result['preserved'].append(key)
                continue
            if key not in desired:
                if exists:
                    path.unlink()
                    result['removed'].append(key)
                owned.pop(key, None)
            else:
                _write(path, desired[key])
                owned[key] = _hash(desired[key])
                result['updated'].append(key)
    finally:
        manifest['files'] = dict(sorted(owned.items()))
        manifest.pop('pending', None)
        _write(project / MARKER, (json.dumps(manifest, indent=2, ensure_ascii=False) + '\n').encode('utf-8'))
    return result
