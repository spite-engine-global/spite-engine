"""Create, build, run, package and verify projects without an engine checkout."""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import struct
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path, PureWindowsPath
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import Toolchain
import ReleaseVersion
import SourceModules
import ProjectSkills
import Usage

SDK = Path(__file__).resolve().parent.parent
_BUILD_ENV = None


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def clean_env(runtime=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("SPITE_", "LD_", "PYTHON", "VCPKG", "CMAKE_"))}
    if "SPITE_LOG_LEVEL" in os.environ:
        env["SPITE_LOG_LEVEL"] = os.environ["SPITE_LOG_LEVEL"]
    env.update(SPITE_NO_USER_STATE="1", VK_LOADER_LAYERS_DISABLE="~implicit~")
    if runtime:
        env["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32") if os.name == "nt" else "/usr/bin:/bin"
    return env


def run(command, cwd, log=None, runtime=False, quiet=False):
    env = clean_env(runtime)
    if not runtime and _BUILD_ENV is not None:
        env.update(_BUILD_ENV)
    result = subprocess.run(list(map(str, command)), cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    if log:
        log.write_text(result.stdout, encoding="utf-8")
    if not quiet:
        print(result.stdout, end="")
    if result.returncode:
        raise ValueError(f"Command failed ({result.returncode}): {command[0]}" + (":\n" + result.stdout[-2000:] if quiet else ""))
    return result.stdout


def writable_arguments(root):
    arguments = []
    for option, folder in (("user-config", "user"), ("saves", "saves"), ("scenes", "scenes"),
                           ("cache", "cache"), ("shadows", "shadows"), ("crashes", "crashes"),
                           ("artifacts", "artifacts"), ("control", "control")):
        path = root / folder
        path.mkdir(parents=True, exist_ok=True)
        arguments += ["--" + option + "-dir", path]
    return arguments


def cmake():
    return Path(Toolchain.resolve('cmake')['path'])


def toolset_version(text):
    return tuple(int(part) for part in text.split(".")[:2])


def minimum_toolset(pin):
    return pin.get("minimumToolset", pin["toolset"])


def toolset_accepted(text, minimum):
    """MSVC 14.x at or above the minimum; the family stays exact."""
    try:
        found, floor = toolset_version(text), toolset_version(minimum)
    except ValueError:
        return False
    return found[0] == floor[0] and found >= floor


def toolset_stamps_compatible(module, host):
    """Python mirror of spite::toolsetStampAccepted (source/closed/base/ToolsetCompat.hpp)."""
    if module == host:
        return True
    parsed = [re.fullmatch(r"msvc(\d+)(-.*)", stamp) for stamp in (module, host)]
    if not all(parsed) or parsed[0][2] != parsed[1][2]:
        return False
    minimum = 1900 + toolset_version(minimum_toolset(read(SDK / "internal/CompilerSetup.json")))[1]
    versions = [int(m[1]) for m in parsed]
    return min(versions) >= minimum and versions[0] // 100 == versions[1] // 100


def installed_toolsets(install_path, minimum):
    """Accepted MSVC toolset directories of one Visual Studio instance, newest first: (version tuple, name)."""
    found = []
    for compiler in Path(install_path).glob("VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe"):
        name = compiler.parents[3].name
        if toolset_accepted(name, minimum):
            found.append((tuple(int(p) for p in re.findall(r"\d+", name)), name))
    return sorted(found, reverse=True)


def vswhere_instances():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        return []
    result = subprocess.run([str(vswhere), "-all", "-prerelease", "-products", "*", "-version", "[18.0,)",
                             "-property", "installationPath"], text=True, capture_output=True, check=True)
    return [Path(p.strip()) for p in result.stdout.splitlines() if p.strip()]


def windows_build_environment(pin):
    minimum = minimum_toolset(pin)
    env = {key.upper(): value for key, value in clean_env().items()}
    def compatible(values):
        compiler = shutil.which('cl', path=values.get('PATH'))
        tool_dir = values.get('VCTOOLSINSTALLDIR')
        return bool(compiler and tool_dir and toolset_accepted(values.get('VCTOOLSVERSION', ''), minimum) and
                    Path(compiler).resolve().is_relative_to(Path(tool_dir).resolve()))
    if compatible(env):
        return env
    if not (Path(os.environ.get('ProgramFiles(x86)', '')) / 'Microsoft Visual Studio/Installer/vswhere.exe').is_file():
        raise ValueError('Visual Studio discovery is missing; run spite setup')
    candidates = [(toolsets[0], path) for path in vswhere_instances() if (toolsets := installed_toolsets(path, minimum))]
    if not candidates:
        raise ValueError('MSVC ' + minimum + ' or newer is missing; run spite setup')
    (_, name), instance = max(candidates, key=lambda item: item[0][0])
    version = '.'.join(name.split('.')[:2])
    devcmd = instance / 'Common7/Tools/VsDevCmd.bat'
    command = f'call "{devcmd}" -no_logo -arch=x64 -host_arch=x64 -vcvars_ver={version} && set'
    result = subprocess.run(f'cmd.exe /d /s /c "{command}"', env=env, text=True, capture_output=True)
    if result.returncode:
        detail = result.stderr.strip()[:2000] or 'No stderr was reported.'
        raise ValueError(f'Visual Studio environment initialization failed ({result.returncode}): {detail}')
    for line in result.stdout.splitlines():
        key, separator, value = line.partition('=')
        if separator and key:
            env[key.upper()] = value
    if not compatible(env):
        raise ValueError('The MSVC ' + minimum + '+ developer environment could not be activated')
    return env


def managed_ninja():
    try:
        return Toolchain.resolve('ninja')['path']
    except (ValueError, OSError, subprocess.SubprocessError):
        return None


def configure_arguments(build_dir, windows=None):
    global _BUILD_ENV
    windows = os.name == "nt" if windows is None else windows
    cache = build_dir / "CMakeCache.txt"
    entries = {}
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8").splitlines():
            if ":" in line and "=" in line:
                key, value = line.split("=", 1)
                entries[key.split(":", 1)[0]] = value
    if windows:
        pin = read(SDK / "internal/CompilerSetup.json")
        if entries.get('CMAKE_GENERATOR', 'Ninja') != 'Ninja':
            raise ValueError("Existing build uses another generator; choose a fresh build directory")
        _BUILD_ENV = windows_build_environment(pin)
        ninja = shutil.which('ninja', path=_BUILD_ENV.get('PATH'))
        if not ninja and _BUILD_ENV.get('VSINSTALLDIR'):
            candidate = Path(_BUILD_ENV['VSINSTALLDIR']) / 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
            if candidate.is_file():
                ninja = str(candidate)
        ninja = ninja or managed_ninja()
        compiler = shutil.which('cl', path=_BUILD_ENV.get('PATH'))
        if not ninja or not compiler:
            raise ValueError('Ninja and an MSVC toolset of the required minimum are required; run spite setup')
        return ['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + ninja, '-DCMAKE_CXX_COMPILER=' + compiler]
    _BUILD_ENV = None
    if entries.get('CMAKE_GENERATOR', 'Ninja') != 'Ninja':
        raise ValueError('Existing build uses another generator; choose a fresh build directory')
    executable = shutil.which('ninja') or managed_ninja()
    if not executable:
        raise ValueError('Build runner ninja is missing; run spite setup')
    return ['-G', 'Ninja', '-DCMAKE_MAKE_PROGRAM=' + executable, '-DCMAKE_CXX_COMPILER=g++-14']


def checked_file(root, relative):
    path = Path(relative)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise ValueError(f"Expected a relative file: {relative}")
    source = (root / path).resolve(strict=True)
    if not source.is_relative_to(root.resolve()) or not source.is_file():
        raise ValueError(f"File escapes selected root: {relative}")
    return source


def copy_file(root, relative, target):
    source = checked_file(root, relative)
    dest = target / relative
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, dest)


def check_runtime_content(relative):
    p = Path(relative)
    if p.parts[0] not in ("assets", "scenes", "shaders") and p.parts[:2] != ("config", "project") and relative != "input_mappings.json":
        raise ValueError(f"Unsupported runtime content root: {relative}")
    if p.parts[:2] == ("shaders", "source"):
        raise ValueError("Ship compiled shaders, not shader sources")
    if p.suffix.lower() in (".cpp", ".hpp", ".h", ".c", ".glb", ".gltf", ".fbx", ".slang", ".py", ".pdb"):
        raise ValueError(f"Authoring or debug file is not runtime content: {relative}")


def binary_scene_entities(data):
    # AudioEmitterComponent's persisted schema hash protects the positional clip field.
    audio_hash = 0xcbf29ce484222325
    for name, kind in (("clip", 14), ("volume", 6), ("playing", 0), ("loop", 0), ("spatial", 0), ("stream", 0)):
        for byte in name.encode() + bytes((0, kind)) + bytes(4) + bytes(1):
            audio_hash = ((audio_hash ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    pos = 0
    def take(size):
        nonlocal pos
        if size > len(data) - pos:
            raise ValueError("truncated binary scene")
        result = data[pos:pos + size]
        pos += size
        return result
    def u32():
        return struct.unpack("<I", take(4))[0]
    def u64():
        return struct.unpack("<Q", take(8))[0]
    if take(4) != b"SPSD" or u32() != 2:
        raise ValueError("unsupported binary scene format")
    count = u32()
    if count > (len(data) - pos) // 16:
        raise ValueError("invalid binary scene entity count")
    entities = []
    for _ in range(count):
        entity_id = u64()
        u32()
        components = u32()
        if components > (len(data) - pos) // 16:
            raise ValueError("invalid binary scene component count")
        for _ in range(components):
            name = take(u32()).decode("utf-8")
            layout = u64()
            payload = take(u32())
            if name != "AudioEmitterComponent":
                continue
            if layout != audio_hash or len(payload) < 12:
                raise ValueError(f"entity {entity_id}: incompatible audio emitter layout; save scene as JSON")
            size = struct.unpack_from("<I", payload)[0]
            if size + 12 != len(payload):
                raise ValueError(f"entity {entity_id}: invalid audio emitter payload")
            clip = payload[4:4 + size].decode("utf-8")
            entities.append({"id": entity_id, "components": {"AudioEmitterComponent": {"clip": clip}}})
    return entities


def package_scene_audio(project, stage, spec):
    audio_root = project / "assets/audio"
    copied = set()
    for relative in spec.get("files", []):
        path = Path(relative)
        if path.parts[:2] == ("assets", "audio"):
            copied.add(Path(*path.parts[2:]))

    scenes = set()
    for relative in spec.get("files", []):
        path = Path(relative)
        if path.parts and path.parts[0] == "scenes":
            scenes.add(path)
        elif path.suffix.lower() == ".spsd":
            scenes.add(path)
        elif path.suffix.lower() == ".json":
            try:
                document = read(checked_file(stage, path))
                if isinstance(document, dict) and isinstance(document.get("entities"), list):
                    scenes.add(path)
            except (OSError, ValueError):
                pass
    if spec.get("scene"):
        scenes.add(Path(spec["scene"]))
    for path in stage.rglob("*.json"):
        relative = path.relative_to(stage)
        if relative.parts[0] == "scenes":
            scenes.add(relative)

    for scene in sorted(scenes):
        scene_file = checked_file(stage, scene)
        try:
            data = scene_file.read_bytes()
            if data[:4] == b"SPSD" or scene.suffix.lower() == ".spsd":
                entities = binary_scene_entities(data)
            else:
                entities = read(scene_file)["entities"]
            if not isinstance(entities, list):
                raise ValueError("entities must be an array")
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise ValueError(f"Scene {scene}: cannot inspect scene audio: {error}") from error
        for index, entity in enumerate(entities):
            if not isinstance(entity, dict):
                raise ValueError(f"Scene {scene}: entity {index} is not an object")
            components = entity.get("components", {})
            if not isinstance(components, dict):
                continue
            emitter = components.get("AudioEmitterComponent")
            if not isinstance(emitter, dict):
                continue
            clip = emitter.get("clip", "")
            if clip == "" or clip is None:
                continue
            context = f"Scene {scene}, entity {entity.get('id', index)}, clip {clip!r}"
            if not isinstance(clip, str):
                raise ValueError(f"{context}: clip must be a path string")
            parts = clip.replace("\\", "/").split("/")
            path = Path(clip)
            windows_path = PureWindowsPath(clip)
            if (path.is_absolute() or windows_path.drive or windows_path.root or
                    any(part in (".", "..", "") for part in parts) or "\x00" in clip or
                    path.suffix.lower() not in (".wav", ".mp3", ".flac")):
                raise ValueError(f"{context}: expected a confined WAV, MP3 or FLAC path relative to assets/audio")
            try:
                if not audio_root.resolve(strict=True).is_relative_to(project.resolve()):
                    raise ValueError("audio root escapes the project")
                source = checked_file(audio_root, path)
            except (OSError, ValueError) as error:
                raise ValueError(f"{context}: audio file missing or outside assets/audio: {error}") from error
            if path not in copied:
                destination = stage / "assets/audio" / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
                copied.add(path)


def publish_directory(stage, output):
    if os.name == "nt":
        stage.rename(output)
        return
    # POSIX rename can replace an empty directory created after the initial check.
    # Linux/glibc's no-replace rename makes the final commit exclusive as well.
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(-100, os.fsencode(stage), -100, os.fsencode(output), 1):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(output))


def installer_path():
    return Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft Visual Studio/Installer/setup.exe"


def compiler_plan(pin):
    """Cheapest way to a usable Windows compiler: none (accepted toolset exists), modify a VS, or install Build Tools."""
    instances = vswhere_instances()
    for path in instances:
        if installed_toolsets(path, minimum_toolset(pin)):
            return {"action": "none", "installPath": str(path)}
    return {"action": "modify", "installPath": str(instances[0])} if instances else {"action": "install"}


def linux_missing():
    missing = [] if shutil.which("g++-14") else ["g++-14"]
    try:
        ctypes.CDLL("libvulkan.so.1")
    except OSError:
        missing.append("libvulkan1")
    return missing


def compiler_detected():
    """Fast check: is a usable compiler present? (The compile-and-load probe is separate.)"""
    if os.name == "nt":
        action = compiler_plan(read(SDK / "internal/CompilerSetup.json"))["action"]
        minimum = minimum_toolset(read(SDK / "internal/CompilerSetup.json"))
        return action == "none", f"MSVC {minimum} or newer " + ("found" if action == "none" else "is missing")
    missing = linux_missing()
    return not missing, "g++-14 and libvulkan1 found" if not missing else "missing: " + ", ".join(missing)


def is_admin():
    return os.name != "nt" or bool(ctypes.windll.shell32.IsUserAnAdmin())


def has_interactive_desktop():
    """False for SSH, services, CI and session 0: nobody can see or approve a UAC prompt there."""
    if os.name != "nt":
        return True
    user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
    session = ctypes.c_ulong()
    name = ctypes.create_unicode_buffer(256)
    needed = ctypes.c_ulong()
    kernel32.ProcessIdToSessionId(os.getpid(), ctypes.byref(session))
    named = user32.GetUserObjectInformationW(user32.GetProcessWindowStation(), 2, name, ctypes.sizeof(name), ctypes.byref(needed))
    return bool(named) and name.value == "WinSta0" and session.value != 0


def installer_command(executable, arguments):
    return subprocess.list2cmdline([str(executable), *map(str, arguments)])


def run_elevated(executable, arguments):
    """One UAC prompt through PowerShell's runas; waits for the installer and returns its exit code."""
    if is_admin():
        return subprocess.run([str(executable), *map(str, arguments)]).returncode
    manual = f"run this in an administrator terminal, then run spite doctor:\n  {installer_command(executable, arguments)}"
    if not has_interactive_desktop():
        raise ValueError("There is no interactive desktop for the administrator prompt (SSH, service or CI session); " + manual)
    if any('"' in str(a) or str(a).endswith("\\") for a in arguments):
        raise ValueError("Installer argument contains unsupported quoting or a trailing separator")
    print("An administrator prompt appears on the screen: the person at the computer must approve it. "
          "Waiting for the installation to finish.", flush=True)
    quoted = " ".join('"' + str(a) + '"' for a in arguments).replace("'", "''")
    command = (f"$p = Start-Process -FilePath '{str(executable).replace(chr(39), chr(39) * 2)}' -ArgumentList '{quoted}' "
               "-Verb RunAs -Wait -PassThru; exit $p.ExitCode")
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command], capture_output=True, text=True)
    if result.returncode and "canceled" in result.stderr.lower():
        raise ValueError("Administrator approval was declined; run spite setup again and approve the prompt")
    if result.returncode and result.stderr.strip() and "Start-Process" in result.stderr:
        raise ValueError(f"The administrator prompt could not be shown ({result.stderr.strip().splitlines()[0]}); " + manual)
    return result.returncode


