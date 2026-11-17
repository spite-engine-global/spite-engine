"""Drive a local SPITE session from Python, or run its agent workflows as a CLI."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import math
import os
from pathlib import Path
import queue
import re
import subprocess
import tempfile
import threading
from dataclasses import dataclass


_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EXE = _ROOT / ("bin/spite.exe" if (_ROOT / "bin/spite.exe").is_file() else "bin/x64/Release/spite.exe")


class SpiteError(RuntimeError):
    """A control refusal, with its original reply available to the caller."""

    def __init__(self, name, reply):
        super().__init__(f"{name}: {reply['error']}")
        self.reply = reply


@dataclass
class Activity:
    id: int
    evidence: str = ""


_UNSET = object()


class _BorrowedSession:
    def __init__(self, owner):
        self._owner = owner

    def __getattr__(self, name):
        return getattr(self._owner, name)

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class Session:
    """A synchronous JSON-lines connection. Use a context manager to end it.

    Raw stdio hosts respond only to requests: use Session(argv, wait_ready=False)
    (the default). wait_ready=True requires a startup ready envelope, as emitted
    by spite attach --json --ready. Use Session.attach() for that workflow.
    """

    def __init__(self, argv, request_timeout=30, *, engine_log=None, wait_ready=False):
        if not math.isfinite(request_timeout) or request_timeout <= 0:
            raise ValueError("request timeout must be positive and finite")
        self._timeout = request_timeout
        self._engine_log = engine_log
        self._stderr = open(engine_log, "w+b") if engine_log else tempfile.TemporaryFile()
        try:
            self._process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._stderr)
        except BaseException:
            self._stderr.close()
            raise
        self._requests = queue.Queue()
        self._replies = queue.Queue()
        self._lock = threading.Lock()
        self._closed = False
        self._scratch = None
        self._activity = None
        self._worker = threading.Thread(target=self._exchange, args=(wait_ready,), daemon=True)
        self._worker.start()
        if wait_ready:
            try:
                ready = self._receive("attach", self._timeout)
                if not isinstance(ready, dict) or ready.get("ready") is not True:
                    raise ConnectionError("SPITE did not confirm the connection")
                self.pipe = ready.get("pipe")
            except SpiteError as error:
                self.close()
                raise ConnectionError(str(error)) from error
            except BaseException:
                self.close()
                raise

    @classmethod
    def attach(cls, executable=_UNSET, *, session=_UNSET, pipe=_UNSET, client=_UNSET, request_timeout=_UNSET,
               engine_log=_UNSET, reuse=None):
        """Connect to an editor, or borrow an existing in-process connection."""
        if reuse is not None:
            if any(value is not _UNSET for value in (executable, session, pipe, client, request_timeout, engine_log)):
                raise ValueError("reuse cannot be combined with connection options")
            if isinstance(reuse, _BorrowedSession):
                reuse = reuse._owner
            if not isinstance(reuse, Session):
                raise TypeError("reuse must be a Session")
            if reuse._closed:
                raise ConnectionError("SPITE session is closed")
            return _BorrowedSession(reuse)
        executable = DEFAULT_EXE if executable is _UNSET else executable
        session = None if session is _UNSET else session
        pipe = None if pipe is _UNSET else pipe
        client = "Python Agent" if client is _UNSET else client
        request_timeout = 30 if request_timeout is _UNSET else request_timeout
        engine_log = None if engine_log is _UNSET else engine_log
        argv = [str(executable), "attach", "--json", "--ready", "--client", client]
        if session is not None:
            argv += ["--session", session]
        if pipe is not None:
            argv += ["--pipe", pipe]
        return cls(argv, request_timeout, engine_log=engine_log, wait_ready=True)

    @classmethod
    def run(cls, executable=DEFAULT_EXE, *, session=None, client="Python Agent", request_timeout=30,
            lifetime=120, engine_args=("--headless",), engine_log=None):
        """Start a disposable editor through SPITE's bounded launch path."""
        if type(lifetime) is not int or lifetime <= 0:
            raise ValueError("engine lifetime must be a positive number of seconds")
        argv = [str(executable), "run", "--json", "--client", client, "--timeout", str(lifetime)]
        if session is not None:
            argv += ["--session", session]
        scratch = tempfile.TemporaryDirectory(prefix="spite-agent-", ignore_cleanup_errors=True)
        writable_args = [
            value
            for folder in ("user-config", "saves", "scenes", "cache", "shadows", "crashes", "artifacts", "control")
            for value in (f"--{folder}-dir", str(Path(scratch.name) / folder))
        ]
        try:
            connection = cls(argv + ["--", *writable_args, *engine_args], request_timeout, engine_log=engine_log)
        except BaseException:
            scratch.cleanup()
            raise
        connection._scratch = scratch
        return connection

    def _exchange(self, wait_ready):
        try:
            with self._process.stdin, self._process.stdout:
                if wait_ready:
                    ready = self._process.stdout.readline()
                    self._replies.put(ready)
                    if not ready:
                        return
                for line in iter(self._requests.get, None):
                    self._process.stdin.write(line)
                    self._process.stdin.flush()
                    reply = self._process.stdout.readline()
                    self._replies.put(reply)
                    if not reply:
                        return
        except OSError as error:
            self._replies.put(error)

    def _close(self, abort=False):
        if self._closed:
            return
        self._closed = True
        self._requests.put(None)
        if abort:
            self._process.kill()
        try:
            self._process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self._process.kill()
            self._process.wait()
        self._worker.join(timeout=3)
        tail = ""
        if abort:
            end = self._stderr.seek(0, os.SEEK_END)
            self._stderr.seek(max(0, end - 1600))
            tail = self._stderr.read().decode("utf-8", errors="replace").strip()
        self._stderr.close()
        if self._scratch is not None:
            self._scratch.cleanup()
        log = f" Engine log: {self._engine_log}." if self._engine_log else ""
        return log + (f"\n{tail}" if tail else "")

    def close(self):
        with self._lock:
            self._close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _receive(self, name, timeout):
        try:
            data = self._replies.get(timeout=timeout)
        except queue.Empty:
            detail = self._close(abort=True)
            hint = "" if name == "attach" else " Reconnect and inspect state before retrying a write."
            raise TimeoutError(f"{name} timed out.{hint}{detail}") from None
        if isinstance(data, OSError) or not data:
            detail = self._close(abort=True)
            raise ConnectionError(f"{name}: SPITE closed the session.{detail}")
        try:
            reply = json.loads(data)
        except (UnicodeError, ValueError):
            self._close(abort=True)
            raise ConnectionError(f"{name}: SPITE returned invalid JSON") from None
        if isinstance(reply, dict) and reply.get("ok") is False and isinstance(reply.get("error"), str):
            raise SpiteError(name, reply)
        if isinstance(reply, dict) and reply.get("ok") is True and "result" in reply:
            return reply["result"]
        self._close(abort=True)
        raise ConnectionError(f"{name}: SPITE returned an invalid reply envelope")

    def _request(self, kind, name, args, *, timeout=None, quiet=False):
        request = {kind: name, "args": args}
        if self._activity is not None:
            request["activity"] = self._activity
        if quiet:
            request["quiet"] = True
        line = (json.dumps(request, allow_nan=False, separators=(",", ":")) + "\n").encode("utf-8")
        with self._lock:
            if self._closed:
                raise ConnectionError("SPITE session is closed")
            self._requests.put(line)
            return self._receive(name, self._timeout if timeout is None else timeout)

    def query(self, name, /, *, quiet=False, **args):
        return self._request("query", name, args, quiet=quiet)

    def command(self, name, /, **args):
        return self._request("command", name, args)

    def search(self, text="", *, owner=None, kind=None, limit=12):
        args = {"text": text, "limit": limit}
        if owner is not None:
            args["owner"] = owner
        if kind is not None:
            args["kind"] = kind
        return self.query("searchCapabilities", **args)

    def describe(self, name):
        return self.query("describe", name=name)["entries"][0]

    def call(self, name, /, **args):
        """Resolve the verb from its live schema, then call it."""
        kind = self.describe(name)["kind"]
        return self._request(kind, name, args)

    def batch(self, name, commands, *, world="scene"):
        return self.command("batch", name=name, world=world, commands=commands)

    @contextmanager
    def activity(self, title, *, kind="script", why=""):
        """Group the requests in this block under one live Agent operation."""
        if self._activity is not None:
            raise RuntimeError("another Agent activity is open on this session")
        row = Activity(self.command("beginAgentActivity", kind=kind, title=title, why=why)["id"])
        self._activity = row.id
        try:
            yield row
        except BaseException as error:
            self._activity = None
            evidence = str(error).encode("utf-8")[:512].decode("utf-8", errors="ignore")
            try:
                self.command("finishAgentActivity", id=row.id, state="failed", evidence=evidence)
            except (OSError, RuntimeError):
                pass
            raise
        else:
            self._activity = None
            self.command("finishAgentActivity", id=row.id, state="done", evidence=row.evidence)

    def wait_module_build(self, operation, seconds):
        """Wait for one compact reply without spending the ordinary request timeout on the build."""
        if type(seconds) is not int or not 1 <= seconds <= 600:
            raise ValueError("wait seconds must be between 1 and 600")
        return self._request("query", "waitAgentModuleBuild", {"operation": operation, "seconds": seconds},
                             timeout=seconds + self._timeout)


