const { spawn, spawnSync } = require("node:child_process");
const { createHash } = require("node:crypto");
const { validate } = require("./ValidateAsset.cjs");
const fs = require("node:fs");
const path = require("node:path");
const readline = require("node:readline");
const { performance } = require("node:perf_hooks");

if (!process.env.SPITE_SDK || !process.env.SPITE_ASSET_OUTPUT) {
    throw new Error("Set SPITE_SDK and SPITE_ASSET_OUTPUT before running this sample");
}
const sdk = path.resolve(process.env.SPITE_SDK);
const output = path.resolve(process.env.SPITE_ASSET_OUTPUT);
const cli = path.join(sdk, "bin/spite.exe");
const editor = path.join(sdk, "bin/SpiteEditor.exe");
const projectName = process.env.SPITE_ASSET_CONSUMER_PROJECT ?? "EditorProject";
if (!/^[a-zA-Z0-9-]+$/.test(projectName)) throw new Error("Invalid project name");
const project = path.join(output, projectName);
if (fs.existsSync(project)) throw new Error("Choose a fresh SPITE_ASSET_CONSUMER_PROJECT; this check writes a scene and reimports assets");
validate(path.join(output, "sdk_checker_bollard.glb"));
fs.mkdirSync(project, { recursive: true });
for (const entry of ["assets", "config", "shaders"]) {
    const destination = path.join(project, entry);
    if (!fs.existsSync(destination)) fs.cpSync(path.join(sdk, entry), destination, { recursive: true });
}
const bakedSeed = path.join(sdk, "assets_baked");
const projectBaked = path.join(project, "assets_baked");
if (!fs.existsSync(projectBaked)) {
    if (fs.existsSync(bakedSeed)) fs.cpSync(bakedSeed, projectBaked, { recursive: true });
    else fs.mkdirSync(projectBaked);
}
const model = "sdk_checker_bollard";
const source = model + ".glb";
const scene = "sdk-asset-acceptance.json";
const name = "Checker bollard at entrance";
fs.mkdirSync(path.join(project, "models"), { recursive: true });
fs.copyFileSync(path.join(output, source), path.join(project, "models", source));
const state = ["user-config", "saves", "scenes", "cache", "shadows", "crashes", "artifacts", "control"].flatMap(key => {
    const directory = path.join(project, key);
    fs.mkdirSync(directory, { recursive: true });
    return ["--" + key + "-dir", directory];
});
const started = performance.now();
const args = ["mcp", "run", "--exe", editor, "--session", "sdk-asset-" + process.pid,
    "--wait", "35", "--timeout", "100", "--", "--headless", "--content-root", project, ...state, ...process.argv.slice(2)];
