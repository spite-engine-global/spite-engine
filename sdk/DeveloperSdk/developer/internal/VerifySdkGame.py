"""Verify a fresh native SDK: create/build/bake/load/save/package/run outside the installation."""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import Sdk


def verify(output, forbidden=()):
    output = output.resolve()
    if output.exists() or output.is_relative_to(Sdk.SDK):
        raise ValueError("Choose a new verification directory outside the SDK")
    output.mkdir(parents=True)
    manifest = Sdk.read(Sdk.SDK / "sdk-manifest.json")
    Sdk.write(output / "sdk-manifest.json", manifest)
    if not Sdk.doctor("build")["ok"]:
        raise ValueError("SDK prerequisite check failed")
    project = output / "My Game"
    Sdk.new_project(project, "RenderConsumer")
    Sdk.build(project)
    for path in (project / "build").rglob("*"):
        if path.is_file() and (path.suffix in (".cmake", ".vcxproj", ".ninja") or path.name == "CMakeCache.txt"):
            text = path.read_text(encoding="utf-8", errors="replace").replace("\\", "/").casefold()
            for forbidden_path in forbidden:
                if forbidden_path.resolve().as_posix().casefold() in text:
                    raise ValueError(f"External build references forbidden engine source path: {path}")
    model = project / "models/sdk_triangle.obj"
    model.parent.mkdir(exist_ok=True)
    model.write_text('o sdk_triangle\nv -1 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n', encoding="ascii")
    suffix = ".exe" if os.name == "nt" else ""
    Sdk.run([Sdk.SDK / ("bin/SpiteBake" + suffix), model, project / "assets_baked"], project, output / "bake.log")
    module = project / ("build/RenderConsumer.dll" if suffix else "build/libRenderConsumer.so")
    requests = [
        {"command": "newScene"},
        {"command": "instantiateModel", "args": {"model": "sdk_triangle", "name": "SDK baked triangle"}},
        {"command": "saveSceneAs", "args": {"path": "start.json", "overwrite": True}},
        {"command": "loadModule", "args": {"path": str(module)}},
        {"query": "modules"},
        {"command": "shutdown"},
    ]
    content_root, mirrors = Sdk.prepare_run_root(project, modules=False)  # the module is loaded explicitly below
    command = [str(Sdk.SDK / ("bin/SpiteEditor" + suffix)), "--control", "stdio", "--content-root", str(content_root),
               "--project-config-root", str(project / 'config/project'),
               *map(str, Sdk.writable_arguments(output / "editor-state")),
               "--scenes-dir", str(project / "scenes"), "--frames", "600"]
    result = subprocess.run(command, input="".join(json.dumps(r) + "\n" for r in requests),
                            cwd=project, env=Sdk.clean_env(runtime=True), text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=120)
    Sdk.write_back(mirrors)
    (output / "control.jsonl").write_text(result.stdout, encoding="utf-8")
    (output / "editor.log").write_text(result.stderr, encoding="utf-8")
    replies = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    if result.returncode or len(replies) != len(requests) or not all(reply.get("ok") for reply in replies):
        raise ValueError("External editor scene/module checks failed; see control.jsonl and editor.log")
    scene = project / "scenes/start.json"
    if "sdk_triangle" not in scene.read_text(encoding="utf-8"):
        raise ValueError("Saved scene did not retain the baked model binding")
    spec = Sdk.read(project / "ship.json")
    spec["files"].append("scenes/start.json")
    spec["scene"] = "scenes/start.json"
    Sdk.write(project / "ship.json", spec)
    Sdk.package(project, output / "Release", [module], True)
    # Repeat from the final committed package, with no SDK/repo search paths.
    game = output / "Release/game"
    for path in game.rglob("*"):
        if path.is_file() and (set(path.relative_to(game).parts) & {"cache", "shadow-copies", "user"}):
            raise ValueError(f"Generated state leaked into the game package: {path}")
    Sdk.run([game / ("bin/SpiteGame" + suffix), "--content-root", game, "--scene", game / "scenes/start.json",
             "--control", "none", "--frames", "8", *Sdk.writable_arguments(output / "standalone-state")],
            output, output / "standalone.log", runtime=True)
    Sdk.write(output / "verification.json", {"format": 1, "target": manifest["target"],
              "doctor": "passed", "build": "passed", "bake": "passed", "explicitModuleLoad": "passed",
              "sceneRoundtrip": "passed", "package": "passed", "standalone": "passed"})
    print("PASS: fresh SDK consumer; evidence in " + str(output))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--forbid-path", type=Path, action="append", default=[], help="reject this checkout path in generated build dependencies")
    args = parser.parse_args()
    try:
        verify(args.output, args.forbid_path)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"VerifySdkGame: {error}\n")
