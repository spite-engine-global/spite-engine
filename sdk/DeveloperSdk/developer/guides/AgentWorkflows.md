# Detailed agent workflows

Paths here are relative to the installed SDK. Read only the section relevant to your task.

## Installed agent guidance

New projects contain identical `AGENTS.md` and `CLAUDE.md` and plain skill copies for
Codex (`.agents/skills/`) and Claude Code (`.claude/skills/`). The SDK source is under
`skills/`; it includes optional art-authoring examples whose external tools are not
SDK prerequisites. No junction or instruction import connects the two project copies.

`spite upgrade` refreshes agent files after a successful upgrade, after rebuilding native
projects or directly for scene-only projects. It compares each file
against the last-installed SHA-256 in `.spite/sdk-managed-files.json`. Edited SDK-owned
files are kept and reported; unrelated user additions are left alone. An unchanged
counterpart in the other tree can still update.
Keep that marker with the project, inspect preservation messages, and merge wanted
guidance manually. Never remove user-added skills while refreshing SDK content.

## Discover, inspect, then edit

The live registry is authoritative for the selected host and loaded modules. Query
`searchCapabilities`, `describe`, `help`, `components`, `worlds` and `availableModules`.
Search results include recipes and their input/binding steps. Read an entry's schema
before calling it; missing editor or GPU capabilities are meaningful mode differences.

```powershell
& $cli request --session my-game '{"query":"searchCapabilities","args":{"text":"create light","kind":"recipe","limit":3}}'
& $cli request --session my-game '{"query":"describe","args":{"name":"instantiateModel"}}'
```

For generic `set`, inspect the live schema's enum choices. Supported scalar enum
fields (`U8`, `U16`, `U32`, `I32`) accept exact, case-sensitive registered labels,
also inside struct and array values; unknown or ambiguous labels are refused
without writing. Range and read-only guards still apply. Numeric input remains
valid and output remains numeric, so read back the value. Only generic `set`
takes labels: `addComponent` values, config edits and scene JSON take numbers.
Use bounded/projected `find` and `assets` queries, then `entity` for a selected ID.
Use returned IDs and imported source identities; display names and traversal order
are not stable identifiers. Use snapshots/tables and `spite sql` for broad joins.
Keep one persistent connection for scripts rather than reconnecting for each field.

Group related scene edits into one named undoable `batch`. Read back the values;
in a disposable scene verify `undo` restores them. The shipped
`tools/SpiteCli/spite_agent.py` exposes this pattern:

```python
from spite_agent import Session  # add the SDK's tools/SpiteCli directory to sys.path

with Session.attach(executable=cli_path, session="my-game") as engine:
    item = engine.command("createEntity", world="scene", name="AgentProbe")["id"]
    engine.batch("Place AgentProbe", [{"command": "set", "args": {
        "id": item, "path": "TransformComponent.position", "value": [2, 0, 0]}}])
    moved = engine.query("entity", world="scene", id=item)
    engine.command("undo", world="scene")
    restored = engine.query("entity", world="scene", id=item)
```

For a raw stdio host, use `Session(argv, wait_ready=False)` (the default): it
responds to requests and emits no startup greeting. `wait_ready=True` expects the
ready envelope from `spite attach --json --ready`; prefer `Session.attach(...)`
for that transport. It waits for the connection before sending a request.

Compare the returned transforms to the expected values; the example leaves the
created probe at its original position. A batch does not make file operations
undoable. Inspect `history`/`provenance` and read failure envelopes (`details`,
`argErrors`) before attempting a revert after intervening edits.

## Models, Play and runtime modules

Author persistent content in `scene`. `play` clones it; Play changes are transient
and `stop` discards them. Save the edit scene separately, reopen it, and re-query
identities/transforms/assets to prove persistence. Follow the runnable
`samples/AssetConsumer` roundtrip for generation, baking, import, save and reopen.

Model bake version 15 corrects false shared-position LOD locks in model space for
every instance (version 17 for skinned parts on separate nodes); in version 16
coarse LOD levels no longer pull a free open border (a terrain tile's outer edge,
a seam with another file) inward at zero reported error; a border can still be
trimmed where that error is priced. Older bakes still load; use `reimportModel` with `name=...` and `wait=true` to regenerate from
an available source. Seam-heavy geometry can legitimately produce few or no coarser
levels. `lodStats` aggregates views; it is not a per-mesh bake inventory; verify
visible coverage separately.
Supplied `instantiateModel` transform fields edit the imported root and can replace
source rotation/scale. To preserve the imported basis, create a separate placement
parent and instantiate under it without overriding source-root fields. Inspect the
resulting hierarchy, especially animation-bound local transforms. An explicit
placement mode is pending; do not assume an undocumented argument exists.

Game logic is a runtime-loaded native C++ module. Include only the game-facing
public headers under `include/spite`, link SDK DLLs through the CMake package, and build against
this SDK's exact API/toolset stamp. `load` registers module capabilities; `attachWorld`
registers Play systems and world services. Use the world passed to that callback,
not the edit scene, for gameplay. Register owned services/systems/handlers under
the module owner and release callbacks/subscriptions on unload. Reload preserves
entities and round-trips schema fields; it is not a scene reset.
Rigidbody and collider authoring edits survive frames with no fixed step and are
consumed by physics at fixed synchronization.
Optional `hasComponent` and `tryGetComponent` inspection returns empty for pending,
dead, stale or unregistered entities; strict `getComponent` requires a committed
entity. Typed `SystemContext` try-access still requires declared dependencies.