def _note(value, name, limit):
    if (not isinstance(value, str) or not value.strip() or len(value.encode("utf-8")) > limit
            or any(ord(char) < 32 or ord(char) == 127 for char in value)):
        raise ValueError(f"{name} must be one line of text, at most {limit} UTF-8 bytes")


def validate_plan(plan):
    if not isinstance(plan, dict) or set(plan) != {"project", "title", "inspect", "apply", "review", "checks"}:
        raise ValueError("plan needs project, title, inspect, apply, review, and checks")
    _note(plan["project"], "project", 128)
    _note(plan["title"], "title", 96)
    for stage in ("inspect", "apply", "review"):
        note = plan[stage]
        if not isinstance(note, dict) or set(note) != {"summary", "why"}:
            raise ValueError(f"{stage} needs summary and why")
        for field in note:
            _note(note[field], f"{stage}.{field}", 280)
    checks = plan["checks"]
    if not isinstance(checks, list) or not 1 <= len(checks) <= 8:
        raise ValueError("plan needs one to eight checks")
    for check in checks:
        if (not isinstance(check, dict) or not {"label", "query", "pointer", "equals"} <= check.keys()
                or check.keys() - {"label", "query", "pointer", "equals", "args"}):
            raise ValueError("each check needs label, query, pointer, and equals, with optional args")
        _note(check["label"], "check.label", 120)
        _note(check["query"], "check.query", 128)
        _note(check["pointer"], "check.pointer", 160)
        args = check.get("args", {})
        if not check["pointer"].startswith("/") or not isinstance(args, dict) or args.get("world", "play") != "play":
            raise ValueError("checks need a JSON pointer starting with / and object arguments scoped to Play")
    json.dumps(plan, allow_nan=False)


