"""Project-local generations for approved SDK source modules.

The caller owns the reviewed manifest, toolchain and binary validation hooks.
"""
import hashlib
import json
import os
import re
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import uuid
import copy

VENDOR_INPUTS = {
    'stb_rect_pack': ('stb_rect_pack.h', 'source/external/stb/stb_rect_pack.h'),
    'stb_image_write': ('stb_image_write.h', 'source/external/stb/stb_image_write.h'),
    'miniaudio': ('miniaudio/miniaudio.h', 'source/external/miniaudio/miniaudio.h'),
}
FIELDS = ('sdkBuildId', 'target', 'configuration', 'apiStamp', 'toolsetStamp', 'gpuContractId')
STANDARD_HEADERS = set(('algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono '
                        'cinttypes climits cmath codecvt compare complex concepts condition_variable coroutine csetjmp csignal '
                        'cstdarg cstddef cstdint cstdio cstdlib cstring ctime cwchar cwctype deque exception execution expected '
                        'filesystem format forward_list fstream functional future initializer_list iomanip ios iosfwd iostream '
                        'istream iterator latch limits list locale map memory memory_resource mutex new numbers numeric optional '
                        'ostream queue random ranges ratio regex scoped_allocator semaphore set shared_mutex source_location span '
                        'sstream stack stacktrace stdexcept stop_token streambuf string string_view syncstream system_error '
                        'thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector '
                        'version assert.h errno.h float.h limits.h math.h stdarg.h stddef.h stdint.h stdio.h stdlib.h string.h time.h wchar.h').split())
PLATFORM_HEADERS = {'windows': {'Windows.h', 'windows.h', 'objbase.h', 'aclapi.h', 'sddl.h', 'intrin.h'},
                    'posix': {'unistd.h', 'dirent.h', 'fcntl.h', 'sys/file.h', 'sys/stat.h', 'sys/sysctl.h', 'poll.h'}}
_COMMENTS = re.compile(r'R"(?P<raw>[^\s()\\]{0,16})\([\s\S]*?\)(?P=raw)"|/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
_INCLUDE = re.compile(r'^\s*#\s*include\s*(.*?)\s*$', re.MULTILINE)
_LITERAL = re.compile(r'^([<"])([^>"\r\n]+)[>"]\s*$')


def _canonical_hash(value):
    return hashlib.sha256((json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False) + '\n').encode('utf-8')).hexdigest()


def compatibility_from_files(sdk, target, configuration, api_stamp, toolset_stamp, binary_files, gpu_files):
    sdk = Path(sdk).resolve(strict=True)
    if not all(isinstance(value, str) and value for value in (target, configuration, api_stamp, toolset_stamp)):
        raise ValueError('SDK source modules: migration pending; native identity is incomplete')
    if not binary_files or not gpu_files or len(set(binary_files)) != len(binary_files) or len(set(gpu_files)) != len(gpu_files):
        raise ValueError('SDK source modules: migration pending; binary and GPU contract inventories are required')
    binaries = {name: _hash(_path(sdk, name)) for name in sorted(binary_files)}
    gpu = {name: _hash(_path(sdk, name)) for name in sorted(gpu_files)}
    gpu_id = _canonical_hash(gpu)
    build_id = _canonical_hash({'target': target, 'configuration': configuration, 'apiStamp': api_stamp,
                                'toolsetStamp': toolset_stamp, 'gpuContractId': gpu_id, 'binaryFiles': binaries})
    return {'sdkBuildId': build_id, 'target': target, 'configuration': configuration, 'apiStamp': api_stamp,
            'toolsetStamp': toolset_stamp, 'gpuContractId': gpu_id}, binaries, gpu


def _identity(values):
    if not isinstance(values, dict) or any(not isinstance(values.get(k), str) or not values[k] for k in FIELDS):
        raise ValueError('SDK source modules: migration pending; required compatibility metadata is missing')
    return {k: values[k] for k in FIELDS}


