"""Local coordinated release preparation; no publication or ABI inference.

Input JSON fields: version, sdkVersion, openVersion, official, dirty, sourceIdentity,
publicSource, snapshotInventory, contracts, buildIdentities, changes, priorManifests.
buildIdentities maps target names to exact identities. Legacy buildIdentity is accepted.
Assemble every target before writing the immutable per-version official manifest.
publicSource has status (pending or canonical), exportIdentity, and, when canonical,
repository and revision. snapshotInventory maps approved relative paths to SHA-256.
Each change has id, class, explanation, reviewed=true, and contracts (names covered).
Prior manifests are complete manifests, not merely a list of version strings.
Optional patchAdditions {approval, changes: [ids]} is an owner-approved exception that lets the named
additions of this release ship in a patch version; it is copied from ReleaseVersion.json and recorded.
"""
import argparse
import copy
import hashlib
import json
import re
import stat
from functools import total_ordering
from pathlib import Path, PurePosixPath

_PATTERN = re.compile(r'^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$')
FIELDS = ('sdkBuildId', 'target', 'configuration', 'apiStamp', 'toolsetStamp', 'gpuContractId')

@total_ordering
class SemVer:
    def __init__(self, text):
        match = _PATTERN.fullmatch(text) if isinstance(text, str) else None
        if not match:
            raise ValueError('invalid SemVer: ' + str(text))
        self.text = text
        self.core = tuple(map(int, match.group(1, 2, 3)))
        self.pre = tuple(match.group(4).split('.')) if match.group(4) else ()
        if any(x.isdigit() and len(x) > 1 and x[0] == '0' for x in self.pre):
            raise ValueError('leading zero in prerelease')

    def _key(self):
        return self.core, (1,) if not self.pre else (0, tuple((0, int(x)) if x.isdigit() else (1, x) for x in self.pre))

    def __eq__(self, other):
        return isinstance(other, SemVer) and self._key() == other._key()

    def __lt__(self, other):
        if not isinstance(other, SemVer):
            return NotImplemented
        return self._key() < other._key()


def canonical(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def numeric_version(version):
    return '.'.join(map(str, SemVer(version).core))


def exact_identity(identity):
    if not isinstance(identity, dict) or any(not isinstance(identity.get(k), str) or not identity[k] for k in FIELDS):
        raise ValueError('missing exact build identity')
    return {k: identity[k] for k in FIELDS}


def _path(name):
    if not isinstance(name, str) or not name:
        raise ValueError('invalid inventory path')
    p = PurePosixPath(name)
    if '\\' in name or ':' in name or p.is_absolute() or any(part in ('', '.', '..') for part in name.split('/')) or str(p) != name:
        raise ValueError('invalid inventory path')


def _hash(value):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value) is not None


def _custom(custom):
    if not isinstance(custom, dict) or set(custom) != {'provenance', 'sourceHash', 'outputHashes'} or not _hash(custom.get('sourceHash')):
        raise ValueError('custom provenance and SHA-256 source hash required')
    provenance = custom.get('provenance')
    valid_snapshot = provenance == {'kind': 'snapshot'}
    valid_git = (isinstance(provenance, dict) and set(provenance) == {'kind', 'revision'} and
                 provenance.get('kind') == 'git' and isinstance(provenance.get('revision'), str) and
                 re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', provenance['revision']))
    if not valid_snapshot and not valid_git:
        raise ValueError('invalid custom source provenance')
    outputs = custom.get('outputHashes')
    if not isinstance(outputs, dict) or not outputs:
        raise ValueError('custom output hashes required')
    for path, digest in outputs.items():
        _path(path)
        if not _hash(digest):
            raise ValueError('invalid custom output hash')


def make_lock(version, identity, source_identity, custom=None):
    SemVer(version)
    if not isinstance(source_identity, str) or not source_identity:
        raise ValueError('missing source/export identity')
    result = {'format': 1, 'version': version, **exact_identity(identity), 'sourceIdentity': source_identity}
    if custom is not None:
        _custom(custom)
        result['custom'] = copy.deepcopy(custom)
    return result


def check_native_lock(lock, identity, source_identity, version=None):
    if not isinstance(lock, dict) or type(lock.get('format')) is not int or lock['format'] != 1:
        raise ValueError('invalid lock format')
    SemVer(lock.get('version'))
    if version is not None:
        SemVer(version)
        if lock['version'] != version:
            raise ValueError('coordinated release version mismatch')
    exact_identity(lock)
    if 'custom' in lock:
        _custom(lock['custom'])
    expected = exact_identity(identity)
    if not isinstance(source_identity, str) or not source_identity or lock.get('sourceIdentity') != source_identity or any(lock.get(k) != v for k, v in expected.items()):
        raise ValueError('native build identity mismatch; rebuild against the exact SDK')


def _reparse(path):
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400))