Check optional services before using them. `ModelSpawner` requires an initialized
asset runtime and a bound world; a no-GPU host does not initialize it. Spawn on the
frame thread outside system execution. Handle unknown/unbaked-model failures;
successful hierarchy creation does not itself prove GPU/resource readiness.

Build the external `samples/RenderConsumer` with the README's CMake commands before
its first load. Query `availableModules` and load the absolute produced module path.
After rebuilding, explicitly invoke `reloadModule`; inspect its reply and the active
module identity. Build success alone does not prove reload or behavior success.
Keep source revision, SDK/API/toolset, configuration, output path and binary hash.

For projects already recognized by the editor build configuration, discover
`buildStatus`, `buildProject`, `cancelBuild`, `agentModuleWork` and the diagnostic
queries. The Python `module-loop` reuses external builds, diagnostics/activity,
explicit reload and checks; `tools/SpiteCli/examples/sandbox-module-loop.json`
shows its current plan format. Standalone SDK CMake registration in that loop is
pending: a successfully loaded external DLL is not automatically a registered build
project. Never compile inside the host or install automatic reload watchers.

## Full-detail LOD for chosen renderables

Cluster LOD (`graphics.lod`, on by default) draws coarser clusters of a mesh as it
gets smaller on screen. To keep chosen instances at full detail (level 0) while every
other instance of the same baked mesh keeps automatic LOD, add
`RenderLodPolicyComponent` (`fullDetail`, default `true`) to each renderable. The
baked mesh is shared and unchanged: nothing is rebaked or duplicated. The policy also
applies to the instance's sun and light shadows. `fullDetail:=false` or removing the
component restores automatic LOD. With `graphics.lod` off every renderable draws
level 0 anyway.

For automatic selection with a per-instance adjustment, set `fullDetail:=false` and
`RenderLodPolicyComponent.lodBias:=1`: positive bias selects coarser clusters,
negative bias selects finer clusters, and the default 0 preserves the existing
selection. `fullDetail` takes priority over the bias. Bias does not guarantee
terrain and road overlay clearance (F95); use full detail when needed.

- The policy acts only on the entity that holds the mesh (`MeshComponent`). It is not
  inherited by children. An imported model's root usually has no mesh. A node with
  several meshes gets one child per mesh, and a skinned mesh is its own entity under
  the model root. Use `find with:="MeshComponent"` and keep the rows whose `parent`
  chain reaches the model root. Add the component to each of those entities.
- Add it with `addComponent id=<renderable> component=RenderLodPolicyComponent`.
  Toggle it with `set path=RenderLodPolicyComponent.fullDetail`. It is scene data:
  saving the scene and Play keep it. A model you instantiate again, or spawn from a
  game module, does not get it. Add it to the new renderables yourself.
- Verify with `lodStats` (forcing every visible instance draws only level 0) or the
  `lod` view mode. A forced instance costs its full triangle count at every distance.
  Use it for the instances whose coarser clusters show visible defects, not as a
  scene-wide switch.
- Selection has its own GPU cost. With LOD on, the object cull visits every cluster of
  each mesh whose bounds a view, sun cascade or light shadow map contains, whatever is
  finally drawn, once per such view it renders. A full-detail instance skips the tests
  but still visits its level 0. A mesh of thousands of clusters whose bounds cover the
  whole map (a world terrain baked as one submesh) is visited by every view and
  cascade; split it into tiles so that frustum culling skips the parts out of view.
  Compare the object-cull GPU passes (`Cull_Objects_*`, and the `*_Objects` pass of
  each shadow cascade or light map) with `graphics.lod` on and off before
  attributing a LOD-on cost to triangles.

## Verify behavior and visible UI

Discover `inputActions` and inspect the action's type, binding and context. Press,
held and axis behavior depends on that action. In the editor, use `gameFocus` when
the shown Game panel must own input. Explicit focus clears stale editor focus when
the Game view is visible and no modal owns input; a later UI press can reclaim focus.
Pause `play`, deliver `injectAction` with an
explicit world and frame count, and use `step` with `fixed` for counted simulation
steps. Always release held input in `finally`, including after assertion failure.
Input requests need an application/input system; a bare engine test may not have one.

Record starting scene/configuration/seed, reset policy, focus, each press/release
and fixed-step count. Compare engine-observed transforms, physics results and spawned
entities against expected values/tolerances as well as module status queries.
Capture expected and observed values and the exact active binary identity. Current
module-loop plans contain equality checks; saved action sequences, tolerant/delta
assertions and wider independent witnesses are pending. Do not claim a module's own
success flag independently proves movement, collision or projectile creation.