def check_layout(layout):
    if not (Path(layout) / "Catalog.json").is_file() or not (Path(layout) / "ChannelManifest.json").is_file():
        raise ValueError("Offline layout is incomplete: Catalog.json and ChannelManifest.json are required")


def layout_arguments(layout):
    """Installer switches that take everything from an offline layout (install and modify)."""
    channel = str(Path(layout) / "ChannelManifest.json")
    return ["--noWeb", "--noUpdateInstaller", "--channelUri", channel, "--installChannelUri", channel,
            "--installCatalogUri", str(Path(layout) / "Catalog.json")]


def fetch_bootstrapper(pin, layout):
    if layout:
        path = Path(layout) / "vs_BuildTools.exe"
        check_layout(layout)
    else:
        path = Toolchain.cache_root() / "compiler" / pin["productVersion"] / "vs_BuildTools.exe"
        if not path.is_file():
            path.parent.mkdir(parents=True, exist_ok=True)
            partial = path.with_suffix(".download")
            with urllib.request.urlopen(pin["bootstrapperUrl"], timeout=90) as response, partial.open("wb") as output:
                shutil.copyfileobj(response, output)
            partial.replace(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != pin["sha256"].lower():
        path.unlink() if not layout else None
        raise ValueError("Compiler bootstrapper SHA-256 mismatch")
    signature = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                                f"$s = Get-AuthenticodeSignature -LiteralPath '{path}'; "
                                "if ($s.Status -ne 'Valid' -or $s.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') { exit 1 }"])
    if signature.returncode:
        raise ValueError("Compiler bootstrapper must have a valid Microsoft Authenticode signature")
    return path


def compiler_commands(pin, plan, layout=None):
    """(executable, arguments) of the elevated Windows step for a plan, or None."""
    components = [c for c in pin["components"] if not c.endswith("Workload.VCTools")]
    if plan["action"] == "none":
        return None
    if plan["action"] == "modify":
        if layout:
            check_layout(layout)
        return installer_path(), ["modify", "--installPath", plan["installPath"], "--add",
                                   "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                   *sum((["--add", c] for c in components if "VC.Tools" not in c), []),
                                   "--passive", "--wait", "--norestart", *(layout_arguments(layout) if layout else [])]
    arguments = ["--passive", "--wait", "--norestart", "--installPath",
                 str(Path(os.environ.get("ProgramFiles", "C:/Program Files")) / ("SpiteSdk/BuildTools-" + pin["productVersion"]))]
    arguments += sum((["--add", c] for c in pin["components"]), [])
    return fetch_bootstrapper(pin, layout), arguments + (layout_arguments(layout) if layout else [])


INSTALLER_PROCESSES = ("vs_installer.exe", "vs_installershell.exe", "vs_buildtools.exe", "vs_setup_bootstrapper.exe",
                       "vs_community.exe", "vs_professional.exe", "vs_enterprise.exe")


def installer_rows():
    """(identified installer process names, hidden-path setup.exe count) of the processes running now."""
    script = "Get-CimInstance Win32_Process | ForEach-Object { $_.Name + '|' + $_.ExecutablePath + '|' + $_.ProcessId + '|' + $_.ParentProcessId }"
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", script], capture_output=True, text=True)
    processes = [(line.split("|") + ["", "", ""])[:4] for line in result.stdout.splitlines() if line.strip()]
    names = {pid: name.lower() for name, _, pid, _ in processes}
    found, hidden = [], 0
    for name, path, _, parent in processes:
        if name.lower() in INSTALLER_PROCESSES:
            found.append(name)
        elif name.lower() == "setup.exe":
            if "microsoft visual studio" in path.lower() and "installer" in path.lower() or names.get(parent, "").startswith("vs_"):
                found.append(name)
            elif not path.strip():
                hidden += 1
    return found, hidden


def running_installers():
    """Names of Visual Studio installer processes running now, whoever started them and elevated or not: vs_* installer
    processes, and a setup.exe whose path is under Visual Studio/Installer or whose parent is a vs_* process (an elevated
    setup.exe hides its path from this one, its parent's name stays readable)."""
    return installer_rows()[0]