def for_target(manifest, target):
    result = copy.deepcopy(manifest)
    protected = result.get('protectedRuntimeFilesByTarget')
    if not isinstance(protected, dict) or not isinstance(protected.get(target), list):
        raise ValueError('SDK source modules: migration pending; target runtime protections are missing')
    result['protectedRuntimeFiles'] = protected[target]
    modules = result.get('modules')
    if not isinstance(modules, dict) or not modules:
        raise ValueError('SDK source modules: migration pending; approved modules are missing')
    for name, spec in modules.items():
        artifacts = spec.get('artifactsByTarget') if isinstance(spec, dict) else None
        if not isinstance(artifacts, dict) or not isinstance(artifacts.get(target), list) or not artifacts[target]:
            raise ValueError('SDK source modules: migration pending; target artifacts are missing for ' + name)
        spec['artifacts'] = artifacts[target]
    return result


def _rel(value):
    if not isinstance(value, str) or '\\' in value or ':' in value:
        raise ValueError('Invalid relative artifact path')
    p = PurePosixPath(value)
    if not p.parts or p.is_absolute() or any(x in ('.', '..') for x in value.split('/')):
        raise ValueError('Invalid relative artifact path: ' + value)
    return p


def _path(root, relative, exists=True):
    root = Path(root).resolve(strict=True)
    p = root
    for part in _rel(relative).parts:
        p = p / part
        if p.is_symlink() or (p.exists() and getattr(p.lstat(), 'st_file_attributes', 0) & 0x400):
            raise ValueError('Links are not allowed: ' + str(p))
    if not p.resolve().is_relative_to(root) or (exists and not p.is_file()):
        raise ValueError('Missing or escaping file: ' + str(p))
    return p


def _source_files(root):
    root = Path(root).resolve(strict=True)
    files = {}
    for directory, dirs, names in os.walk(root, topdown=True, followlinks=False):
        current = Path(directory)
        for name in list(dirs):
            path = current / name
            if path.is_symlink() or getattr(path.lstat(), 'st_file_attributes', 0) & 0x400:
                raise ValueError('Links are not allowed in source trees')
            if name.casefold() == '.git':
                if current != root:
                    raise ValueError('Nested Git repositories are not source inputs')
                dirs.remove(name)
        for name in names:
            path = current / name
            if path.is_symlink() or getattr(path.lstat(), 'st_file_attributes', 0) & 0x400:
                raise ValueError('Links are not allowed in source trees')
            if current == root and name.casefold() == '.git':
                continue
            if path.is_file():
                files[path.relative_to(root).as_posix()] = path
    return files


def _tree(root):
    h = hashlib.sha256()
    for name, path in sorted(_source_files(root).items()):
        encoded = name.encode('utf-8')
        h.update(len(encoded).to_bytes(8, 'big')); h.update(encoded)
        h.update(bytes.fromhex(_hash(path)))
    return h.hexdigest()


def _provenance(value):
    if not isinstance(value, dict):
        raise ValueError('Invalid custom source provenance')
    if value == {'kind': 'snapshot'}:
        return dict(value)
    if set(value) == {'kind', 'revision'} and value.get('kind') == 'git' and isinstance(value.get('revision'), str) and re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', value['revision']):
        return dict(value)
    raise ValueError('Invalid custom source provenance')