Runtime HUDs must explicitly register their font with the world's
`UiViewContext::registerFont` and select it in styles. Do not assume the editor's
font registrations exist in Play. Query `uiFonts` for missing fonts, pending/failed
glyphs and atlas readiness. `uiTree` proves structure/text; a windowed capture is
needed to establish visible text. Use observed readiness and settled operation
replies rather than arbitrary sleeps. Headless/no-GPU modes cannot prove visibility.

## Scene and game-host checks

Scene components need at least one persisted schema field. A zero-field native
marker can be added in memory but scene collection omits it; an observed-only
schema is likewise not scene data. Give an authored marker a persisted field and
query its presence before saving and again after reopening. Verify the module is
loaded before opening a scene that contains its component types.

Use `createPrimitive` with `parent` for a new child, or `reparent` with the live
child `id` and `parent` for an existing one. Reparent keeps the local transform;
read back the resulting world transform after the hierarchy updates. Do not reorder
or delete entities directly in scene JSON: its parent references are array indices,
not persistent IDs. Save, reopen, resolve names to new live IDs, and verify the
parent and world matrices of the rendered children, not only a controller's state.
The stock torus revolves around local Y, with its ring in XZ; its ring radius
is 0.35 and tube radius 0.15 before scaling. Rotate it to stand upright and
inspect the rendered result after nonuniform scaling.

`MaterialComponent.materialHandle` is read-only to `set`. For an existing primitive,
create or locate the material, then remove its `MaterialComponent` and add it with
`value={"materialHandle":{"name":"coral","assetType":3,"subAssetIndex":0}}`.
Read the component back and verify it after reopening; use an asset actually present
in the project. Discover the current `createMaterial` and component schemas first.

A game-host launch is a separate check from editor Play. `SpiteGame --scene` opens
the scene then starts Play; when its old view camera is gone, scene loading chooses
a loaded camera, with no stable ordering promised among multiple cameras. Keep one
scene camera when relying on automatic selection, then inspect `views` in a fresh
host without first issuing `bindCamera`. The cooked game host scans scene entities
in document order. For an interactive editor session, bind the intended camera
explicitly as described below.

`step`'s `fixed` argument is an integer count of fixed steps, not seconds. For a
720p HUD, inspect a game-host swapchain screenshot and its reported dimensions: a
Game-panel screenshot only covers that panel's current size. Stylesheet background
uses `"background":{"color":[r,g,b,a]}`. Check the `Expected` results from
font, stylesheet and binding operations and report their error details; a successful
UI tree query alone does not prove the HUD is visible. For example, create the
panel whose ID the style targets and retain errors from both operations:

```cpp
const auto font = view.registerFont(InternedString("hudFont"),
                                    resolveContentPath("assets/fonts/Geist-Regular.ttf"));
if (!font)
    throw std::runtime_error(font.error().message);
const auto style = view.stylesheet().loadLayerText(
    InternedString("hud"), R"({"defaults":{"text":{"font":"hudFont"}},"elements":{"hudPanel":{"background":{"color":[0,0,0,0.8]}}}})");
if (!style)
    throw std::runtime_error(style.error().message);
UiRectDesc rect;
rect.size = {400, 100};
scope.createPanel(InternedString("hudPanel"), rect);
scope.commit();
```

`kinds.panel` styles panels by widget kind; `elements.hudPanel` matches the
actual element ID. Unmatched selectors are valid, including when styles load
before elements are created.

`shutdown` ends an owned host; `Session.close()` detaches the client.

## Native iteration and input

A query view with one requested type yields the component directly. Including
`Entity` yields a tuple instead. The SalvageGame sample compiles both forms:

```cpp
std::string initialScore = "0";
for (const auto& state : query->view<Read<ClockworkState>>())
{
    initialScore = std::to_string(state.score);
    break;
}
for (auto [entity, state] : query->view<Entity, Read<ClockworkState>>())
{
    (void)state;
    auto binding = ui.bindings().add(scope, score.entity(),
        {.source = entity, .component = "ClockworkState", .field = "score"});
    if (!binding)
        throw std::runtime_error(binding.error().message);
    break;
}
```

Native input uses `KeyCode::RETURN`, as in the sample's `mapActionToKey` call; `ENTER` may
be a text alias in input configuration but is not the C++ enumerator.

## World and component lifetimes

Entity handles belong to a particular world. Re-resolve them after a scene is
opened or a world is replaced, including Play cloning; names are labels and
are not necessarily unique. Component references and pointers may be invalidated
by structural changes anywhere in the world: moving another entity can compact
its old archetype. Copy values needed before adding or removing components and
reacquire references afterwards.

Play clones relations before calling a module's `attachWorld`, but the compiled
hierarchy may not yet be built. `attachedParentOf`, `currentParentOf` and
`collectSubtree` read the current relation store; the latter two respect
hierarchy membership. `parentOf`, `childrenOf`, `subtreeOf`, `depthOf`,
`isInHierarchy` and `compiledOrder` read the compiled index. The transform
frame update compiles it, or call `compile()` outside a system when an immediate
compiled view is required. Later relation edits likewise require compilation.

Primitive collider dimensions and offsets ignore entity and parent transform
scale. Supply dimensions and offsets in physics world units; offsets are in the
body's local axes and rotate with it. Collider primitives have no independent
rotation.

## Teleporting an existing physics body