def module_loop(session, plan, *, start_play=False, build_timeout=180, verify_active=False):
    """Record Play checks against a newly linked or explicitly selected active binary."""
    validate_plan(plan)
    if not math.isfinite(build_timeout) or not 0 < build_timeout <= 600:
        raise ValueError("build timeout must be between 1 and 600 seconds")

    def playing():
        return any(world["name"] == "play" for world in session.query("worlds")["worlds"])

    stage = "inspect"
    operation = None
    checks = []
    build = {}
    log = ""

    def work():
        status = session.query("agentModuleWork")
        if status["operation"] != operation:
            raise RuntimeError("the editor's active module operation changed")
        return status

    try:
        if not start_play and not playing():
            raise RuntimeError("start Play in the editor or pass --start-play")
        begin = session.command("beginAgentModuleWork", project=plan["project"], title=plan["title"],
                                verifyActive=verify_active, **plan["inspect"])
        operation = begin["operation"]
        if verify_active:
            binary = begin["binary"]
        else:
            stage = "build"
            session.command("noteAgentModuleWork", operation=operation, stage="apply", **plan["apply"])
            started = session.command("buildProject", project=plan["project"], operation=operation)["started"]
            log = started.get("output", "")
            build = session.wait_module_build(operation, math.ceil(build_timeout))
            if build["operation"] != operation:
                raise RuntimeError("the editor's active module operation changed")
            if build["timedOut"]:
                raise TimeoutError("timed out waiting for the linked build. Inspect its status in the editor.")
            if build["build"] == "unchanged":
                return {"ok": False, "outcome": "unchanged", "stage": stage, "operation": operation,
                        "binary": build["binary"], "checks": [],
                        "message": "The build produced the same binary. Use --verify-active to check the loaded one.",
                        "log": build.get("log") or log}
            if build["build"] != "ok":
                detail = f" {build['diagnostic']}" if build.get("diagnostic") else ""
                raise RuntimeError(f"linked build ended as {build['build']}.{detail}")

            stage = "reload"
            binary = build["binary"]
            module = build["module"]
            projects = session.query("buildStatus", project=started["project"])["projects"]
            row = next((entry for entry in projects if entry["project"] == started["project"]), {})
            if (not binary or row.get("pid") != started["pid"] or row.get("state") != "ok"
                    or row.get("currentWriteFiletime") != binary
                    or (row.get("binary") or {}).get("writeFiletime") != binary
                    or row.get("module") != module or not row.get("loaded")):
                raise RuntimeError("the linked build is no longer the reload candidate")
            if not build["loaded"]:
                session.command("reloadModule", name=module)
            status = work()
            if not status["loaded"] or status["binary"] != binary:
                raise RuntimeError("the editor did not activate the linked binary")

        stage = "review"
        if not playing() and start_play:
            session.command("play")
        for check in plan["checks"]:
            result = session.command("checkAgentModuleWork", operation=operation, **check)
            checks.append({"label": check["label"], "passed": result["passed"]})
        session.command("noteAgentModuleWork", operation=operation, stage="review", **plan["review"])
        status = work()
        if not status["loaded"] or status["binary"] != binary:
            raise RuntimeError("the selected binary changed during review")
        return {"ok": all(check["passed"] for check in checks), "stage": stage, "operation": operation,
                "binary": binary, "checks": checks}
    except (RuntimeError, OSError) as error:
        if operation is not None and build.get("build") != "failed" and not isinstance(error, (TimeoutError, ConnectionError)):
            try:
                evidence = str(error).encode("utf-8")[:512].decode("utf-8", errors="ignore")
                session.command("finishAgentActivity", id=operation, state="failed", evidence=evidence)
            except (OSError, RuntimeError):
                pass
        result = {"ok": False, "stage": stage, "operation": operation, "error": str(error), "checks": checks}
        if build.get("diagnostic"):
            result["diagnostic"] = build["diagnostic"]
        if build.get("log") or log:
            result["log"] = build.get("log") or log
        if isinstance(error, SpiteError):
            result["reply"] = error.reply
        return result