def _hash(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def _no_links(path):
    path = Path(path).absolute()
    for p in (path, *path.parents):
        if p.is_symlink() or (p.exists() and getattr(p.lstat(), 'st_file_attributes', 0) & 0x400):
            raise ValueError('Links are not allowed: ' + str(p))
    return path


def _roots(sdk, project):
    sdk, project = Path(sdk).resolve(strict=True), Path(project).resolve(strict=True)
    if project.is_relative_to(sdk) or sdk.is_relative_to(project):
        raise ValueError('SDK and project must be separate trees')
    _path(project, '.spite/sdk.json')
    return sdk, project


def _module(manifest, name):
    modules = manifest.get('modules', {})
    if not isinstance(name, str) or name not in modules:
        raise ValueError('Module is not approved: ' + str(name))
    spec = modules[name]
    if not isinstance(spec.get('target'), str) or not spec['target'] or not spec.get('artifacts'):
        raise ValueError('Incomplete approved module manifest')
    for a in spec['artifacts']:
        _rel(a['output']); _rel(a['runtime'])
        if a.get('kind') not in ('binary', 'shader'):
            raise ValueError('Unknown artifact kind')
    return spec


def validate_source_inputs(sdk, source, module, spec):
    sdk = Path(sdk).resolve(strict=True)
    source = _no_links(source).resolve(strict=True)
    owner = 'source/open/' + module + '/'
    names = spec.get('sources')
    headers = spec.get('sdkHeaders')
    if not isinstance(names, list) or not names or not isinstance(headers, list):
        raise ValueError('SDK source modules: migration pending; approved source inputs are missing')
    source_names = []
    for name in names:
        if not isinstance(name, str) or not name.startswith(owner):
            raise ValueError('Invalid approved module source path')
        source_names.append(str(_rel(name[len(owner):])))
    directories = []
    for name in spec.get('includeDirectories', []):
        if not isinstance(name, str) or name != owner[:-1] and not name.startswith(owner):
            raise ValueError('Invalid approved module include directory')
        relative = name[len(owner):] if name != owner[:-1] else ''
        if relative:
            _rel(relative)
        directories.append(relative)
    external_inputs = spec.get('externalInputs', {})
    vendor_includes = {VENDOR_INPUTS[key][0] for key in spec.get('vendorInputs', {}) if key in VENDOR_INPUTS}
    package_headers = {header for package in spec.get('externalPackages', {}).values() for header in package.get('headers', [])}
    package_headers.update(name.removeprefix('source/external/jolt/') for name in spec.get('staticDependencies', {}).get('Jolt', {}).get('headers', {}))
    external_files = {entry['stagedPath'][len(owner):] for entry in external_inputs.values()
                      if isinstance(entry.get('stagedPath'), str) and entry['stagedPath'].startswith(owner)}
    external_directories = set()
    for name in external_files:
        matches = [directory for directory in directories if directory and PurePosixPath(name).is_relative_to(PurePosixPath(directory))]
        external_directories.add(max(matches, key=len) if matches else PurePosixPath(name).parts[0])
    platform_sources = {name[len(owner):]: platform for platform, values in spec.get('platformSources', {}).items()
                        for name in values if name.startswith(owner)}
    seen = set()

    def visit(name, chain):
        name = str(_rel(name))
        if name in seen:
            return
        seen.add(name)
        path = _path(source, name)
        if any(part.casefold() == '.git' for part in path.resolve(strict=True).relative_to(source).parts):
            raise ValueError('Git metadata cannot be a source input')
        text = path.read_text(encoding='utf-8-sig')
        if module == 'editor' and name == 'EditorBuildId.cpp':
            if (source / 'SpiteBuildId.h').exists():
                raise ValueError('Unreviewed generated editor build ID')
            text = text.replace('#if __has_include("SpiteBuildId.h")', '#if 0')
        text = _COMMENTS.sub(lambda match: re.sub(r'[^\n]', ' ', match.group()) if match.group().startswith(('/', 'R"')) else match.group(), text)
        if re.search(r'(?m)^\s*#\s*(?:include_next|import|embed)\b|^\s*(?:export\s+)?import\s+|\b__has_include\s*\(', text):
            raise ValueError(' -> '.join(chain) + ': unsupported dynamic source input')
        for match in _INCLUDE.finditer(text):
            line = text.count('\n', 0, match.start()) + 1
            literal = _LITERAL.fullmatch(match.group(1))
            edge = chain + [f'{name}:{line}']
            if not literal:
                raise ValueError(' -> '.join(edge) + ': macro or malformed include')
            delimiter, include = literal.groups()
            if module == 'editor' and name == 'EditorBuildId.cpp' and include == 'SpiteBuildId.h':
                continue
            if '\\' in include or ':' in include or PurePosixPath(include).is_absolute() or '..' in PurePosixPath(include).parts:
                raise ValueError(' -> '.join(edge + [include]) + ': include escapes approved roots')
            candidates = [PurePosixPath(name).parent / include] + [PurePosixPath(directory) / include for directory in directories]
            local = next((p for p in candidates if (source / p).exists() or (source / p).is_symlink()), None)
            if local is not None:
                if include in vendor_includes:
                    raise ValueError(' -> '.join(edge + [include]) + ': unapproved vendor shadowing')
                local_name = str(local)
                if local_name in external_files:
                    continue
                if include in package_headers or any(local.is_relative_to(PurePosixPath(directory)) for directory in external_directories):
                    raise ValueError(' -> '.join(edge + [local_name]) + ': unapproved external header shadowing')
                if local.suffix.lower() not in ('.h', '.hpp', '.inl', '.ipp', '.inc'):
                    raise ValueError(' -> '.join(edge + [local_name]) + ': local source injection is not supported')
                visit(local_name, edge + [local_name])
            elif include.removeprefix('spite/') in headers or include == 'base/ApiStamp.generated.hpp':
                _path(sdk, 'include/spite/' + include.removeprefix('spite/'))
            elif include in external_inputs or include in package_headers or include in vendor_includes:
                continue
            elif delimiter == '<' and (include in STANDARD_HEADERS or include in {'Windows.h', 'windows.h', 'intrin.h', 'unistd.h', 'dirent.h', 'fcntl.h', 'shellapi.h', 'linux/openat2.h', 'poll.h', 'pthread.h', 'signal.h', 'spawn.h', 'crt_externs.h', 'sys/file.h', 'sys/resource.h', 'sys/stat.h', 'sys/syscall.h', 'sys/wait.h', 'sys/types.h'} or include in PLATFORM_HEADERS.get(platform_sources.get(name), set())):
                continue
            else:
                raise ValueError(' -> '.join(edge + [include]) + ': unapproved source dependency')
    for name in source_names:
        visit(name, [module, name])


def copy_stock_sources(sdk, destination, stock_relative):
    sdk = Path(sdk).resolve(strict=True)
    destination = _no_links(destination)
    if destination.exists() or destination.is_symlink() or destination.resolve().is_relative_to(sdk):
        raise ValueError('Choose a new source tree outside the SDK')
    source = sdk.joinpath(*_rel(stock_relative).parts)
    # Check every component and entry before copying.
    for part in [source, *source.parents]:
        if part == sdk.parent: break
        if part.is_symlink() or getattr(part.lstat(), 'st_file_attributes', 0) & 0x400:
            raise ValueError('Links are not allowed in stock sources')
    _tree(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.spite-source-', dir=destination.parent) as tmp:
        stage = Path(tmp) / 'source'
        shutil.copytree(source, stage)
        if destination.exists() or destination.is_symlink():
            raise ValueError('Source destination already exists')
        os.rename(stage, destination)
    return destination


def build_module(sdk, project, manifest, compatibility, name, source, builder, validator, shader_validator=None, source_provenance=None):
    sdk, project = _roots(sdk, project)
    identity = _identity(compatibility)
    spec = _module(manifest, name)
    provenance = _provenance({'kind': 'snapshot'} if source_provenance is None else source_provenance)
    source = _no_links(source).resolve(strict=True)
    if source.is_relative_to(sdk) or sdk.is_relative_to(source):
        raise ValueError('Source must be outside the SDK')
    before = _tree(source)
    builds = project / '.spite' / 'source-builds'
    builds.mkdir(parents=True, exist_ok=True)
    _path(project, '.spite/source-builds', False)
    build = builds / uuid.uuid4().hex
    build.mkdir()
    try:
        builder(sdk, source, build, spec['target'], identity)
        if before != _tree(source):
            raise ValueError('Source changed during build')
        artifacts = []
        for a in spec['artifacts']:
            output = _path(build, a['output'])
            validator(output, a, identity)
            if a['kind'] == 'shader':
                if shader_validator is None:
                    raise ValueError('Custom shaders require an approved GPU contract validator')
                shader_validator(output, a, identity)
            artifacts.append({'output': a['output'], 'runtime': a['runtime'], 'kind': a['kind'], 'sha256': _hash(output)})
        record = {'format': 1, 'module': name, 'compatibility': identity, 'sourceProvenance': provenance, 'sourceHash': before, 'artifacts': artifacts}
        (build / 'source-build.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
        return build
    except BaseException:
        shutil.rmtree(build, ignore_errors=True)
        raise


def _validated_build(build, manifest, identity, validator, shader_validator):
    build = Path(build).resolve(strict=True)
    record = json.loads(_path(build, 'source-build.json').read_text(encoding='utf-8'))
    if record.get('format') != 1 or record.get('compatibility') != identity:
        raise ValueError('Build compatibility mismatch')
    if 'sourceRevision' in record:
        raise ValueError('Legacy source revision record requires rebuild')
    _provenance(record.get('sourceProvenance'))
    if not re.fullmatch(r'[0-9a-f]{64}', str(record.get('sourceHash', ''))):
        raise ValueError('Invalid custom source hash')
    spec = _module(manifest, record['module'])
    expected = [(a['output'], a['runtime'], a['kind']) for a in spec['artifacts']]
    actual = [(a['output'], a['runtime'], a['kind']) for a in record['artifacts']]
    if expected != actual:
        raise ValueError('Build artifacts do not match approved manifest')
    for a, contract in zip(record['artifacts'], spec['artifacts']):
        p = _path(build, a['output'])
        if _hash(p) != a['sha256']:
            raise ValueError('Build artifact hash mismatch')
        validator(p, contract, identity)
        if a['kind'] == 'shader':
            if shader_validator is None:
                raise ValueError('Custom shaders require an approved GPU contract validator')
            shader_validator(p, contract, identity)
    return record


def select_modules(sdk, project, manifest, compatibility, runtime_files, builds, validator, shader_validator=None):
    sdk, project = _roots(sdk, project)
    identity = _identity(compatibility)
    if not isinstance(manifest.get('protectedRuntimeFiles'), list):
        raise ValueError('SDK source modules: migration pending; protected runtime manifest is missing')
    protected = {str(_rel(p)).casefold() for p in manifest['protectedRuntimeFiles']}
    runtime = {str(_rel(p)).casefold(): p for p in runtime_files}
    if len(runtime) != len(runtime_files) or len({PurePosixPath(p).name.casefold() for p in runtime_files}) != len(runtime_files):
        raise ValueError('Runtime path or basename collision')
    records, replacements = [], {}
    for build in builds:
        build = _no_links(build).resolve(strict=True)
        if not build.is_relative_to((project / '.spite/source-builds').resolve()):
            raise ValueError('Build must be project-owned')
        record = _validated_build(build, manifest, identity, validator, shader_validator)
        if any(r['module'] == record['module'] for r in records):
            raise ValueError('Duplicate module')
        record['buildRecordSha256'] = _hash(_path(build, 'source-build.json'))
        record['buildPath'] = build.relative_to(project).as_posix()
        records.append(record)
        for a in record['artifacts']:
            key = a['runtime'].casefold()
            if key not in runtime or key in replacements or key in protected:
                raise ValueError('Artifact is not a unique stock runtime replacement')
            replacements[key] = (build, a)
    base = project / '.spite' / 'generations'
    base.mkdir(parents=True, exist_ok=True)
    _path(project, '.spite/generations', False)
    generation = base / uuid.uuid4().hex
    generation.mkdir()
    try:
        hashes = {}
        for key, rel in runtime.items():
            src = _path(sdk, rel)
            if key in replacements:
                build, artifact = replacements[key]
                src = _path(build, artifact['output'])
            dest = generation.joinpath(*_rel(rel).parts)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest)
            hashes[rel] = _hash(dest)
            if key in replacements and hashes[rel] != replacements[key][1]['sha256']:
                raise ValueError('Artifact changed while staging')
        selection = {'format': 1, 'generation': generation.name, 'compatibility': identity,
                     'modules': records, 'files': hashes}
        record = project / '.spite' / 'source-selection.json'
        tmp = record.with_name('.source-selection-' + uuid.uuid4().hex)
        try:
            tmp.write_text(json.dumps(selection, indent=2), encoding='utf-8')
            os.replace(tmp, record)
        finally:
            tmp.unlink(missing_ok=True)
        return generation
    except BaseException:
        shutil.rmtree(generation, ignore_errors=True)
        raise


def restore_stock(sdk, project, manifest, compatibility, runtime_files, validator):
    return select_modules(sdk, project, manifest, compatibility, runtime_files, [], validator)


def resolve_run(sdk, project, manifest, compatibility, runtime_files, executable, validator, shader_validator=None):
    sdk, project = _roots(sdk, project)
    identity = _identity(compatibility)
    record = json.loads(_path(project, '.spite/source-selection.json').read_text(encoding='utf-8'))
    if record.get('format') != 1 or record.get('compatibility') != identity:
        raise ValueError('Selection compatibility mismatch')
    generation_name = record['generation']
    if not isinstance(generation_name, str) or len(generation_name) != 32 or any(c not in '0123456789abcdef' for c in generation_name):
        raise ValueError('Invalid generation')
    generation = project / '.spite' / 'generations' / generation_name
    _path(project, '.spite/generations/' + generation_name, False)
    if not isinstance(manifest.get('protectedRuntimeFiles'), list):
        raise ValueError('SDK source modules: migration pending; protected runtime manifest is missing')
    protected = {str(_rel(p)).casefold() for p in manifest['protectedRuntimeFiles']}
    if len({PurePosixPath(str(_rel(p))).name.casefold() for p in runtime_files}) != len(runtime_files):
        raise ValueError('Runtime basename collision')
    if set(record['files']) != set(runtime_files) or executable not in runtime_files:
        raise ValueError('Selection runtime manifest mismatch')
    for rel, digest in record['files'].items():
        if _hash(_path(generation, rel)) != digest:
            raise ValueError('Selected runtime hash mismatch')
    seen = set()
    for module in record['modules']:
        build_path = str(_rel(module.get('buildPath')))
        if not build_path.startswith('.spite/source-builds/'):
            raise ValueError('Invalid selected source build path')
        build_record = _path(project, build_path + '/source-build.json')
        if _hash(build_record) != module.get('buildRecordSha256'):
            raise ValueError('Selected source build record changed')
        original = json.loads(build_record.read_text(encoding='utf-8'))
        if original != {key: value for key, value in module.items() if key not in ('buildPath', 'buildRecordSha256')}:
            raise ValueError('Selected source provenance differs from build record')
        spec = _module(manifest, module['module'])
        if module.get('compatibility') != identity or [(a['output'], a['runtime'], a['kind']) for a in module['artifacts']] != [(a['output'], a['runtime'], a['kind']) for a in spec['artifacts']]:
            raise ValueError('Selected module mismatch')
        if 'sourceRevision' in module:
            raise ValueError('Legacy source revision record requires rebuild')
        _provenance(module.get('sourceProvenance'))
        if not re.fullmatch(r'[0-9a-f]{64}', str(module.get('sourceHash', ''))):
            raise ValueError('Invalid selected source hash')
        for a, contract in zip(module['artifacts'], spec['artifacts']):
            if a['runtime'].casefold() in seen or a['runtime'].casefold() in protected or a['runtime'] not in runtime_files:
                raise ValueError('Invalid selected artifact')
            seen.add(a['runtime'].casefold())
            p = _path(generation, a['runtime'])
            if _hash(p) != a['sha256']:
                raise ValueError('Selected artifact hash mismatch')
            validator(p, contract, identity)
            if a['kind'] == 'shader':
                if shader_validator is None:
                    raise ValueError('Custom shaders require an approved GPU contract validator')
                shader_validator(p, contract, identity)
    for rel, digest in record['files'].items():
        if rel.casefold() not in seen and _hash(_path(sdk, rel)) != digest:
            raise ValueError('Stock runtime differs from the selected SDK')
    return _path(generation, executable)