In game systems, `ctx.services().write<PhysicsWorld>().get().teleport(entity, position, rotation)` queues a world-space pose for the next fixed step. It returns false if the body has not been created or has been removed; repeated requests before that step use the last pose. The step clears non-static linear and angular velocity and wakes the body. The request is consumed once, even if no fixed step occurs in the frame that submitted it.

Changing `RigidbodyComponent::bodyType` in place rebuilds the Jolt body at the next fixed step: existing contacts are discarded, the old velocities are supplied to the new body, and non-static bodies are activated while static bodies are not. Contact and sleep behavior can therefore differ from leaving a body untouched.


Pause the Play world and discover `RigidbodyComponent` through `components`;
its `transformBehaviourMode` enum exposes the numeric values. Read and save
the body's current mode, select the discovered `Teleport` value (currently 1),
write the desired `TransformComponent` pose, and advance fixed simulation.
`Teleport` is a persistent mode, not an immediate one-shot call. At fixed
synchronization a changed pose is applied and non-static linear and angular
velocities are cleared; an unchanged pose does not trigger that reset.
Read the resulting transform and the observed `RigidbodyStateComponent` linear
and angular velocities after the step; compare the pose and velocities with
the intended result. The body may move again under gravity or contacts during
the step, so account for those forces in the check. Restore the saved numeric
mode in cleanup even if a command or assertion fails. For native code the
corresponding value is `RigidbodyComponent::TransformBehaviourMode::Teleport`.
Do not assume a transform write alone moves a physics-driven dynamic body.
`PhysicsWorld::setLinearVelocity` and `setAngularVelocity` wake eligible non-static
bodies even when the requested velocity is zero.

## Saving game progress

Scene saving persists authored scene data; it is not a save of transient Play
progress. For progress, define a game-owned schema with a version field and
stable logical keys (for example, a level key and a game-assigned object key),
not live `Entity` IDs or scene order. Serialize that data with
`serializeJson`; on load, deserialize into a temporary value, check the return
and error, validate its version and keys, then apply it to newly resolved live
entities. A failed read can already have modified earlier fields of its target.
When writing custom text instead, use `std::numeric_limits<T>::max_digits10`
digits for the floating-point type `T` to preserve round trips, and check file
write and read errors. Keep the authored scene and progress files separate.

## Pacing and profiling

Do not label requested pacing as observed. Query `config` with
`section="application.timing"` and inspect each field's `value` and `running`
for `vsyncEnabled`, `limitFPS` and `targetFPS`. `setConfig` changes a staged value;
check its apply status and re-query the running values after deferred changes.
In SDK project runs, project-layer `saveConfig` writes to the project's `config/project`,
while `build/spite-content` supplies fallback settings and the generated module list.
User-layer sections retain their user-config destination; the `config` query's file targets identify the save location.

Do not call `saveConfig` for a temporary experiment. Preserve and restore the prior
values, and reject a capture if requested and running settings disagree. The
running configuration is also exposed in the `ApplicationConfig_config` table.
A bounded `spite run` starts with vsync off, regardless of the saved setting;
read the running value rather than inferring it from the project file.

When the host exposes profiling, discover `profileStart`, `profileStop` and
`profileStatus`. The in-engine capture writes the canonical zones report and a
separate timeline sidecar. Responses below refer to the unwrapped `result` object.
`profileStop` returns a reserved `capture` ID; it is not proof of a sidecar.
Use a bounded deadline and handle connection errors as errors. For example:

```text
reserved = command(profileStop).capture
until deadline:
    status = query(profileStatus)
    if status.stop.state == "stopping": continue
    if status.stop.state == "failed": error(status.stop.error)
    if status.stop.state != "written": error("unexpected stop state")
    check status.lastReportValid and status.lastWarning
    timeline = status.stop.timeline
    if not timeline.built: error(timeline.reason)
    if timeline.sidecar is an object and timeline.sidecar.skipped:
        error(timeline.sidecar.reason)
    if reserved == 0 or timeline.capture != reserved:
        error("no confirmed matching timeline capture")
    break
else: inconclusive("report still stopping")

until deadline:
    entries = query(profileCaptures).captures
    entry = the entry whose id equals reserved, if present
    if entry is missing: continue
    if entry.state == "ready": use profileGpu with capture=reserved; stop
    if entry.state == "failed": error(entry.error)
    if entry.state != "writing": error("unexpected capture state")
else: inconclusive("capture missing or still writing")
```

Pause between polls. `written` means report completion, not timeline readiness;
a ready entry includes its sidecar `path`. The report's environment records the actual renderer present mode; compare that and the report's provenance with the
intended run. A settings readback alone does not prove a particular present mode.
Capture on a quiet machine and record device, workload and metric semantics; busy
or different workloads do not support a performance ranking. Do not substitute a
separate callback timer for the zones producer.

## Prepared and generated content

Discover `prepareModelImport`, `prepareModelUpdate`, `modelPreparation`,
`previewModelPreparation`, `planModelPreparation` and `prepareModelScene` before
large edits. Retain preparation IDs/revisions, inspect conflicts, and cancel work
that is no longer needed. Baseline/current/source reconciliation preserves supported
local overrides; review unsupported additions and stale inputs instead of forcing
them through. Preview or a prepared scene string does not prove publication.