def warn_about_hidden_setups(rows=None):
    """A setup.exe with a hidden path and no vs_* parent may be an elevated Visual Studio installation or any program;
    no signal links it to Visual Studio across elevation, so setup warns once and goes on rather than block on a guess."""
    hidden = installer_rows()[1] if rows is None else rows
    if hidden:
        print("Another setup.exe is running and cannot be identified; if it is a Visual Studio installation, wait for it "
              "to finish (or cancel this and run spite setup again later).", flush=True)
    return hidden


def wait_for_installers(wait=True, sleep=None, limit=3600, interval=30):
    """True when no Visual Studio installer is running (after waiting for it, when `wait`)."""
    sleep = sleep or time.sleep
    running = running_installers()
    if not running:
        return True
    print(f"A Visual Studio installation is already running ({', '.join(sorted(set(running)))}); "
          "spite setup never starts a second one.", flush=True)
    if not wait:
        print("Wait for it to finish, then run spite doctor.")
        return False
    print("Waiting for it to finish (up to " + str(limit // 60) + " minutes).", flush=True)
    for elapsed in range(interval, limit + interval, interval):
        sleep(interval)
        if not running_installers():
            print("The other installation finished.")
            return True
        print(f"  still running after {elapsed // 60} min {elapsed % 60} s", flush=True)
    raise ValueError("The running Visual Studio installation did not finish in time; wait for it, then run spite doctor")


def windows_compiler_setup(layout=None, elevate=None, wait=True):
    pin = read(SDK / "internal/CompilerSetup.json")
    if not wait_for_installers(wait):
        return 1
    plan = compiler_plan(pin)
    minimum = minimum_toolset(pin)
    if plan["action"] != "none":
        warn_about_hidden_setups()
    if plan["action"] == "none":
        print(f"MSVC {minimum} or newer is already installed ({plan['installPath']}); nothing to install.")
        return 0
    print("Installing the Visual Studio C++ toolset and Windows SDK (about 1-3 GB; one administrator approval)."
          if plan["action"] == "modify" else
          "Installing Microsoft Visual Studio Build Tools (about 1-3 GB; one administrator approval). Build Tools are "
          "licensed by Microsoft under the Visual Studio license terms; the installer downloads them from Microsoft.")
    print("It takes 5-20 minutes. If your command has a shorter time limit, run spite setup in the background: the "
          "installer keeps running on its own, and running spite setup again waits for it.", flush=True)
    executable, arguments = compiler_commands(pin, plan, layout)
    code = (elevate or run_elevated)(executable, arguments)
    if code in (3010, 1641):
        print("Installed; restart Windows, then run spite doctor.")
        return 3010
    if code:
        raise ValueError(f"Compiler installer failed or was cancelled (exit {code}); see %TEMP%\\dd_bootstrapper*, dd_setup*")
    return 0


def linux_compiler_setup(runner=subprocess.run, release_file="/etc/os-release"):
    missing = linux_missing()
    if not missing:
        print("g++-14 and libvulkan1 are already installed; nothing to install.")
        return 0
    release = {}
    try:
        for line in Path(release_file).read_text(encoding="utf-8").splitlines():
            key, _, value = line.partition("=")
            release[key] = value.strip('"')
    except OSError:
        pass
    family = {release.get("ID", "")} | set(release.get("ID_LIKE", "").split())
    if family & {"debian", "ubuntu"}:
        command = ["apt-get", "install", "-y", *missing]
    else:
        print("Automatic compiler setup is unavailable on this distribution. Use an Ubuntu 24.04 "
              "environment with g++-14 and the Vulkan loader, matching the SDK's exact compiler and "
              "libstdc++ toolset stamp; then run spite doctor.")
        return 1
    root = getattr(os, "geteuid", lambda: 1)() == 0
    can_sudo = root or runner(["sudo", "-n", "true"], capture_output=True).returncode == 0
    if command[0] != "apt-get" or not can_sudo:
        print("Run this, then spite doctor:\n  sudo " + " ".join(command))
        return 1
    print("Running: sudo " + " ".join(command))
    code = runner(command if root else ["sudo", *command]).returncode
    if code:
        raise ValueError(f"{command[0]} failed ({code})")
    return 0


def setup(layout=None, offline_tools=None, elevate=None, wait=True):
    """Install tier-1 tools (no admin), then the compiler by the cheapest path, then prove it with doctor."""
    for name, entry in Toolchain.tools("build", True, offline_tools or os.environ.get("SPITE_OFFLINE_TOOLS")).items():
        print(f"{name} {entry['version']}: {entry['path']}")
    code = windows_compiler_setup(layout, elevate, wait) if os.name == "nt" else linux_compiler_setup()
    if code:
        return code
    return 0 if doctor("build")["ok"] else 1


def compiler_fix(needs_admin=True, incompatible=False):
    if os.name != "nt" and incompatible:
        return {"command": "Inspect the compiler-abi error and use a toolset matching the SDK exact stamp, then run spite doctor.",
                "manual": "Use the exact compiler and libstdc++ toolset stamp shown by doctor on the Ubuntu 24.04 baseline.",
                "needsAdmin": False, "elevation": "manual toolset selection"}
    if not needs_admin:
        return {"command": "spite setup", "needsAdmin": False,
                "elevation": "downloads Python, CMake and Ninja; no administrator approval needed"}
    if os.name == "nt":
        manual = f'powershell -NoProfile -ExecutionPolicy Bypass -File "{SDK / "Setup.ps1"}" -Install'
    else:
        manual = "On Ubuntu 24.04, install the missing prerequisites with apt; other distributions require manual setup matching the exact SDK toolset stamp."
    return {"command": "spite setup", "manual": manual, "needsAdmin": True,
            "elevation": "asks for administrator approval once (Linux: sudo); doctor never elevates"}


def require_ready():
    """build's fast pre-check: fail with the exact setup line instead of a CMake error."""
    try:
        Toolchain.tools("build")
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise ValueError(f"{error}\nRun: spite setup") from error
    ok, detail = compiler_detected()
    if not ok:
        raise ValueError(f"The C++ compiler is not ready ({detail}).\nRun: spite setup")


PROBE_NAME = "SpiteDoctorProbe"
PROBE_SOURCE = """#include "engine/Module.hpp"

namespace
{
    bool load(const spite::ModuleContext&) { return true; }
}

SPITE_MODULE_EXPORT const spite::ModuleDescriptor* SPITE_MODULE_ENTRY()
{
    static const spite::ModuleDescriptor descriptor{.name = "%s", .load = &load};
    return &descriptor;
}
""" % PROBE_NAME


def build_probe_module(scratch):
    """Compiles and links a minimal module against this SDK with the selected compiler; returns the built file."""
    (scratch / "probe.cpp").write_text(PROBE_SOURCE, encoding="utf-8")
    (scratch / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.25)\nproject(Doctor LANGUAGES CXX)\n'
        'find_package(Spite CONFIG REQUIRED PATHS "' + SDK.as_posix() + '/cmake" NO_DEFAULT_PATH)\n'
        f'add_library({PROBE_NAME} MODULE probe.cpp)\nspite_configure_module({PROBE_NAME})\n'
        f'target_link_libraries({PROBE_NAME} PRIVATE Spite::Base Spite::Ecs)\n', encoding="utf-8")
    build_dir = scratch / "build"
    run([cmake(), "-S", scratch, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release", *configure_arguments(build_dir)], scratch, quiet=True)
    run([cmake(), "--build", build_dir, "--config", "Release"], scratch, quiet=True)
    name = ("" if os.name == "nt" else "lib") + PROBE_NAME + (".dll" if os.name == "nt" else ".so")
    built = next((path for path in (build_dir / name, build_dir / "Release" / name) if path.is_file()), None)
    if not built:
        raise ValueError(f"The probe module {name} was not produced by the build")
    return built


def load_probe_module(module, scratch):
    """Loads the module into the SDK's own host, which checks the API and toolset stamps. The host needs a Vulkan device
    (even --headless retains one), so a machine without a usable GPU fails here and not in the compiler check."""
    host = SDK / "bin" / ("SpiteEditor" + (".exe" if os.name == "nt" else ""))
    requests = [{"command": "loadModule", "args": {"path": str(module)}}, {"query": "modules"}, {"command": "shutdown"}]
    command = [str(host), "--headless", "--control", "stdio", "--content-root", str(SDK),
               *map(str, writable_arguments(scratch / "host-state")), "--frames", "600"]
    try:
        result = subprocess.run(command, input="".join(json.dumps(r) + "\n" for r in requests), cwd=scratch,
                                env=clean_env(runtime=True), text=True, capture_output=True, timeout=90)
    except subprocess.TimeoutExpired as error:
        raise ValueError("The SDK host did not finish loading the probe module within 90 seconds") from error
    replies = []
    for line in result.stdout.splitlines():
        try:
            replies.append(json.loads(line)) if line.startswith("{") else None
        except ValueError:
            pass
    if not replies:
        detail = (result.stderr.strip() or result.stdout.strip() or "no output")[-600:]
        raise ValueError(f"The SDK host could not start (exit {result.returncode}); it needs a Vulkan GPU and driver, "
                         f"which spite never installs: {detail}")
    if not replies[0].get("ok"):
        raise ValueError("The SDK host refused the probe module built by this compiler: " + str(replies[0].get("error", replies[0])))
    if PROBE_NAME not in json.dumps(replies[1:2]):
        raise ValueError("The SDK host accepted the probe module but does not list it as loaded")


def doctor(profile, as_json=False, fast=False):
    manifest = read(SDK / "sdk-manifest.json")
    checks = []
    expected = manifest["target"]
    actual = ("windows" if os.name == "nt" else "linux") + "-" + ("x86_64" if platform.machine().lower() in ("amd64", "x86_64") else platform.machine())
    checks.append({"check": "target", "ok": actual == expected, "expected": expected, "actual": actual})
    fix = None
    try:
        checks.append({"check": "tools", "ok": True, "selected": Toolchain.tools(profile)})
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        checks.append({"check": "tools", "ok": False, "error": str(error)})
    if profile == "build" and checks[-1]["ok"]:
        present, detail = compiler_detected()
        checks.append({"check": "compiler", "ok": present, "detail": detail})
        if present and not fast:
            with tempfile.TemporaryDirectory(prefix="spite-doctor-") as scratch:
                module = None
                try:
                    module = build_probe_module(Path(scratch))
                    checks.append({"check": "compiler-abi", "ok": True, "expected": manifest["toolsetStamp"]})
                except (ValueError, OSError, subprocess.SubprocessError) as error:
                    checks.append({"check": "compiler-abi", "ok": False, "expected": manifest["toolsetStamp"], "error": str(error)})
                if module:
                    try:
                        load_probe_module(module, Path(scratch))
                        checks.append({"check": "module-load", "ok": True})
                    except (ValueError, OSError, subprocess.SubprocessError) as error:
                        checks.append({"check": "module-load", "ok": False, "error": str(error)})
    failing = [c for c in checks if not c["ok"] and c["check"] in ("tools", "compiler", "compiler-abi")]
    if failing:
        compiler = any(c["check"] != "tools" for c in failing)
        fix = compiler_fix(compiler, incompatible=os.name != "nt" and any(c["check"] == "compiler-abi" for c in failing))
        failing[-1]["setup"] = (manifest.get("prerequisites", {}).get("compiler", "Run spite setup to install the required compiler toolset.")
                                if compiler else "Run spite setup to download the missing build tools.")
        if os.name != "nt" and failing[-1]["check"] == "compiler-abi":
            failing[-1]["setup"] = fix["manual"]
        failing[-1]["fix"] = fix
    try:
        version = sdk_version()
    except (OSError, ValueError, KeyError):
        version = None
        checks.append({"check": "version", "ok": False, "error": "SDK version record is missing or invalid; reinstall this SDK"})
    result = {"format": 2, "ok": all(c["ok"] for c in checks), "profile": profile, "probe": "skipped" if fast else "run",
              "sdk": {"version": version, "root": str(SDK), "target": expected}, "checks": checks, "fix": fix}
    if as_json:
        print(json.dumps(result, indent=2))
    else:
        for c in checks:
            print(("ok    " if c["ok"] else "FAIL  ") + c["check"] + ("" if c["ok"] else ": " + c.get("error", c.get("detail", ""))))
            if not c["ok"] and "expected" in c:
                print("      expected: " + str(c["expected"]))
        print("SPITE " + str(version) + (": ready" if result["ok"] else ": not ready") + (" (compile probe skipped)" if fast and result["ok"] else ""))
        if fix:
            print("Fix (" + fix["elevation"] + "):\n  " + fix["command"])
    return result


def sdk_version():
    path = SDK / "ReleaseVersion.json"
    version = read(path).get("version") if path.is_file() else read(SDK / "sdk-manifest.json").get("version")
    return ReleaseVersion.SemVer(version).text


def find_project(start=None):
    start = Path(start or Path.cwd()).resolve()
    for directory in (start, *start.parents):
        if (directory / ".spite/sdk.json").is_file():
            return directory
    raise ValueError("Not inside a SPITE project: no .spite/sdk.json found; run 'spite new <path>' first")


def make_pin(manifest):
    return {"format": 2, "version": sdk_version(), "target": manifest["target"], "sourceHead": manifest["sourceHead"],
            "apiStamp": manifest["apiStamp"], "toolsetStamp": manifest["toolsetStamp"]}


def select_project(project):
    project = project.resolve(strict=True)
    record = project / ".spite/sdk.json"
    if not record.is_file():
        raise ValueError("Not an SDK project: missing .spite/sdk.json")
    manifest = read(SDK / "sdk-manifest.json")
    source_lock = source_lock_for_sdk()
    shutil.copy2(record, record.with_name("sdk.json.bak"))
    previous = read(record)
    write(record, {**make_pin(manifest), **({"tools": previous["tools"]} if "tools" in previous else {})})
    (project / ".spite/sdk-root").unlink(missing_ok=True)
    if source_lock:
        write(project / '.spite/source-lock.json', source_lock)
    print(f"Pinned {project.name} to SPITE {sdk_version()}; previous pin saved as .spite/sdk.json.bak. Rebuild native modules.")


def upgrade(project):
    project = project.resolve(strict=True)
    managed = ProjectSkills.read_manifest(project)
    name = managed.get('projectName', module_identifier(project.name))
    select_project(project)
    if (project / 'CMakeLists.txt').is_file() or read(project / 'ship.json').get('modules'):
        build(project)
    else:
        prepare_run_root(project)
    report = ProjectSkills.refresh(project, SDK / 'skills', project_instructions(name), project_name=name)
    for path in report['preserved']:
        print(f'Preserved agent file (edited, linked or user-owned): {path}')


def check_project_sdk(project):
    """Verify the project's pin names the running SDK; migrate a format-1 pin."""
    record_path = project / ".spite/sdk.json"
    record = read(record_path)
    manifest = read(SDK / "sdk-manifest.json")
    identity = ("sourceHead", "apiStamp", "toolsetStamp")
    if record.get("format") != 2:
        if (Path(record.get("sdkRoot", "")).resolve() != SDK or
                any(record.get(key) != manifest[key] for key in identity)):
            raise ValueError("Project pin predates the global 'spite' and names another SDK; "
                             "run 'spite upgrade' in the project to pin the SDK you want")
        shutil.copy2(record_path, record_path.with_name("sdk.json.bak"))
        write(record_path, {**make_pin(manifest), **({"tools": record["tools"]} if "tools" in record else {})})
        (project / ".spite/sdk-root").unlink(missing_ok=True)
        print(f"note: migrated .spite/sdk.json to format 2 (SPITE {sdk_version()}); old pin saved as .spite/sdk.json.bak",
              file=sys.stderr)
        return
    if record.get("version") != sdk_version():
        raise ValueError(f"Project pins SPITE {record.get('version')} but this is SPITE {sdk_version()}; "
                         "run the command through 'spite' (it selects the pinned version), or 'spite upgrade'")
    if any(record.get(key) != manifest[key] for key in identity):
        raise ValueError(f"SPITE {sdk_version()} is installed with different identity stamps than the project pinned; "
                         "the installation was modified; reinstall that version")


def source_sdk_metadata():
    manifest = read(SDK / 'sdk-manifest.json')
    required = ('sourceCompatibility', 'sourceIdentity', 'publicSource', 'sourceManifestSha256',
                'sourceRuntimeFiles', 'sdkBinaryFiles', 'gpuContractFiles', 'sourceSnapshotFiles')
    if any(key not in manifest for key in required) or not (SDK / 'ReleaseVersion.json').is_file():
        raise ValueError('SDK source modules: migration pending; compatibility and provenance metadata are missing')
    version_record = read(SDK / 'ReleaseVersion.json')
    if version_record.get('format') != 1 or not isinstance(version_record.get('version'), str):
        raise ValueError('SDK source modules: invalid release version record')
    ReleaseVersion.SemVer(version_record['version'])
    headers = manifest.get('supportedHeaderFiles')
    if not isinstance(headers, dict) or not headers:
        raise ValueError('SDK source modules: supported header inventory is missing')
    for name, digest in headers.items():
        if not isinstance(name, str) or not name.startswith('include/spite/') or not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest) or SourceModules._hash(SourceModules._path(SDK, name)) != digest:
            raise ValueError('SDK source modules: supported header identity mismatch')
    public = manifest['publicSource']
    local = (manifest.get('distribution') in ('local-unverified-subset', 'local-unverified') and isinstance(public, dict) and
             public.get('status') == 'pending' and public.get('exportIdentity') == manifest['sourceIdentity'] and
             re.fullmatch(r'[0-9a-f]{64}', str(public.get('exportIdentity', ''))) and
             'repository' not in public and 'revision' not in public)
    canonical = (isinstance(public, dict) and public.get('status') == 'canonical' and
                 re.fullmatch(r'https://[^/@\s]+/[^\s?#]+', str(public.get('repository', ''))) and
                 re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', str(public.get('revision', ''))))
    if not local and not canonical:
        raise ValueError('SDK source modules: migration pending; canonical source provenance is missing')
    identity = ReleaseVersion.exact_identity(manifest['sourceCompatibility'])
    if (identity['target'] != manifest.get('target') or identity['configuration'] != manifest.get('configuration') or
        identity['apiStamp'] != manifest.get('apiStamp') or identity['toolsetStamp'] != manifest.get('toolsetStamp')):
        raise ValueError('SDK source modules: native identity disagrees with the SDK')
    if not all(re.fullmatch(r'[0-9a-f]{64}', identity[key]) for key in ('sdkBuildId', 'gpuContractId')):
        raise ValueError('SDK source modules: invalid exact native identity')
    binary_files, gpu_files = manifest['sdkBinaryFiles'], manifest['gpuContractFiles']
    if not isinstance(binary_files, dict) or not isinstance(gpu_files, dict):
        raise ValueError('SDK source modules: invalid SDK identity inventories')
    actual, binaries, gpu = SourceModules.compatibility_from_files(SDK, identity['target'], identity['configuration'],
                                                                  identity['apiStamp'], identity['toolsetStamp'],
                                                                  list(binary_files), list(gpu_files))
    if actual != identity or binaries != binary_files or gpu != gpu_files:
        raise ValueError('SDK source modules: SDK binaries or GPU contract do not match their identity')
    path = SourceModules._path(SDK, 'open-source-manifest.json')
    if SourceModules._hash(path) != manifest['sourceManifestSha256']:
        raise ValueError('SDK source modules: approved source manifest identity mismatch')
    source_manifest = SourceModules.for_target(read(path), identity['target'])
    runtime = manifest['sourceRuntimeFiles']
    if not isinstance(runtime, list) or not runtime or len(runtime) != len(set(runtime)) or any(not isinstance(p, str) or not p.startswith('bin/') or p not in binary_files for p in runtime):
        raise ValueError('SDK source modules: invalid runtime inventory')
    if not isinstance(manifest['sourceSnapshotFiles'], dict) or not manifest['sourceSnapshotFiles']:
        raise ValueError('SDK source modules: migration pending; source snapshot inventory is missing')
    for name, digest in manifest['sourceSnapshotFiles'].items():
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest) or SourceModules._hash(SourceModules._path(SDK, name)) != digest:
            raise ValueError('SDK source modules: stock source snapshot identity mismatch')
    return manifest, source_manifest, identity, runtime, version_record['version']


def source_project_context(project):
    project = Path(project).resolve(strict=True)
    check_project_sdk(project)
    manifest, sources, identity, runtime, version = source_sdk_metadata()
    lock = project / '.spite/source-lock.json'
    if not lock.is_file():
        raise ValueError('SDK source modules: project has no exact source lock; run spite upgrade')
    ReleaseVersion.check_native_lock(read(lock), identity, manifest['sourceIdentity'], version=version)
    return project, manifest, sources, identity, runtime, version


def source_lock_for_sdk():
    manifest = read(SDK / 'sdk-manifest.json')
    if 'sourceCompatibility' not in manifest:
        return None
    manifest, _, identity, _, version = source_sdk_metadata()
    return ReleaseVersion.make_lock(version, identity, manifest['sourceIdentity'])


def verified_source_revision(source, requested):
    if not isinstance(requested, str) or not re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', requested):
        raise ValueError('Use the full lowercase commit ID of the local source fork')
    source = SourceModules._no_links(source).resolve(strict=True)
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith('GIT_')}
    def git(*args):
        return subprocess.run(['git', '-C', str(source), *args], env=env, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout
    try:
        repo = Path(os.fsdecode(git('rev-parse', '--show-toplevel')).strip()).resolve(strict=True)
        commit = git('rev-parse', '--verify', requested + '^{commit}').decode('ascii').strip()
        if commit != requested:
            raise ValueError('Custom fork revision does not resolve to the requested commit')
        prefix = source.relative_to(repo).as_posix()
        args = ['--literal-pathspecs', 'ls-tree', '-r', '-z', '--full-tree', requested]
        if prefix != '.':
            args += ['--', prefix]
        entries = git(*args)
    except (OSError, subprocess.SubprocessError) as error:
        raise ValueError('Custom source must be in a local Git repository containing the claimed commit') from error
    tracked = {}
    path_prefix = '' if prefix == '.' else prefix + '/'
    for entry in entries.split(b'\0'):
        if not entry:
            continue
        metadata, name = entry.split(b'\t', 1)
        mode, kind, oid = metadata.decode('ascii').split()
        name = os.fsdecode(name)
        if mode not in ('100644', '100755') or kind != 'blob' or not name.startswith(path_prefix):
            raise ValueError('Claimed commit contains unsupported source entries')
        relative = name[len(path_prefix):]
        if not relative or relative in tracked:
            raise ValueError('Invalid source tree in claimed commit')
        tracked[relative] = oid
    files = SourceModules._source_files(source)
    if not tracked or set(tracked) != set(files):
        raise ValueError('Custom source files are not exactly tracked by the claimed commit')
    algorithm = hashlib.sha1 if len(requested) == 40 else hashlib.sha256
    for name, path in files.items():
        data = path.read_bytes()
        blob = algorithm(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
        if blob != tracked[name]:
            raise ValueError('Custom source bytes differ from claimed commit: ' + name)
    return requested


def validate_source_recipe(source, module, spec):
    prefix = 'source/open/' + module + '/'
    for name, digest in spec.get('buildInputs', {}).items():
        if not name.startswith(prefix):
            raise ValueError('Unsupported source build recipe')
        path = SourceModules._path(source, name[len(prefix):])
        if hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != digest:
            raise ValueError('Source build recipe changed; use an approved SDK recipe')
    if prefix + 'CMakeLists.txt' not in spec.get('buildInputs', {}):
        raise ValueError('SDK source modules: migration pending; approved build recipe is missing')
    for name, digest in spec.get('staticDependencies', {}).get('Jolt', {}).get('headers', {}).items():
        path = SourceModules._path(SDK, name)
        if hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != digest:
            raise ValueError('Pinned Jolt header changed')
    for key, digest in spec.get('vendorInputs', {}).items():
        if key not in SourceModules.VENDOR_INPUTS:
            raise ValueError('Unsupported SDK vendor key')
        path = SourceModules._path(SDK, SourceModules.VENDOR_INPUTS[key][1])
        if hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != digest:
            raise ValueError('Pinned SDK vendor input changed')
    for entry in spec.get('externalInputs', {}).values():
        staged = entry.get('stagedPath', '')
        if not staged.startswith(prefix):
            raise ValueError('Unsupported external source input')
        path = SourceModules._path(source, staged[len(prefix):])
        if hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != entry.get('sha256'):
            raise ValueError('Pinned external source input changed')


def source_builder(module, spec):
    def build_source(sdk, source, build_dir, target, identity):
        validate_source_recipe(source, module, spec)
        SourceModules.validate_source_inputs(sdk, source, module, spec)
        deps = os.environ.get('SPITE_SDK_DEPS_ROOT')
        vcpkg = os.environ.get('VCPKG_ROOT')
        if not deps or not vcpkg or not Path(deps).is_dir() or not (Path(vcpkg) / 'scripts/buildsystems/vcpkg.cmake').is_file():
            raise ValueError('Pre-provisioned SPITE_SDK_DEPS_ROOT and VCPKG_ROOT are required')
        if Path(deps).resolve().is_relative_to(Path(sdk).resolve()):
            raise ValueError('SDK dependencies must be provisioned outside the immutable SDK')
        wrapper = os.environ.get('SPITE_SDK_BUILD_WRAPPER')
        if not wrapper or not Path(wrapper).is_file() or Path(wrapper).name != 'Build.ps1':
            raise ValueError('SDK source modules: migration pending; set SPITE_SDK_BUILD_WRAPPER to the provisioned Build.ps1')
        project = next((parent for parent in Path(source).resolve().parents if (parent / '.spite/sdk.json').is_file()), None)
        if project is None:
            raise ValueError('Source must be inside its selected project')
        pwsh = shutil.which('pwsh')
        if not pwsh:
            raise ValueError('PowerShell is required for the machine-limited source build')
        result = subprocess.run([pwsh, '-NoProfile', '-File', str(Path(wrapper).resolve()),
                                 '-SdkConsumerRoot', str(sdk), '-SdkConsumerSource', str(source),
                                 '-SdkConsumerBuild', str(build_dir), '-SdkConsumerTarget', target,
                                 '-SdkConsumerProject', str(project)], cwd=project, check=False)
        if result.returncode:
            raise ValueError(f'SDK source build failed ({result.returncode})')
    return build_source


def source_artifact_validator(path, artifact, identity):
    if artifact['kind'] == 'shader':
        data = Path(path).read_bytes()
        if len(data) < 20 or len(data) % 4 or struct.unpack_from('<I', data)[0] != 0x07230203:
            raise ValueError('Invalid custom SPIR-V artifact')
        return
    if artifact['kind'] != 'binary' or not isinstance(artifact.get('gpuSensitive'), bool):
        raise ValueError('SDK source modules: migration pending; native artifact contract is missing')
    exports = artifact.get('requiredExports')
    if not isinstance(exports, list) or not exports or not all(isinstance(name, str) and name and not re.search(r'\s', name) for name in exports):
        raise ValueError('SDK source modules: migration pending; required native exports are missing')
    data = Path(path).read_bytes()
    if identity['target'] == 'windows-x86_64':
        if len(data) < 64 or data[:2] != b'MZ':
            raise ValueError('Custom artifact is not an x64 Windows DLL')
        offset = struct.unpack_from('<I', data, 0x3c)[0]
        if offset + 24 > len(data) or data[offset:offset + 4] != b'PE\0\0' or struct.unpack_from('<H', data, offset + 4)[0] != 0x8664 or not (struct.unpack_from('<H', data, offset + 22)[0] & 0x2000):
            raise ValueError('Custom artifact is not an x64 Windows DLL')
    elif identity['target'] == 'linux-x86_64':
        if len(data) < 20 or data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<HH', data, 16) != (3, 62):
            raise ValueError('Custom artifact is not an x86-64 Linux shared object')
    else:
        raise ValueError('Unsupported SDK native target')
    api = ('SPITE_SDK_API_STAMP=' + identity['apiStamp'] + 'ull').encode('ascii') + b'\0'
    toolset = identity['toolsetStamp'].encode('ascii') + b'\0'
    embedded = (re.findall(rb'msvc\d+-m[td]-idl\d-debug\d(?:-asan)?\0', data) if identity['target'] == 'windows-x86_64' else [])
    if api not in data or not (toolset in data or any(
            toolset_stamps_compatible(stamp[:-1].decode('ascii'), identity['toolsetStamp']) for stamp in embedded)):
        raise ValueError('SDK source modules: migration pending; native artifact lacks matching embedded API and toolset evidence')
    if artifact['gpuSensitive'] and ('SPITE_GPU_CONTRACT_ID=' + identity['gpuContractId']).encode('ascii') + b'\0' not in data:
        raise ValueError('SDK source modules: migration pending; native GPU contract evidence is missing')
    stock = SourceModules._path(SDK, artifact['runtime'])
    if identity['target'] == 'windows-x86_64':
        env = windows_build_environment(read(SDK / 'internal/CompilerSetup.json'))
        dumpbin = Path(env['VCTOOLSINSTALLDIR']) / 'bin/Hostx64/x64/dumpbin.exe'
        if not dumpbin.is_file():
            raise ValueError('Pinned MSVC dumpbin is required to validate source modules')
        def inspect(file, option):
            return subprocess.run([str(dumpbin), option, str(file)], env=env, check=True, text=True, capture_output=True).stdout
        exported = set(re.findall(r'^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)', inspect(path, '/exports'), re.MULTILINE))
        def imports(file):
            return {item.casefold() for item in re.findall(r'^\s+(\S+\.dll)\s*$', inspect(file, '/dependents'), re.MULTILINE | re.IGNORECASE)}
    else:
        tool = shutil.which('readelf')
        if not tool:
            raise ValueError('readelf is required to validate source modules')
        def inspect(file, option):
            return subprocess.run([tool, option, str(file)], check=True, text=True, capture_output=True).stdout
        exported = set()
        for line in inspect(path, '--dyn-syms').splitlines():
            parts = line.split()
            if len(parts) >= 8 and parts[4] in ('GLOBAL', 'WEAK') and parts[5] in ('DEFAULT', 'PROTECTED') and parts[6] != 'UND':
                exported.add(parts[7].split('@', 1)[0])
        def imports(file):
            return set(re.findall(r'Shared library: \[([^]]+)\]', inspect(file, '-d')))
    if not set(exports) <= exported:
        raise ValueError('Custom artifact is missing required exported symbols')
    if not imports(path) <= imports(stock):
        raise ValueError('Custom artifact adds unapproved runtime library dependencies')


def source_shader_validator(path, artifact, identity):
    data = Path(path).read_bytes()
    if len(data) < 20 or len(data) % 4 or struct.unpack_from('<I', data)[0] != 0x07230203:
        raise ValueError('Invalid custom SPIR-V artifact')
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    cursor = 5
    opcodes = set()
    while cursor < len(words):
        count = words[cursor] >> 16
        if count == 0 or cursor + count > len(words):
            raise ValueError('Invalid custom SPIR-V instruction stream')
        opcodes.add(words[cursor] & 0xffff)
        cursor += count
    if not words[3] or words[4] or not {14, 15, 17} <= opcodes:
        raise ValueError('Custom SPIR-V is missing required module declarations')
    accepted = artifact.get('approvedSpirvSha256')
    if artifact.get('gpuContractId') != identity['gpuContractId'] or not isinstance(accepted, list) or SourceModules._hash(path) not in accepted:
        raise ValueError('SDK source modules: migration pending; custom shader interface validation is unavailable')


def source_copy(project, module, destination):
    project, manifest, sources, _, _, _ = source_project_context(project)
    SourceModules._module(sources, module)
    destination = SourceModules._no_links(destination).resolve()
    if not destination.is_relative_to(project) or destination == project:
        raise ValueError('Choose a new source directory inside the project')
    prefix = 'source/open/' + module + '/'
    expected = {name: digest for name, digest in manifest['sourceSnapshotFiles'].items() if name.startswith(prefix)}
    if not expected:
        raise ValueError('SDK source modules: migration pending; stock source snapshot is missing')
    source = SDK / 'source/open' / module
    actual = {}
    for path in source.rglob('*'):
        if path.is_file() or path.is_symlink():
            name = path.relative_to(SDK).as_posix()
            actual[name] = SourceModules._hash(SourceModules._path(SDK, name))
    if actual != expected:
        raise ValueError('SDK stock source differs from the approved snapshot')
    copied = SourceModules.copy_stock_sources(SDK, destination, 'source/open/' + module)
    try:
        copied_files = {prefix + path.relative_to(copied).as_posix(): SourceModules._hash(SourceModules._path(copied, path.relative_to(copied).as_posix()))
                        for path in copied.rglob('*') if path.is_file()}
        if copied_files != expected:
            raise ValueError('SDK stock source changed while copying')
    except BaseException:
        shutil.rmtree(copied, ignore_errors=True)
        raise
    return copied


def source_build(project, module, source, revision=None):
    project, manifest, sources, identity, _, version = source_project_context(project)
    spec = SourceModules._module(sources, module)
    source = SourceModules._no_links(source).resolve(strict=True)
    if not source.is_relative_to(project) or source == project:
        raise ValueError('Custom source must be in a project-owned module directory')
    provenance = {'kind': 'git', 'revision': verified_source_revision(source, revision)} if revision else {'kind': 'snapshot'}
    build_dir = SourceModules.build_module(SDK, project, sources, identity, module, source,
                                           source_builder(module, spec), source_artifact_validator,
                                           source_shader_validator, source_provenance=provenance)
    try:
        if revision:
            verified_source_revision(source, revision)
        current, _, current_identity, _, current_version = source_sdk_metadata()
        if current_identity != identity or current_version != version or current['sourceIdentity'] != manifest['sourceIdentity']:
            raise ValueError('SDK identity changed during source build')
        record = read(build_dir / 'source-build.json')
        custom = {'provenance': record['sourceProvenance'], 'sourceHash': record['sourceHash'],
                  'outputHashes': {item['output']: item['sha256'] for item in record['artifacts']}}
        write(build_dir / 'source-lock.json', ReleaseVersion.make_lock(version, identity, manifest['sourceIdentity'], custom))
    except BaseException:
        shutil.rmtree(build_dir, ignore_errors=True)
        raise
    return build_dir


def validate_source_build_locks(builds, identity, source_identity, version):
    for build in builds:
        build = Path(build).resolve(strict=True)
        record = read(SourceModules._path(build, 'source-build.json'))
        lock = read(SourceModules._path(build, 'source-lock.json'))
        ReleaseVersion.check_native_lock(lock, identity, source_identity, version=version)
        custom = {'provenance': record['sourceProvenance'], 'sourceHash': record['sourceHash'],
                  'outputHashes': {item['output']: item['sha256'] for item in record['artifacts']}}
        if lock.get('custom') != custom:
            raise ValueError('Custom source or output lock does not match its build record')


def source_select(project, builds):
    project, manifest, sources, identity, runtime, version = source_project_context(project)
    owned = (project / '.spite/source-builds').resolve()
    for build in builds:
        if not SourceModules._no_links(build).resolve(strict=True).is_relative_to(owned):
            raise ValueError('Source builds must be owned by this project')
    validate_source_build_locks(builds, identity, manifest['sourceIdentity'], version)
    return SourceModules.select_modules(SDK, project, sources, identity, runtime, builds,
                                        source_artifact_validator, source_shader_validator)


def source_restore(project):
    project, _, sources, identity, runtime, _ = source_project_context(project)
    return SourceModules.restore_stock(SDK, project, sources, identity, runtime, source_artifact_validator)


RUN_ROOT = "build/spite-content"
ENGINE_TREES = ("assets", "shaders", "config")
PROJECT_TREES = ("assets_baked", "models", "scenes", "assets/audio")


def link_directory(target, link):
    if os.name == "nt":
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        os.symlink(target, link, target_is_directory=True)


def overlay(engine, project, run, mirrors):
    """Merge engine content (copied) with project content (directories linked, files hard-linked or copied)."""
    run.mkdir(parents=True, exist_ok=True)
    names = {p.name for tree in (engine, project) if tree.is_dir() for p in tree.iterdir()}
    for name in sorted(names):
        e, q, target = engine / name, project / name, run / name
        if e.is_dir() and q.is_dir():
            overlay(e, q, target, mirrors)
        elif q.is_dir():
            if not target.exists():
                link_directory(q, target)
        elif q.exists():
            same = target.exists() and os.path.samefile(q, target)
            if not same and not (target.exists() and target.stat().st_mtime > q.stat().st_mtime):
                target.unlink(missing_ok=True)
                try:
                    os.link(q, target)
                except OSError:
                    shutil.copy2(q, target)
            mirrors.append((q, target))
        elif e.is_dir():
            if not target.exists():
                shutil.copytree(e, target)
        elif not target.exists() or (target.stat().st_size, int(target.stat().st_mtime)) != (e.stat().st_size, int(e.stat().st_mtime)):
            shutil.copy2(e, target)


def prepare_run_root(project, modules=True):
    """Content root for a host run: this SDK's engine content overlaid by the project's own files.

    The runtime reads one content root, so engine content is copied into the project's build output
    (never its source tree); project directories are linked so editor writes land in the project.
    HostArgs selects the authoritative project config separately, retaining this root's defaults and module list."""
    run = project / RUN_ROOT
    manifest = read(SDK / "sdk-manifest.json")
    stamp = {"sdk": str(SDK), "version": sdk_version(), "apiStamp": manifest["apiStamp"], "toolsetStamp": manifest["toolsetStamp"]}
    if (run / ".spite-run.json").is_file() and read(run / ".spite-run.json") != stamp:
        shutil.rmtree(run)
    for tree in PROJECT_TREES:
        (project / tree).mkdir(parents=True, exist_ok=True)
    mirrors = []
    for tree in ENGINE_TREES:
        overlay(SDK / tree, project / tree, run / tree, mirrors)
    for tree in PROJECT_TREES:
        if not (run / tree).exists():
            link_directory(project / tree, run / tree)
    write(run / ".spite-run.json", stamp)
    stage_modules(project, run, modules)
    return run, mirrors


def stage_modules(project, run, load):
    """The run root lists the project's built game modules (none for an explicit-load run), unless the project lists its own in config/project/application."""
    if (project / "config/project/application/modules.json").is_file():
        return
    built = []
    if load:
        try:
            built = built_modules(project, read(project / "ship.json").get("modules", []))
        except (OSError, ValueError) as error:
            print(f"note: no game module loaded: {error}", file=sys.stderr)
    target = run / "config/project/application/modules.json"
    target.unlink(missing_ok=True)
    target.parent.mkdir(parents=True, exist_ok=True)
    write(target, {"modules": [str(path.resolve()) for path in built]})


def write_back(mirrors):
    for source, copy in mirrors:
        if copy.is_file() and not os.path.samefile(source, copy) and copy.stat().st_mtime > source.stat().st_mtime:
            shutil.copy2(copy, source)


def project_info(project, mirrors=None):
    project = project.resolve(strict=True)
    if project.is_relative_to(SDK):
        raise ValueError("Keep project content outside the SDK")
    check_project_sdk(project)
    run, pairs = prepare_run_root(project)
    if mirrors is not None:
        mirrors.extend(pairs)
    arguments = ["--content-root", str(run), "--project-config-root", str(project / 'config/project'),
                 *map(str, writable_arguments(project))]
    return {"ProjectRoot": str(project), "SdkRoot": str(SDK), "SdkVersion": sdk_version(), "ContentRoot": str(run),
            "Cli": str(SDK / "bin/spite.exe") if os.name == "nt" else None,
            "HostArgs": arguments}


def module_identifier(text):
    name = re.sub(r"[^A-Za-z0-9_]", "_", text)
    return name if re.match(r"[A-Za-z_]", name) else "Game_" + name


def project_instructions(name):
    return (f"# {name}\n\nA SPITE game project. Use the global `spite` command from this directory: `spite build`, "
            "`spite run`, `spite ship <out-dir>`, `spite doctor`; `spite upgrade` moves the project to the newest "
            "installed SDK. Engine assets and shaders stay in the SDK named by `.spite/sdk.json`; never copy them "
            "here. `spite project` prints `SdkRoot`: read its `AGENTS.md`, `README.md` and `guides/AgentWorkflows.md` "
            "before working.\n\nAgent skills are installed in `.agents/skills/` and `.claude/skills/`. "
            "Use spite-feedback for requested engine feedback. Use the relevant skill for the task. Upgrade refreshes SDK-owned agent files only when unchanged; "
            "edited SDK-owned files are preserved and reported; unrelated user-added files are left alone. "
            "Ownership hashes live in "
            "`.spite/sdk-managed-files.json`.\n")


def new_project(output, name=None, content_only=False):
    output = output.resolve()
    if output.is_relative_to(SDK):
        raise ValueError("Choose a new project directory outside the SDK")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise ValueError("Choose a new project directory (an existing one must be empty)")
    name = name or module_identifier(output.name)
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise ValueError(f"Invalid project name {name!r}: use letters, digits and underscores, not starting with a digit")
    source_lock = source_lock_for_sdk()
    manifest = read(SDK / "sdk-manifest.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".spite-project-", dir=output.parent) as scratch:
        stage = Path(scratch) / "project"
        stage.mkdir()
        if not content_only:
            sample = SDK / "samples/RenderConsumer"
            for template, target in (("CMakeLists.txt", "CMakeLists.txt"), ("RenderConsumer.cpp", name + ".cpp")):
                text = checked_file(sample, template).read_text(encoding="utf-8")
                (stage / target).write_text(text.replace("RenderConsumer", name), encoding="utf-8")
            shutil.copytree(sample / "shaders", stage / "shaders/custom")
        ProjectSkills.refresh(stage, SDK / 'skills', project_instructions(name), project_name=name)
        (stage / ".gitignore").write_text("build/\nuser/\nsaves/\ncache/\nshadows/\ncrashes/\nartifacts/\ncontrol/\n", encoding="utf-8")
        for directory in ("models", "assets/audio", "scenes"):
            (stage / directory).mkdir(parents=True, exist_ok=True)
        write(stage / ".spite/sdk.json", {**make_pin(manifest), "tools": {"python": Toolchain.probe("python", sys.executable)}})
        if source_lock:
            write(stage / ".spite/source-lock.json", source_lock)
        write(stage / "assets_baked/asset_manifest.json", {})
        write(stage / "config/project/input/actions.json", {"actions": []})
        write(stage / "ship.json", {"format": 1, "modules": [] if content_only else [name],
              "files": ["config/project/input/actions.json"] + ([] if content_only else
              ["shaders/bin/sdk_stripe.vert.spv", "shaders/bin/sdk_stripe.frag.spv",
               "shaders/bin/sdk_hdr.vert.spv", "shaders/bin/sdk_hdr.frag.spv"])})
        if output.exists():
            output.rmdir()
        publish_directory(stage, output)
    print(output)


def run_project(project, game=False, control="stdio", host_args=()):
    mirrors = []
    info = project_info(project, mirrors)
    name = ("SpiteGame" if game else "SpiteEditor") + (".exe" if os.name == "nt" else "")
    executable = SDK / 'bin' / name
    if (Path(info['ProjectRoot']) / '.spite/source-selection.json').is_file():
        project, _, sources, identity, runtime, _ = source_project_context(info['ProjectRoot'])
        executable = SourceModules.resolve_run(SDK, project, sources, identity, runtime, 'bin/' + name,
                                               source_artifact_validator, source_shader_validator)
    # Inherit stdin/stdout: agents exchange control JSON with the running host.
    result = subprocess.run([str(executable), *info["HostArgs"], "--control", control, *host_args],
                            cwd=info["ProjectRoot"], env=clean_env())
    write_back(mirrors)
    if result.returncode:
        raise ValueError(f"Host exited with code {result.returncode}")


def desktop_entries(project, output=None):
    if os.name == "nt":
        raise ValueError("Desktop entries are for the native Linux SDK")
    project = project.resolve(strict=True)
    check_project_sdk(project)
    output = output or Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "applications"
    output.mkdir(parents=True, exist_ok=True)

    def value(text):
        return str(text).replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")

    def argument(text):
        escaped = str(text).replace("%", "%%")
        for char in ("\\", '"', "`", "$"):
            escaped = escaped.replace(char, "\\" + char)
        return '"' + value(escaped) + '"'

    identity = hashlib.sha256(os.fsencode(project)).hexdigest()[:12]
    for name in ("SpiteEditor", "SpiteGame", "SpiteBake", "ShaderCompiler"):
        terminal = name in ("SpiteBake", "ShaderCompiler")
        command = [SDK / "bin" / name] if terminal else [
            "sh", SDK / "sdk.sh", "run", project, "--control", "none"] + (["--game"] if name == "SpiteGame" else [])
        entry = output / f"spite-{identity}-{name}.desktop"
        entry.write_text("[Desktop Entry]\nType=Application\nName=" + value(f"{name} — {project.name}") +
                         "\nExec=" + " ".join(map(argument, command)) +
                         "\nPath=" + value(project) + "\nIcon=" + value(SDK / "assets/branding/SpiteShowcases.png") +
                         "\nTerminal=" + str(terminal).lower() + "\nCategories=Development;\n", encoding="utf-8")
        print(entry)


def verify_sdk(output, forbidden=()):
    sys.path.insert(0, str(SDK / "internal"))
    from VerifySdkGame import verify
    verify(output, forbidden)


def build(project):
    project = project.resolve(strict=True)
    check_project_sdk(project)
    require_ready()
    cache = project / "build/CMakeCache.txt"
    configured = [Path(line.split("=", 1)[1]) for line in cache.read_text(encoding="utf-8").splitlines()
                  if line.startswith("SPITE_SDK:")] if cache.is_file() else []
    if configured and configured[0] != SDK:
        cache.unlink()
        shutil.rmtree(project / "build/CMakeFiles", ignore_errors=True)
    args = configure_arguments(project / "build")
    run([cmake(), "-S", project, "-B", project / "build", "-DSPITE_SDK=" + str(SDK), "-DCMAKE_BUILD_TYPE=Release", *args], project)
    run([cmake(), "--build", project / "build", "--config", "Release"], project)
    if (project / "shaders/custom").is_dir():
        run([SDK / ("bin/ShaderCompiler.exe" if os.name == "nt" else "bin/ShaderCompiler"),
             "--sources", project / "shaders/custom", "--output", project / "shaders/bin", "--spirv-only"], project)
    record = read(project / '.spite/sdk.json')
    record['tools'] = Toolchain.tools('build')
    write(project / '.spite/sdk.json', record)
    prepare_run_root(project)


def package_contract():
    path = SDK / "sdk-manifest.json"
    try:
        contract = read(path)
    except (OSError, ValueError) as error:
        raise ValueError(f"Invalid SDK manifest {path}: cannot read JSON; republish or reinstall a complete SDK") from error
    for field in ("sourceHead", "apiStamp", "toolsetStamp", "target"):
        value = contract.get(field) if isinstance(contract, dict) else None
        if not isinstance(value, str) or not value:
            raise ValueError(f"Invalid SDK manifest {path}: {field} must be a nonempty string; "
                             "republish or reinstall a complete SDK")
    for field in ("gameRuntimeFiles", "systemRuntimeLibraries"):
        value = contract.get(field) if isinstance(contract, dict) else None
        if (not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value)
                or (field == "gameRuntimeFiles" and not value)):
            raise ValueError(f"Invalid SDK manifest {path}: {field} must be "
                             f"{'a nonempty' if field == 'gameRuntimeFiles' else 'a'} list of nonempty strings; "
                             "republish or reinstall a complete SDK")
    return contract


def built_modules(project, names):
    prefix, suffix = ("", ".dll") if os.name == "nt" else ("lib", ".so")
    found = []
    for name in names:
        if not name.isidentifier() or Path(name).name != name:
            raise ValueError(f"Invalid module name: {name}")
        file = prefix + name + suffix
        build = project / "build"
        hits = [next((p for p in (build / file, build / "Release" / file) if p.is_file()), None)]
        hits = [p for p in hits if p] or [p for p in build.rglob(file) if "CMakeFiles" not in p.parts]
        if len(hits) != 1:
            raise ValueError(f"Module {name} ({file}) " + ("is built more than once under build/; pass --module" if hits
                                                           else "is not built; run 'spite build' first"))
        found.append(hits[0])
    return found


def package(project, output, modules, smoke):
    project, output = project.resolve(strict=True), output.resolve()
    if output.exists() or output.is_relative_to(project) or output.is_relative_to(SDK):
        raise ValueError("Choose a new output directory outside the SDK and project")
    contract = package_contract()
    check_project_sdk(project)
    spec = read(project / "ship.json")
    if spec.get("format") != 1:
        raise ValueError("ship.json format must be 1")
    if not spec["modules"]:
        raise ValueError("ship.json lists no modules; the modular-game packager needs at least one")
    modules = modules or built_modules(project, spec["modules"])
    if len(modules) != len(spec["modules"]):
        raise ValueError("Provide one --module binary for each ship.json module, in order")
    scene = spec.get("scene")
    if "scene" in spec:
        if not isinstance(scene, str) or not scene or not re.fullmatch(r"[A-Za-z0-9_ ./-]+", scene):
            raise ValueError("ship.json.scene must be a nonempty relative path using supported launcher characters")
        try:
            checked_file(project, scene)
        except FileNotFoundError as error:
            raise ValueError(f"ship.json.scene source {scene} is missing; correct the path or create the file") from error
        except ValueError as error:
            raise ValueError(f"Invalid ship.json.scene {scene}: {error}") from error
        files = spec.get("files", [])
        if not isinstance(files, list) or scene not in files:
            raise ValueError(f"ship.json.scene {scene} is not in ship.json.files; add that exact path")
        try:
            check_runtime_content(scene)
        except ValueError as error:
            raise ValueError(f"ship.json.scene {scene} cannot be staged from ship.json.files; choose supported runtime content: {error}") from error
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".spite-game-", dir=output.parent) as scratch:
        release = Path(scratch) / "release"
        stage = release / "game"
        stage.mkdir(parents=True)
        for relative in contract["gameRuntimeFiles"]:
            copy_file(SDK, relative, stage)
        # The runtime contract includes stock fonts, localization, UI and shaders;
        # project overrides are explicit, never a recursive copy of authoring roots.
        for relative in spec.get("files", []):
            check_runtime_content(relative)
            copy_file(project, relative, stage)
        package_scene_audio(project, stage, spec)
        baked = project / "assets_baked"
        manifest = read(baked / "asset_manifest.json")
        for section in manifest.values():
            if not isinstance(section, dict):
                continue
            for entry in section.values():
                if not isinstance(entry, dict):
                    continue
                entry.pop("source", None)
                entry.pop("authoringSource", None)
                for field in ("baked", "skin"):
                    if not entry.get(field):
                        continue
                    path = Path(entry[field])
                    if path.is_absolute():
                        path = path.resolve(strict=True).relative_to(baked.resolve())
                    elif path.parts[0] == "assets_baked":
                        path = Path(*path.parts[1:])
                    else:
                        raise ValueError(f"Invalid baked path: {path}")
                    copy_file(baked, path, stage / "assets_baked")
                    entry[field] = "assets_baked/" + path.as_posix()
        write(stage / "assets_baked/asset_manifest.json", manifest)
        image_suffix = ".dll" if os.name == "nt" else ".so"
        for name, source in zip(spec["modules"], modules):
            if not name.isidentifier() or Path(name).name != name:
                raise ValueError(f"Invalid module name: {name}")
            dest = stage / "bin" / (name + image_suffix)
            shutil.copy2(source.resolve(strict=True), dest)
            if os.name != "nt":
                dependencies = run(["ldd", dest], stage, release / (name + "-dependencies.log"), runtime=True)
                for line in dependencies.splitlines():
                    fields = line.strip().split()
                    if not fields:
                        continue
                    if "not found" in line:
                        raise ValueError(f"Missing module dependency: {line.strip()}")
                    if Path(fields[0]).name in contract["systemRuntimeLibraries"]:
                        continue
                    if len(fields) < 3 or fields[1] != "=>" or Path(fields[2]).resolve().parent != (stage / "bin").resolve():
                        raise ValueError(f"Module dependency resolves outside the game: {line.strip()}")
        write(stage / "config/project/application/modules.json", {"modules": spec["modules"]})
        exe = stage / "bin" / ("SpiteGame.exe" if os.name == "nt" else "SpiteGame")
        run([exe, "--validate-content", stage], stage, release / "content-validation.log", runtime=True)
        args = [exe, "--content-root", stage, "--control", "none", "--frames", "8"]
        args += writable_arguments(Path(scratch) / "runtime-state")
        if scene:
            checked_file(stage, scene)
            args += ["--scene", stage / scene]
            if not re.fullmatch(r"[A-Za-z0-9_ ./-]+", scene):
                raise ValueError("Startup scene path contains unsupported launcher characters")
        if os.name == "nt":
            scene_arg = (' --scene "%~dp0' + scene.replace('/', '\\') + '"') if scene else ''
            (stage / "Game.cmd").write_text('@echo off\n"%~dp0bin\\SpiteGame.exe" --content-root "%~dp0." --control none' + scene_arg + ' %*\nexit /b %errorlevel%\n', encoding="utf-8")
        else:
            scene_arg = (' --scene "$root"/' + shlex.quote(scene)) if scene else ''
            launcher = stage / "Game.sh"
            launcher.write_text('#!/bin/sh\nroot=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexec "$root/bin/SpiteGame" --content-root "$root" --control none' + scene_arg + ' "$@"\n', encoding="utf-8")
            launcher.chmod(0o755)
        if smoke:
            result = run(args, stage, release / "runtime-smoke.log", runtime=True)
            if f'with {len(modules)} game module(s)' not in result:
                raise ValueError("Runtime did not confirm loading every requested game module")
        write(release / "game-manifest.json", {"format": 1, "target": contract["target"], "apiStamp": contract["apiStamp"], "toolsetStamp": contract["toolsetStamp"], "contentValidation": "passed", "runtimeSmoke": "passed" if smoke else "skipped", "files": {p.relative_to(stage).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(stage.rglob("*")) if p.is_file()}})
        archive = release / ("game.zip" if os.name == "nt" else "game.tar.gz")
        if os.name == "nt":
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
                for path in sorted(stage.rglob("*")):
                    if path.is_file():
                        bundle.write(path, path.relative_to(stage))
        else:
            with tarfile.open(archive, "w:gz") as bundle:
                bundle.add(stage, arcname=".")
        (release / (archive.name + ".sha256")).write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + "  " + archive.name + "\n", encoding="ascii")
        publish_directory(release, output)
    print(output)


def main():
    try:
        manifest = read(SDK / "sdk-manifest.json")
        # An installed SDK reaches here through the global spite (SPITE_SDK_RESOLVED) and came from a promoted release
        if isinstance(manifest, dict) and manifest.get("distribution") == "unverified-candidate" and not os.environ.get("SPITE_SDK_RESOLVED"):
            print("WARNING: This SDK is an unverified candidate. Qualification is recorded separately in the promotion bundle.", file=sys.stderr)
    except (OSError, ValueError, KeyError):
        pass
    arguments = sys.argv[1:]
    if arguments[:1] == ['python']:
        try:
            Toolchain.resolve('python')
            return subprocess.call([sys.executable, '-B', *arguments[1:]], env=clean_env())
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            print(f'SDK Python: {error}', file=sys.stderr)
            return 1
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("doctor", help="check runtime or exact native compiler prerequisites; no installation")
    check.add_argument("--profile", choices=("editor", "build"), default="build")
    check.add_argument("--fast", action="store_true", help="skip the final compile-and-load probe (detection only)")
    check.add_argument("--json", action="store_true", help="print the machine-readable verdict, including the exact compiler install command")
    setting_up = commands.add_parser("setup", help="install Python, CMake and Ninja (no admin), then the compiler with one administrator approval")
    setting_up.add_argument("--layout", type=Path, help="offline Visual Studio layout (Windows)")
    setting_up.add_argument("--no-wait", action="store_true", help="when a Visual Studio installation is already running, report it and stop instead of waiting for it (Windows)")
    setting_up.add_argument("--offline-tools", type=Path, help="directory with the archives named in Toolchain.json (or SPITE_OFFLINE_TOOLS)")
    new = commands.add_parser("new", help="create a self-contained game project (any path; missing or empty) pinned to this SDK")
    new.add_argument("output", type=Path)
    new.add_argument("--name", help="project and module name (default: the folder name as an identifier)")
    new.add_argument("--content-only", action="store_true", help="create content workspace without a C++ sample")
    select = commands.add_parser("select", help="re-pin an existing project to this SDK without rebuilding (backs up the pin)")
    select.add_argument("project", nargs="?", type=Path)
    upgrade_cmd = commands.add_parser("upgrade", help="back up .spite/sdk.json, re-pin the project to this SDK and rebuild")
    upgrade_cmd.add_argument("project", nargs="?", type=Path)
    upgrade_cmd.add_argument("--to", help="ignored here; the global spite already selected the target SDK version")
    info = commands.add_parser("project", help="print project/SDK paths and host arguments as JSON")
    info.add_argument("project", nargs="?", type=Path)
    launch = commands.add_parser("run", help="launch the editor or game with project-owned writable roots")
    launch.add_argument("project", nargs="?", type=Path, help="default: the project containing the current directory")
    launch.add_argument("--game", action="store_true")
    launch.add_argument("--control", choices=("stdio", "none"), default="stdio")
    desktop = commands.add_parser("desktop", help="create Linux desktop entries with the SDK icon for a project")
    desktop.add_argument("project", nargs="?", type=Path)
    desktop.add_argument("--output", type=Path, help="desktop-entry directory (default XDG applications)")
    tools = commands.add_parser('tools', help='print resolved tool paths, versions and hashes; never download')
    tools.add_argument('--profile', choices=('editor', 'build'), default='build')
    verify = commands.add_parser("verify", help="run optional native end-to-end SDK acceptance")
    verify.add_argument("--output", type=Path, required=True)
    verify.add_argument("--forbid-path", action="append", default=[], type=Path)
    compile_project = commands.add_parser("build", help="configure and build a project with selected CMake")
    compile_project.add_argument("project", nargs="?", type=Path)
    source = commands.add_parser('source', help='build and select project-local open module implementations')
    source_commands = source.add_subparsers(dest='source_command', required=True)
    source_copy_cmd = source_commands.add_parser('copy', help='copy an approved stock module source tree into the project')
    source_copy_cmd.add_argument('project', type=Path); source_copy_cmd.add_argument('module'); source_copy_cmd.add_argument('destination', type=Path)
    source_build_cmd = source_commands.add_parser('build', help='build a customized module with an exact SDK identity')
    source_build_cmd.add_argument('project', type=Path); source_build_cmd.add_argument('module'); source_build_cmd.add_argument('source', type=Path)
    source_build_cmd.add_argument('--fork-revision', help='full commit ID whose complete source tree exactly matches; omit for a content snapshot')
    source_select_cmd = source_commands.add_parser('select', help='select project-owned builds for the next run')
    source_select_cmd.add_argument('project', type=Path); source_select_cmd.add_argument('--build', action='append', type=Path, required=True)
    source_restore_cmd = source_commands.add_parser('restore', help='select stock implementations for the next run')
    source_restore_cmd.add_argument('project', type=Path)
    release = commands.add_parser('release', help='check the project against the exact selected SDK release')
    release.add_argument('project', nargs='?', type=Path)
    ship = commands.add_parser("ship", help="validate and archive a standalone modular game; finds the built modules in build/")
    ship.add_argument("output", type=Path, help="new release directory outside the project and SDK")
    ship.add_argument("--project", type=Path, help="default: the project containing the current directory")
    ship.add_argument("--module", action="append", default=[], type=Path, help="override: module binaries in ship.json order")
    ship.add_argument("--smoke", action="store_true", help="run eight frames before committing output")
    legacy = commands.add_parser("package", help="alias of ship with an explicit project and --module binaries")
    legacy.add_argument("project", type=Path)
    legacy.add_argument("output", type=Path)
    legacy.add_argument("--module", action="append", required=True, type=Path)
    legacy.add_argument("--smoke", action="store_true")
    usage = commands.add_parser('usage', help='enable or disable anonymous daily SDK activity reporting for this user')
    usage.add_argument('setting', choices=('on', 'off'))
    host_args = []
    if arguments[:1] == ["run"] and "--" in arguments:
        separator = arguments.index("--")
        arguments, host_args = arguments[:separator], arguments[separator + 1:]
    args = parser.parse_args(arguments)
    try:
        if args.command == 'usage':
            Usage.set_enabled(args.setting == 'on')
            print('SDK activity reporting ' + args.setting)
            return 0
        if Usage.qualifies(args.command, host_args):
            try:
                Usage.launch(sdk_version())
            except (OSError, ValueError, KeyError, TypeError):
                pass
        Toolchain.resolve('python')
        if args.command == "doctor":
            return 0 if doctor(args.profile, args.json, args.fast)["ok"] else 1
        if args.command == "setup":
            return setup(args.layout, args.offline_tools, wait=not args.no_wait)
        if hasattr(args, "project") and args.command != "package":
            args.project = args.project or find_project()
        if args.command == "new":
            new_project(args.output, args.name, args.content_only)
        elif args.command == "select":
            select_project(args.project)
        elif args.command == "upgrade":
            upgrade(args.project)
        elif args.command == "project":
            print(json.dumps(project_info(args.project), indent=2))
        elif args.command == "run":
            run_project(args.project, args.game, args.control, host_args)
        elif args.command == "desktop":
            desktop_entries(args.project, args.output)
        elif args.command == 'tools':
            print(json.dumps(Toolchain.tools(args.profile), indent=2))
        elif args.command == "verify":
            verify_sdk(args.output, args.forbid_path)
        elif args.command == "build":
            build(args.project)
        elif args.command == 'source':
            if args.source_command == 'copy':
                print(source_copy(args.project, args.module, args.destination))
            elif args.source_command == 'build':
                print(source_build(args.project, args.module, args.source, args.fork_revision))
            elif args.source_command == 'select':
                print(source_select(args.project, args.build))
                print('Selection takes effect on the next run; running processes are unchanged.')
            else:
                print(source_restore(args.project))
                print('Stock selection takes effect on the next run; running processes are unchanged.')
        elif args.command == 'release':
            source_project_context(args.project)
            print('The project lock matches this exact SDK build. Version numbers do not establish binary compatibility.')
        else:
            package(args.project, args.output, args.module, args.smoke)
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"SDK: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