const transcript = path.join(project, "mcp-transcript.jsonl");
fs.writeFileSync(transcript, "");
function log(direction, value) {
    fs.appendFileSync(transcript, JSON.stringify({ elapsedMs: Math.round(performance.now() - started), direction, value }) + "\n");
}
log("launch", [cli, ...args]);
const child = spawn(cli, args, { cwd: project, windowsHide: true });
const pending = new Map();
let nextId = 1;
child.stderr.on("data", data => log("stderr", data.toString()));
readline.createInterface({ input: child.stdout }).on("line", line => {
    let message;
    try {
        message = JSON.parse(line);
    } catch {
        log("stdout", line);
        return;
    }
    log("response", message);
    const request = pending.get(message.id);
    if (!request) return;
    pending.delete(message.id);
    clearTimeout(request.timer);
    if (message.error) request.reject(new Error(JSON.stringify(message.error)));
    else request.resolve(message.result);
});
function failPending(error) {
    for (const request of pending.values()) {
        clearTimeout(request.timer);
        request.reject(error);
    }
    pending.clear();
}
child.on("error", failPending);
child.stdin.on("error", failPending);
child.on("exit", (code, signal) => {
    log("exit", { code, signal });
    failPending(new Error("SDK CLI exited with " + (signal ?? code)));
});
function send(message) {
    log("request", message);
    child.stdin.write(JSON.stringify({ jsonrpc: "2.0", ...message }) + "\n");
}
function call(method, params = {}) {
    const id = nextId++;
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            pending.delete(id);
            reject(new Error("Timed out waiting for " + method));
        }, 45000);
        pending.set(id, { resolve, reject, timer });
        send({ id, method, params });
    });
}
async function tool(name, args = {}) {
    const output = await call("tools/call", { name, arguments: args });
    const reply = JSON.parse(output.content[0].text);
    if (!reply.ok) throw new Error(name + " " + JSON.stringify(reply));
    return reply.result;
}
async function main() {
    await call("initialize", { protocolVersion: "2024-11-05", capabilities: {},
        clientInfo: { name: "sdk-asset-" + process.pid, version: "1.0" } });
    send({ method: "notifications/initialized" });
    if (process.env.SPITE_FULL_TOOL_REGISTRY === "1") {
        const { tools } = await call("tools/list");
        fs.writeFileSync(path.join(project, "mcp-tools.json"), JSON.stringify(tools, null, 2));
    }
    const roots = await tool("roots");
    console.log(JSON.stringify(roots));
    const importRoot = roots.roots.find(root => root.name === "import");
    if (!importRoot || path.resolve(importRoot.directory) !== path.join(project, "models"))
        throw new Error("The import root did not resolve to the isolated project");
    const previous = await tool("assets", { type: "model" });
    if (!previous.assets.some(asset => asset.name === model)) {
        const imported = await tool("importModel", { path: source, wait: true });
        if (imported.status !== "done") throw new Error("Import did not finish");
    }
    await tool("imports");
    const assets = await tool("assets", { type: "model", bounds: true });
    const textures = await tool("assets", { type: "texture" });
    const mesh = assets.assets.find(asset => asset.name === model);
    const texture = textures.assets.find(asset => asset.name === "Amber and teal checks_baseColor");
    if (mesh?.bounds?.max[1] !== 1.5 || texture?.format !== "BC7 sRGB")
        throw new Error("Imported mesh or texture is missing from the manifest");
    await tool("newScene");
    const instance = await tool("instantiateModel", {
        model, name, transform: { position: [2, 0, -3], scale: [1, 1, 1] }
    });
    console.log(JSON.stringify({ instance }));
    await tool("find", { name });
    await tool("saveSceneAs", { path: scene, overwrite: true });
    await tool("sceneInfo");
    await tool("newScene");
    await tool("openScene", { path: scene });
    const reopened = await tool("find", { name, values: ["TransformComponent"] });
    console.log(JSON.stringify({ reopened }));
    const entity = reopened.entities[0];
    if (reopened.total !== 1 || entity?.name !== name ||
        JSON.stringify(entity?.TransformComponent?.position) !== "[2,0,-3]") {
        throw new Error("The saved scene did not restore the instance and position");
    }
    const document = JSON.parse(fs.readFileSync(path.join(project, "scenes", scene), "utf8"));
    const meshHandle = document.entities[0]?.components?.MeshComponent?.meshHandle;
    const materialHandle = document.entities[0]?.components?.MaterialComponent?.materialHandle;
    if (meshHandle?.name !== model || materialHandle?.name !== "Amber and teal checks") {
        throw new Error("The saved scene lost the mesh or material reference");
    }
    const beforeMaterial = await tool("material", { name: materialHandle.name });
    const texturePath = path.join(project, "assets_baked/textures/Amber and teal checks_baseColor.spitetex");
    const hash = file => createHash("sha256").update(fs.readFileSync(file)).digest("hex");
    const beforeTexture = hash(texturePath);
    const beforeSource = hash(path.join(project, "models", source));
    const generated = spawnSync(process.execPath, [path.join(__dirname, "MakeAsset.cjs")], {
        env: { ...process.env, SPITE_ASSET_OUTPUT: path.join(project, "models"), SPITE_ASSET_SEED: "732" }, encoding: "utf8"
    });
    if (generated.status !== 0) throw new Error("Regeneration failed: " + generated.stderr);
    validate(path.join(project, "models", source));
    const reimport = await tool("reimportModel", { name: model, wait: true });
    const afterAssets = await tool("assets", { type: "model", text: model });
    const afterTextures = await tool("assets", { type: "texture", text: "Amber and teal checks" });
    let afterEntity;
    const modelMaterials = afterAssets.assets.find(asset => asset.name === model)?.materials ?? [];
    for (let attempt = 0; attempt < 100; attempt++) {
        afterEntity = await tool("find", { name, values: ["MeshComponent", "MaterialComponent"] });
        if (modelMaterials.includes(afterEntity.entities[0]?.MaterialComponent?.materialHandle?.name)) break;
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    const live = afterEntity.entities[0];
    const newMaterialName = live?.MaterialComponent?.materialHandle?.name;
    const newMaterial = await tool("material", { name: newMaterialName });
    const newTextureName = newMaterial.fields.albedoTextureName;
    const newTexture = afterTextures.assets.find(asset => asset.name === newTextureName);
    const newTextureHash = newTexture && hash(path.join(project, newTexture.baked));
    const afterSource = hash(path.join(project, "models", source));
    await tool("saveSceneAs", { path: scene, overwrite: true });
    await tool("newScene");
    await tool("openScene", { path: scene });
    const persisted = await tool("find", { name, values: ["MeshComponent", "MaterialComponent"] });
    const evidence = { beforeMaterial, beforeTexture, beforeSource, afterSource, reimport, afterAssets, afterTextures,
        afterEntity, newMaterial, newTextureHash, persisted };
    fs.writeFileSync(path.join(project, "reimport-evidence.json"), JSON.stringify(evidence, null, 2));
    const persistedEntity = persisted.entities[0];
    if (reimport.status !== "done" || beforeSource === afterSource || !newTextureHash || beforeTexture === newTextureHash ||
        live?.MeshComponent?.meshHandle?.name !== model ||
        !afterAssets.assets.find(asset => asset.name === model)?.materials.includes(newMaterialName) ||
        persistedEntity?.MeshComponent?.meshHandle?.name !== model ||
        persistedEntity?.MaterialComponent?.materialHandle?.name !== newMaterialName)
        throw new Error("Changed-seed binding round trip failed; inspect reimport-evidence.json");
    console.log(JSON.stringify({ reimport: reimport.status, material: newMaterialName, texture: newTextureName,
        beforeTexture, newTextureHash, persisted: true }));
    await tool("shutdown");
}
main().catch(error => {
    console.error(error.message);
    process.exitCode = 1;
}).finally(() => child.stdin.end());