Keep generator/version, seed, canonical parameters, dependency fingerprints and
stable logical output keys beside generated files. Use staged GLB/binary files for
large payloads. Reproducible generator-receipt integration and combined staged
asset-plus-scene publication/recovery/undo remain pending. Do not promise an atomic
regeneration from existing preview calls. Visible terrain mesh tiles also do not
prove terrain collision; shared render/physics heightfield support requires its own
implementation and parity evidence.

Finish an edit with named undo history, independent readback, save/reopen checks and
evidence paths. Optionally run `samples/verification/VerifySdk.ps1` from the Windows SDK into a fresh external
output directory for the shipped asset/render sample acceptance checks. Report
build, load/reload, scene persistence, behavior and visible-output outcomes separately.


## Session ownership and focused identity

For a named Windows host, use the bounded `--detach --timeout 600` launch in
`README.md` when the launching CLI must exit while the host stays alive. Without
`--detach`, keep its stdin open: closing it ends the host. Shut down a host you
started. Attach a single `Session` for an authoring script; closing an attached
session closes the connection, not the host. `Session.run` instead owns a bounded
disposable host and temporary writable roots. It is useful for probes, not for
persisting a project scene unless you supply and retain appropriate roots.

Select the fields needed for evidence rather than printing the binary inventory:

```python
import json
from pathlib import Path
manifest = json.loads((Path(sdk) / "sdk-manifest.json").read_text(encoding="utf-8"))
print({key: manifest[key] for key in ("sourceHead", "sourceIdentity", "target",
      "configuration", "distribution", "apiStamp", "toolsetStamp")})
```

For a repeatable script, query before creating: `find` with a distinctive name and
`world="scene"`; create only when `total` is zero, reuse the returned ID when it is
one, and stop for ambiguous matches. Reconcile fields on that ID and save; do not
repeat `newScene`, blind `createEntity` or model import against an existing project.
Use a fresh project for destructive round trips. The sample transcript is bounded
by default; `SPITE_FULL_TOOL_REGISTRY=1` explicitly saves the full MCP registry.

## Procedural asset round trip

Run `samples/AssetConsumer` in a fresh writable output directory. Its small seeded
GLB generator demonstrates separate vertices for faceted normals and UV seams,
indexed triangles, an embedded texture and a node `extras.spite_source_id` that
stays fixed when the seed changes. The sample validator checks this specific packed
format; it is not a general glTF validator. The editor check reimports seed 732 over
731, waits for the existing instance's material to match the model's new binding,
checks that material's texture and its changed baked bytes, then saves and reopens.
Keep the returned material/texture names: a reimport can create suffixed names.
A successful bake alone does not prove that an existing scene now points at them.
The sample reports evidence in `reimport-evidence.json`; inspect visible output
separately. To extend geometry, preserve logical node IDs, validate each triangle's
index range, area and winding, normal length, UV seam and intended atlas range.

The terrain material profile has one shared `terrainDetailMetres` scale. It currently exposes no independent per-layer scales, near/far macro blend, stochastic sampling or variant selector. Bake source variation into the existing texture slots when those controls are needed; this remains periodic. Game-specific camera clamps belong to the game's own controls, not the terrain profile.

Changed baked material and texture content can receive new names on reimport. Model-owned bindings that were not overridden are eligible for refresh; query the current model and entity material names before deciding whether to rebind. Use `assignMaterial` with the selected `world`, entity `id` and manifest `material` name for an undoable replacement; a matching binding is a no-op. Bulk reimport and first-use loading can disturb residency and timing; observed spikes alone do not establish preview generation as their cause.

A `glTF parse:` error reports a whole-file parser result, not a terrain field path.
Check the source with a glTF validator and inspect material and texture references
to localize malformed input; separately reported terrain validation paths remain useful.

## Audio import, update and playback

The following is a sequence for a disposable project using a persistent attached
`Session` named `s` (attach as in the discovery section). The path is an existing absolute WAV, MP3 or FLAC file outside
the SDK. Import rejects a duplicate basename; it does not replace an existing clip.

```python
import time
job = s.command("importExternalAudio", path=str(source.resolve()))
for _ in range(100):
    result = s.query("externalAudioImport", **{"import": job["import"]})
    if result["status"] in ("done", "failed"):
        break
    time.sleep(0.05)
if result["status"] != "done":
    raise RuntimeError(result)
# Create only in a fresh scene; query first in a reusable authoring script.
emitter = s.command("createEntity", world="scene", name="Audio probe")["id"]
s.command("addComponent", world="scene", id=emitter, component="AudioEmitterComponent",
          value={"clip": job["name"], "spatial": False, "playing": False})
s.command("addComponent", world="scene", id=emitter, component="AudioVoiceSettingsComponent",
          value={"category": "music", "pitch": 1.0})
s.command("play")
try:
    live = s.query("find", world="play", name="Audio probe")["entities"][0]["id"]
    s.command("playAudio", world="play", id=live)
    print(s.query("audioStatus", world="play", id=live))
    s.command("stopAudio", world="play", id=live)
finally:
    s.command("stop")
```