def snapshot_inventory(root):
    root = Path(root)
    if _reparse(root) or not root.is_dir():
        raise ValueError('snapshot must be a directory without a reparse point')
    result = {}
    def visit(directory):
        for path in sorted(directory.iterdir()):
            if _reparse(path):
                raise ValueError('snapshot reparse points and symlinks are not allowed')
            if path.is_dir():
                visit(path)
            elif path.is_file():
                result[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
            else:
                raise ValueError('unsupported snapshot entry')
    visit(root)
    return result


def _inventory(value):
    if not isinstance(value, dict) or not value:
        raise ValueError('approved snapshot inventory required')
    for name, digest in value.items():
        _path(name)
        if not _hash(digest):
            raise ValueError('invalid snapshot hash')
    return value


def _identities(data):
    if 'buildIdentities' in data and 'buildIdentity' in data:
        raise ValueError('use one build identity representation')
    if 'buildIdentities' in data:
        identities = data['buildIdentities']
    else:
        one = exact_identity(data['buildIdentity'])
        identities = {one['target']: one}
    if not isinstance(identities, dict) or not identities:
        raise ValueError('build identities required')
    result = {}
    for target, identity in identities.items():
        identity = exact_identity(identity)
        if target != identity['target']:
            raise ValueError('build identity target key mismatch')
        result[target] = identity
    return result


def _change(change):
    if not isinstance(change, dict) or set(change) != {'id', 'class', 'explanation', 'reviewed', 'contracts'} or change['class'] not in ('breaking', 'addition', 'fix') or change['reviewed'] is not True or not all(isinstance(change[k], str) and change[k].strip() for k in ('id', 'explanation')) or not isinstance(change['contracts'], list) or not all(isinstance(x, str) and x for x in change['contracts']):
        raise ValueError('invalid reviewed change record')
    return change


def prepare(data, snapshot):
    version = data['version']
    target = SemVer(version)
    if data['sdkVersion'] != version or data['openVersion'] != version:
        raise ValueError('SDK and open source versions must match')
    if type(data['official']) is not bool or type(data['dirty']) is not bool:
        raise ValueError('official and dirty must be boolean')
    if data['official'] and data['dirty']:
        raise ValueError('dirty official candidate')
    source = data['sourceIdentity']
    public = data['publicSource']
    if not isinstance(source, str) or not source or not isinstance(public, dict) or public.get('status') not in ('pending', 'canonical') or not isinstance(public.get('exportIdentity'), str) or not public['exportIdentity']:
        raise ValueError('explicit source and approved export identities required')
    if public['status'] == 'canonical' and not all(isinstance(public.get(k), str) and public[k] for k in ('repository', 'revision')):
        raise ValueError('canonical public repository and revision required')
    if public['status'] == 'pending' and ('repository' in public or 'revision' in public):
        raise ValueError('pending provenance must not claim a public revision')
    inventory = _inventory(data['snapshotInventory'])
    if snapshot_inventory(snapshot) != inventory:
        raise ValueError('exported snapshot differs from approved inventory')
    contracts = data['contracts']
    if not isinstance(contracts, dict) or not all(isinstance(k, str) and k and isinstance(v, str) and v for k, v in contracts.items()):
        raise ValueError('invalid supported contract inventory')
    identities = _identities(data)
    changes = [_change(c) for c in data['changes']]
    if len({c['id'] for c in changes}) != len(changes):
        raise ValueError('duplicate change id')
    priors = data['priorManifests']
    stable = None
    candidates = []
    same = None
    seen = set()
    for prior in priors:
        pv = SemVer(prior['version'])
        if prior.get('format') != 1 or prior['version'] in seen or not isinstance(prior.get('consumedChanges'), list) or not isinstance(prior.get('contracts'), dict):
            raise ValueError('invalid prior manifest')
        seen.add(prior['version'])
        _identities(prior)
        _inventory(prior['snapshotInventory'])
        hashes = prior.get('recordHashes')
        if not isinstance(hashes, dict) or set(hashes) != set(prior['consumedChanges']) or not all(isinstance(k, str) and k and _hash(v) for k, v in hashes.items()):
            raise ValueError('invalid prior change inventory')
        for change in prior.get('changes', []):
            _change(change)
            if hashes.get(change['id']) != hashlib.sha256(canonical(change)).hexdigest():
                raise ValueError('changed consumed record')
        if prior['version'] == version:
            same = prior
        elif pv < target and not pv.pre and (stable is None or SemVer(stable['version']) < pv):
            stable = prior
        elif pv < target and pv.pre and pv.core == target.core:
            candidates.append(prior)
    candidates.sort(key=lambda item: SemVer(item['version']))
    baseline_hashes = dict(stable['recordHashes']) if stable else {}
    record_hashes = dict(baseline_hashes)
    train = {}
    for candidate in candidates:
        declared = dict(baseline_hashes)
        for change in candidate['changes']:
            declared[change['id']] = hashlib.sha256(canonical(change)).hexdigest()
        if declared != candidate['recordHashes'] or not set(train).issubset({c['id'] for c in candidate['changes']}):
            raise ValueError('invalid prerelease change history')
        for ident, digest in candidate['recordHashes'].items():
            if ident in record_hashes and record_hashes[ident] != digest:
                raise ValueError('changed consumed record')
            record_hashes[ident] = digest
        for change in candidate['changes']:
            if change['id'] not in baseline_hashes:
                train[change['id']] = change
    previous_ids = set(record_hashes)
    for change in changes:
        digest = hashlib.sha256(canonical(change)).hexdigest()
        if change['id'] in record_hashes and record_hashes[change['id']] != digest:
            raise ValueError('changed consumed record')
        record_hashes[change['id']] = digest
        if change['id'] not in baseline_hashes:
            train[change['id']] = change
    previous = candidates[-1] if candidates else stable
    if previous:
        changed = {k for k in set(previous['contracts']) | set(contracts) if previous['contracts'].get(k) != contracts.get(k)}
        covered = {k for c in changes if c['id'] not in previous_ids for k in c['contracts']}
        if changed - covered:
            raise ValueError('changed supported contracts need reviewed records: ' + ', '.join(sorted(changed - covered)))
    # An owner-approved exception ships named additions of this release in a patch version; breaking changes never.
    exception = data.get('patchAdditions')
    if exception is not None and (not isinstance(exception, dict) or set(exception) != {'approval', 'changes'} or
                                  not isinstance(exception['approval'], str) or not exception['approval'].strip() or
                                  not isinstance(exception['changes'], list) or not exception['changes'] or
                                  len(set(exception['changes'])) != len(exception['changes']) or
                                  any(train.get(i, {}).get('class') != 'addition' for i in exception['changes'])):
        raise ValueError('a patch-addition exception needs an approval and names only additions of this release')
    excepted = set(exception['changes']) if exception else set()
    if stable and train:
        old = SemVer(stable['version'])
        if exception and (target.core[:2] != old.core[:2] or target.core[2] <= old.core[2]):
            raise ValueError('a patch-addition exception requires a patch version bump')
        level = max(1 if c['id'] in excepted else {'fix': 1, 'addition': 2, 'breaking': 2 if old.core[0] == 0 else 3}[c['class']]
                    for c in train.values())
        a, b, c = old.core
        minimum = (a + 1, 0, 0) if level == 3 else (a, b + 1, 0) if level == 2 else (a, b, c + 1)
        if target.core < minimum:
            raise ValueError('insufficient version bump')
    elif exception:
        raise ValueError('a patch-addition exception needs a prior stable release')
    if stable and target <= SemVer(stable['version']):
        raise ValueError('version must advance')
    manifest = {'format': 1, 'version': version, 'official': data['official'], 'sourceIdentity': source, 'publicSource': public, 'snapshotInventory': inventory, 'contracts': contracts, 'buildIdentities': identities, 'consumedChanges': sorted(record_hashes), 'recordHashes': record_hashes, 'changes': sorted(train.values(), key=lambda c: c['id'])}
    if exception:
        manifest['patchAdditions'] = copy.deepcopy(exception)
    notes = '# ' + version + '\n\n' + ''.join('- ' + c['explanation'].strip() + '\n' for c in manifest['changes'])
    if exception:
        notes += '\nThis release adds API in a patch version by owner approval: ' + exception['approval'].strip() + '\n'
    if same is not None and canonical(same) != canonical(manifest):
        raise ValueError('existing version identity has changed')
    return copy.deepcopy(manifest), notes.encode('utf-8')


def write_immutable(path, content):
    path = Path(path)
    if path.exists():
        if path.read_bytes() != content:
            raise ValueError('existing release output differs: ' + str(path))
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('xb') as stream:
            stream.write(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare', 'check'))
    parser.add_argument('--input', required=True)
    parser.add_argument('--snapshot', required=True)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--notes', required=True)
    args = parser.parse_args()
    data = json.loads(Path(args.input).read_text(encoding='utf-8-sig'))
    manifest, notes = prepare(data, args.snapshot)
    outputs = ((Path(args.manifest), canonical(manifest)), (Path(args.notes), notes))
    if args.action == 'check':
        for path, content in outputs:
            if not path.is_file() or path.read_bytes() != content:
                raise ValueError('release output missing or different: ' + str(path))
    else:
        for path, content in outputs:
            if path.exists() and path.read_bytes() != content:
                raise ValueError('existing release output differs: ' + str(path))
        for path, content in outputs:
            write_immutable(path, content)

if __name__ == '__main__':
    main()