def scene_probe(session):
    """Create, inspect, move, and undo a probe in a disposable scene."""
    found = session.search("find", kind="query", limit=3)
    if not found["matches"] or found["matches"][0]["name"] != "find":
        raise RuntimeError("find is missing from the live capability search")
    entity = session.command("createEntity", world="scene", name="PythonAgentProbe")["id"]
    session.batch("Move Python agent probe", [{
        "command": "set", "args": {"id": entity, "path": "TransformComponent.position", "value": [2, 0, 0]}
    }])
    moved = session.query("find", world="scene", name="PythonAgentProbe", values=["TransformComponent"], limit=1)
    position = moved["entities"][0]["TransformComponent"]["position"]
    session.command("undo", world="scene")
    restored = session.query("entity", world="scene", id=entity)["components"]["TransformComponent"]["position"]
    return {"ok": position == [2, 0, 0] and restored == [0, 0, 0], "moved": position, "restored": restored}


def configure_flipbook(session, *, grid=None, frames=None, fps=None, loop=None, camera_facing=None):
    """Apply or restart flipbook playback on the editor's selected card."""
    selection = session.query("selection")
    if len(selection["selection"]) != 1:
        raise ValueError("select exactly one flipbook card in the editor")
    entity, = selection["selection"]
    world = selection["world"]
    components = session.query("entity", world=world, id=entity, quiet=True)["components"]
    model = components.get("MeshComponent", {}).get("meshHandle", {}).get("name", "")
    if not model:
        raise ValueError("select the textured mesh entity of the flipbook card")

    metadata = {"framesPerSecond": 24, "loop": False, "cameraFacing": False}
    if grid is not None:
        size = re.fullmatch(r"([1-9][0-9]{0,3})[xX]([1-9][0-9]{0,3})", grid)
        if size is None:
            raise ValueError("use --grid COLSxROWS, for example --grid 5x5")
        columns, rows = map(int, size.groups())
        metadata.update(columns=columns, rows=rows, frameCount=columns * rows)
    else:
        root = next((Path(row["directory"]).resolve() for row in session.query("roots")["roots"]
                     if row["name"] == "import"), None)
        if root is None or Path(model).name != model or ":" in model:
            raise ValueError("cannot find a preset for this model; supply --grid COLSxROWS")
        preset = (root / f"{model}.flipbook.json").resolve()
        if not preset.is_relative_to(root) or not preset.is_file():
            raise ValueError(f"no preset for {model}; supply --grid COLSxROWS")
        values = json.loads(preset.read_text(encoding="utf-8"))
        grid_fields = {"columns", "rows", "frameCount"}
        if (not isinstance(values, dict) or not grid_fields <= values.keys()
                or values.keys() - (metadata.keys() | grid_fields)):
            raise ValueError(f"invalid flipbook preset: {preset.name}")
        metadata.update(values)

    for key, value in (("frameCount", frames), ("framesPerSecond", fps), ("loop", loop),
                       ("cameraFacing", camera_facing)):
        if value is not None:
            metadata[key] = value
    metadata.update(elapsedSeconds=0, playing=True)
    commands = []
    if "FlipbookComponent" in components:
        commands.append({"command": "removeComponent", "args": {"id": entity, "component": "FlipbookComponent"}})
    commands.append({"command": "placeFlipbook", "args": {"id": entity, "metadata": metadata}})
    session.batch(f"Configure flipbook: {model}", commands, world=world)
    return {"ok": True, "id": entity, "world": world, "model": model, "settings": metadata}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE, help="path to the SPITE CLI executable")
    parser.add_argument("--session", help="named editor session")
    parser.add_argument("--pipe", help="explicit attachment pipe")
    parser.add_argument("--client", default="Python Agent")
    parser.add_argument("--engine-log", type=Path, help="write engine diagnostics to this file")
    parser.add_argument("--request-timeout", type=float, default=30)
    verbs = parser.add_subparsers(dest="verb", required=True)
    search = verbs.add_parser("search", help="search the live registry of an open editor")
    search.add_argument("text", nargs="?", default="")
    search.add_argument("--owner")
    search.add_argument("--kind", choices=("query", "command", "component", "recipe"))
    search.add_argument("--limit", type=int, default=12)
    native = verbs.add_parser("module-loop", help="build, reload, and check a game module in the editor")
    native.add_argument("plan", type=Path, help="JSON plan with decision notes and Play checks")
    native.add_argument("--start-play", action="store_true", help="start Play after reload if it is stopped")
    native.add_argument("--verify-active", action="store_true", help="check the loaded binary without building or reloading")
    native.add_argument("--build-timeout", type=float, default=180)
    flipbook = verbs.add_parser("flipbook", help="configure or restart playback on the selected textured card")
    flipbook.add_argument("--grid", help="COLSxROWS; omit to use the selected model's preset")
    flipbook.add_argument("--frames", type=int, help="number of usable frames")
    flipbook.add_argument("--fps", type=float, help="frames per second")
    flipbook.add_argument("--loop", action=argparse.BooleanOptionalAction, help="repeat playback")
    flipbook.add_argument("--camera-facing", action=argparse.BooleanOptionalAction, help="face the active camera")
    verbs.add_parser("scene-probe", help="exercise search, edit, readback, and undo in a disposable scene")
    args = parser.parse_args()
    options = {"session": args.session, "client": args.client, "request_timeout": args.request_timeout,
               "engine_log": args.engine_log}
    try:
        if args.verb == "scene-probe":
            if args.pipe:
                raise ValueError("scene-probe starts a disposable session and cannot attach to a pipe")
            options["session"] = args.session or f"python-probe-{os.getpid()}"
            with Session.run(args.exe, **options) as session:
                result = scene_probe(session)
        else:
            with Session.attach(args.exe, pipe=args.pipe, **options) as session:
                if args.verb == "search":
                    result = session.search(args.text, owner=args.owner, kind=args.kind, limit=args.limit)
                elif args.verb == "flipbook":
                    result = configure_flipbook(session, grid=args.grid, frames=args.frames, fps=args.fps,
                                                loop=args.loop, camera_facing=args.camera_facing)
                else:
                    plan = json.loads(args.plan.read_text(encoding="utf-8"))
                    result = module_loop(session, plan, start_play=args.start_play, build_timeout=args.build_timeout,
                                         verify_active=args.verify_active)
    except (RuntimeError, OSError, ValueError) as error:
        result = {"ok": False, "error": str(error)}
        if isinstance(error, SpiteError):
            result["reply"] = error.reply
    print(json.dumps(result, indent=2))
    if result.get("outcome") == "unchanged":
        return 3
    return 0 if result.get("ok", True) else 1


if __name__ == "__main__":
    raise SystemExit(main())