`available`, `loaded` and `playing` are backend observations, not proof a person
heard the output. For an update, replace the project's `assets/audio/<name>` with
an updated supported clip (keep a backup; never edit the SDK installation), then
call `s.command("reloadAudio", world="play", id=live)` for the live Play emitter and
inspect `s.query("audioStatus", world="play", id=live)`. Import is
not an update command. Check channels/rate/frames on initial completion; a human
listening check or independent loopback recording is needed to establish audibility.

If the world has no enabled `AudioListenerComponent` with a transform, spatial audio uses a listener at the world origin, facing -Z with +Y up. It does not follow the camera automatically. `audioStatus` reports backend and voice state; it does not measure output level or prove audibility.

Set a runtime mix category with `s.command("setAudioCategoryGain", world="play", category="music", gain=0.5)`; the empty category is the default, and every unset category starts at 1. With an emitter ID, `audioStatus` includes `category`, `categoryGain`, `masterGain`, and `effectiveGain`. The last is applied emitter volume × category gain × master gain before spatial attenuation, not an output-level or audibility meter. Gains affect playing tails and future voices, and do not rewrite the emitter's volume.

## Windowed scene capture

A scene capture that cannot be recorded within 10 seconds fails with a timeout.

Run a windowed editor with a Vulkan device; headless `captureScene` may return
`scene capture unavailable`. In a fresh scene, create a renderable and a light,
then position the editor camera and capture. For example, with an attached `s`:

```python
s.command("createPrimitive", world="scene", kind="cube", name="Capture probe")
light = s.command("createEntity", world="scene", name="Capture light")["id"]
s.command("set", world="scene", id=light, path="TransformComponent.position", value=[2, 3, 3])
s.command("addComponent", world="scene", id=light, component="PointLightComponent",
          value={"intensity": 200, "radius": 12})
s.command("editorCamera", position=[4, 3, 5], pivot=[0, 0, 0])
result = s.command("captureScene", path="probe.png", width=160, height=90, settle=8)
print(result["path"])
```

Width and height are optional together; omit both to use the current Scene view extent. Both must be positive integers, each at most the device's maximum 2D image dimension, with at most 16,777,216 pixels in total. The deferred success reply contains `path` and the temporary `view` id. Validation, render graph, readback, encoding, and shutdown failures return an error instead of a success result.

For restart comparisons, save the `sceneView` query's `camera` fields before shutdown. After restoring the scene and layout, use `editorCamera` to restore the supported position, pivot, yaw, pitch, distance, fov, near and far fields, then query `sceneView` again and compare the resulting pose. This does not restore hidden perspective/orthographic camera history; check the desired view mode separately. Record the Game camera too when it affects a comparison.

Before capturing, confirm graph readiness as well as normal asset and view readiness.
Discover diagnostic table names at runtime with `tables`, then inspect them with
`tableRows`. Graph stats expose `ready`, `compileFailures`, `sampledDemand` and
`storageDemand`. Descriptor preflight can safely fail a graph build and clean up;
the renderer retries automatically after retirement restores capacity. While the
graph is unready no pass records: the main view presents undefined content and
secondary windows present their clear color, so judge a frame by `ready`, not by
its pixels. Native
extensions retain the void `compile` API and can inspect `ready` and
`canRetryCompile`. `BindlessStats` pending texture and storage-image counts are
already included in their live counts. `texturesRetired` and `storageImagesRetired`
count slots retired for good when their handle generation reached its limit; they
are outside the live counts, and preflight counts them as occupied, so a failed
build retries only when capacity actually returns. The storage-image table remains
capped at 1024; `maxBindlessTextures` does not control it. Graph recreation waits
for device idle and retires completed pending descriptors before allocating the
replacement graph. Graph stats count rebuilds by cause (`rebuildsRetry`,
`rebuildsGeneration`, `rebuildsSwapchainView`, `rebuildsViewExtent`,
`rebuildsRenderScale`, `rebuildsSwapchain`); each rebuild logs an Info line naming
its cause (a generation rebuild also names its requester, such as material classes
or bloom), each swapchain recreation logs its cause (window resized, vsync changed,
acquire or present out of date or suboptimal), and a failed build's error names the
cause. Historical device-loss causality has not been established.
The capture uses a temporary copy of the editor camera, not a scene CameraComponent;
`frame="all"` or `"selection"` can frame bounds. Inspect the PNG: a successful
reply only proves a file was produced, not that lighting, framing or appearance is
correct. The capture path belongs to the captures root from `roots`.

## Flipbook cards

Place a padded row-major PNG/JPEG/TGA atlas under the import root from `roots`. When specifying a colour target, use linear-light albedo values. Colour atlas RGB is sRGB-encoded and the albedo and emissive texture slots decode it to linear light when sampled; data textures use linear values. Material colour factors and object tint multiply the decoded albedo in linear space. Alpha is coverage, not sRGB colour. Convert a linear target to sRGB before writing atlas RGB bytes, and account for any tint or style multiplier in linear space.
Discover `prepareFlipbookCard`, `flipbookCard`, `importModel`, `placeFlipbook` and
`flipbook`. Example metadata is `{"columns": 2, "rows": 2, "frameCount": 4,
"framesPerSecond": 4, "loop": true, "playing": true}` (JSON spelling shown).
Call `prepareFlipbookCard` with that metadata, `path`, a fresh `name`, and
`wait=true`; it returns a generated GLB path. Import it with `importModel` and
`wait=true`, instantiate the returned model name, and call `placeFlipbook` on its
entity with the same metadata. If placement says the card asset is not ready,
wait for readiness and retry that specific refusal. `flipbook` reports frame,
dimensions, pivot and atlas-cell diagnostics. `emissive=true` uses the atlas for
emission; camera-facing cards require the material and mesh constraints described
by `flipbook`. Padding avoids mip bleed. This demonstrates a textured animated card,
not a general particle simulation; inspect a windowed capture or Play output to
establish appearance and animation.

```python
import time
from spite_agent import SpiteError
metadata = {"columns": 2, "rows": 2, "frameCount": 4,
            "framesPerSecond": 4, "loop": True, "playing": True}
card = s.command("prepareFlipbookCard", path="atlas.png", name="effect-card",
                 metadata=metadata, wait=True)
model = s.command("importModel", path=card["path"], wait=True)
entity = s.command("instantiateModel", model=model["name"], name="Effect")["id"]
for attempt in range(100):
    try:
        s.command("placeFlipbook", world="scene", id=entity, metadata=metadata)
        break
    except SpiteError as error:
        if "not ready" not in str(error) or attempt == 99:
            raise
        time.sleep(0.05)
print(s.query("flipbook", id=entity))
```

## Native gameplay: action values and Play capture

For continuous movement, bind keys as `InputActionType::Axis` and read
`InputSystem::getActionValue` each frame. Axis contributes the control value
multiplied by the binding scale. `Hold` has a different contract: it contributes
elapsed hold time only after that time reaches the binding scale, which is the
hold threshold. The default binding scale is 1.0. A short injected Hold can
therefore produce no movement; expecting it to act like an axis is a consumer
assumption, not evidence of an SDK defect. Use `Press` and
`wasActionPressedThisFrame` for one-shot actions. Check the active binding and
context with `inputActions`, and verify movement from the entity's transform.

For a screenshot from an authored game camera, start Play paused and step the
simulation before capture. At frame zero the camera's derived transform may not
have been initialized; a successful screenshot can consequently be white. In the
Clockwork trial two steps produced the intended Game view, including HUD. Inspect
the resulting image and `uiFonts` readiness; a successful command alone does not
prove a meaningful capture. This trial observation is not a guarantee that two
steps settle every scene.

For an authored `CameraComponent`, `fov` is in radians: for example,
`math.radians(37)` is about 0.646. The `editorCamera` command takes degrees
instead. Saving a camera entity does not guarantee that the selected view binding
persists across launches. Find the desired camera and call `bindCamera` on the
game view before Play; the native gameplay sample demonstrates this.

### Frame-boundary module work

From a game module, call `context.runAtFrameStart([] { /* work */ });` on the frame thread to run work once at the next control pump, outside system execution. Queue from callbacks without retaining ECS row pointers or service locks. Pending work is dropped when its module unloads; a callback cannot unload or reload its own module while it runs. Tool-driven scene edits still go through control commands and their undo path.

### Writable game data

Use `savesDirectory()` from `base/ContentRoot.hpp` for game saves. `writableDirectory()` in `base/WritableLayout.hpp` also exposes user config, scenes, cache, shadow copies, crashes, artifacts and control roots. An explicit path takes precedence, followed by the corresponding startup option, environment variable and executable-adjacent `spite-layout.json`; absent these, saves defaults to `saves` beside the executable. The SDK project launcher can configure these roots, so do not assume the content root or project directory is writable. The default resolver creates and verifies the directory and reports failure.

### Game canvas pointers

A play-world canvas can receive `UiCanvasPointer` events in the editor Game view when the game owns input and the simulation is not paused. The editor layers route first and may consume input; the pointer must be within the displayed view unless a press is already captured. The adapter maps the displayed, possibly letterboxed view rectangle into render-target pixels, then the canvas reports `local` and drag `delta` in unscaled UI pixels relative to its own top-left. Wheel events retain the last canvas position. Click the Game view to give it input and use F6 or click outside to release it.

### Game input focus

`views.gameInputFocused` retains its host-ownership meaning. The additive
`gameInputEffectiveFocus` field mirrors the C++ getter, including native focus observations.

`InputSystem::hasGameInputFocus()` reports host game-input ownership combined with the last observed native focus state across the application windows. Native focus starts as true, including for an initially unfocused windowed application, until focus events establish the observed state. This is not a direct OS focus sample or a particular-window focus query. Standalone, headless and injected input retain that initial state until the host or a focus observation changes it. In the editor, moving focus away from the Game view relinquishes ownership; stopping Play relinquishes game input ownership. Pausing simulation does not itself change ownership, though paused game UI does not receive input. `isCursorCaptured()` reports a capture request, not game focus or confirmation that the OS granted capture.

### Reviewing agent activities

`reviewAgentActivity` with `{"id": <activity-id>, "action": "acknowledge"}` clears attention for a failed or cancelled activity when it has no pending reviewable scene step. Its evidence and history remain available. `keep` and `revert` apply to the latest linked scene undo step only; earlier or interleaved steps are not a whole-activity undo.


## Render-pass service locks

`RenderPassExecutionContext` helpers acquire service handles internally. Graph and texture lookups take the `RenderGraph` write handle; buffer-address lookups also take the `IRenderResourceManager` write handle, and `frameIndex()` reads `IRenderer`. Release your corresponding service handles before calling these helpers. Keep direct resource-manager access in a narrow scope, copy the values you need, release the handle, then call the context helper; these service locks are not recursive.

For a per-view pass, `passViewCamera(context)` in `renderruntime/RenderView.hpp` copies the executing view's camera matrices. It returns empty if the view is unavailable. Release any `RenderViews` service handle before calling it. The matrices are the current frame's CPU values; copy them into module-owned GPU data when the shader needs them.

Modules can create buffers with `IRenderResourceManager::createBuffer` and obtain their device address with `context.bufferAddress(handle)`. Include the required usage flags, including `SHADER_DEVICE_ADDRESS` for an address. Mapped CPU-to-GPU buffers stay mapped, but each frame slot needs its own storage or another synchronization scheme: do not overwrite bytes still used by the GPU. Stop submitting uses before `destroyBuffer`; the resource manager retires the GPU object after the frame-slot fence. Release the resource-manager service handle before calling `bufferAddress`.

`AfterTransparent` runs in the HDR scene before exposure and temporal processing. Declare every graph dependency: load and write `SceneColor` as a color attachment to preserve earlier lighting, and declare `DepthBuffer` as a read-only depth attachment when depth testing. Use the scene attachment's format and extent for the pipeline. Reverse depth applies. The graph can then order and synchronize the pass with the surrounding work.

When module-owned data changes the picture, call `RenderPassSetupService::markViewContentChanged(view)` for each affected view before frame planning. This wakes converged temporal processing and auto exposure; a call from the pass callback is too late for that frame's graph predicates. Signal removal of a contribution as well. Dropping a registered owner invalidates every view automatically.

The scene's `AssetRuntime` service exposes `textureIndex(name, space)` for a baked texture. It returns empty until that colour-space upload is ready. The index is borrowed: retain the asset through its existing load or binding lifecycle, query again while recording each frame, and stop using it when that owner releases or replaces the asset. `load` coalesces requests; it is not an independent reference per caller. Coordinate ownership if several consumers use the same texture, and pair the owner's load with release. Do not cache an index across frames or release while another consumer relies on that load.

## Sparse authoring checkouts

For parallel authoring, use Git sparse checkout to omit large source models and
baked content only in checkouts that do not need them. Keep the runtime content
required by each scene available through the project's configured roots, either
in the checkout or in a deliberately managed shared content location. Check
scene and asset readiness before running; sparse checkout alone does not make
missing content available. Preserve shared content and never delete assets
arbitrarily to save space. Include omitted paths again when importing or editing
them, and coordinate writes to any shared content.

## Binding readiness


Request: {"id":1,"query":"bindingReadiness","args":{"world":"scene","limit":100}}
world selects the control world; limit defaults to 100 and accepts 0 through 1000.

Available result example, with one unloaded mesh binding:
{"frameCompletion":"notChecked","availability":"available","total":1,"ready":0,"loading":0,"failed":0,"unknown":1,"truncated":false,"bindings":[{"entity":42,"component":"MeshComponent","handle":{"name":"rock","assetType":1,"subAssetIndex":0},"status":"unknown","meshletComponent":false,"objectStaticInfoComponent":false}]}

assetType is the numeric AssetType value (the example value is illustrative). Rows may also name MaterialComponent; status is ready, loading, failed, or unknown. A failed row additionally has reason. Counts cover all active bindings, even when limit truncates rows. Derived-component flags do not establish frame completion. With no GPU asset factory the result is exactly {"frameCompletion":"notChecked","availability":"unavailable"}; the query does not return CPU-known bindings in that case.

## Audio

`AudioEmitterComponent` defaults to volume 1. Optional `AudioVoiceSettingsComponent` defaults to pitch 1 (a playback-rate ratio), minDistance 1, maxDistance FLT_MAX and rolloff 1. Distances use world units. Spatial voices use inverse-distance attenuation: gain is `minDistance / (minDistance + rolloff * (clamp(distance, minDistance, maxDistance) - minDistance))`. Nonspatial voices do not use distance attenuation. Effective pitch is clamped to [0.01, 16], minDistance to [0.01, 1e30], maxDistance to at least minDistance, and rolloff to [0, 100]; nonfinite values fall back to their defaults.

`setAudioMasterGain` takes `{"world":"play","gain":0.5}`; gain is linear, defaults to 1 and accepts [0, 1]. It affects active and future voices in that world. For example, `audioStatus` with `{"world":"play","id":42}` can return `{"available":true,"masterGain":0.5,"loaded":true,"playing":true,"ended":false,"pitch":1,"minDistance":1,"maxDistance":3.4028235e38,"rolloff":1}` as its result. These are effective parameters, not measured output level.
