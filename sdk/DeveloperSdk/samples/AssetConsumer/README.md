# SDK asset consumer

From this sample directory in PowerShell with Node.js 22.2 or later, set the
developer SDK path and choose a writable output directory.

```powershell
$env:SPITE_SDK = '<absolute path to the developer SDK>'
$env:SPITE_ASSET_OUTPUT = '<absolute path to a writable sample output directory>'
$env:SPITE_ASSET_SEED = '731'
node ./MakeAsset.cjs
node ./ValidateAsset.cjs "$env:SPITE_ASSET_OUTPUT/sdk_checker_bollard.glb"
./BakeAsset.ps1
node ./CheckEditor.cjs
```

The generator writes a GLB with a hexagonal mesh, UVs, face normals and an embedded
seeded checker texture. Set `SPITE_ASSET_SEED` to an unsigned 32-bit integer
(default 731) to change the texture. The node has a stable `spite_source_id`;
keep that ID across regeneration. This alone does not verify material or texture
identity on reimport.
The standalone baker writes a timestamped folder containing the baked assets,
its log, and a SHA-256 inventory. The editor check runs the staged editor via
`spite mcp run` in headless mode, which still requires a Vulkan device. It copies
stock content into `EditorProject` under the output directory and assigns writable
host state there.

The editor check discovers the tools and their import and scene roots through
MCP. It imports the GLB, checks the model bounds and BC7 sRGB texture, places one
instance, and saves and reopens `scenes/sdk-asset-acceptance.json`. It verifies the
restored position and the saved mesh and material references. It regenerates seed 732,
reimports, waits for the instance's binding to match the new model material, checks
the material's texture and changed baked texture hash, then saves and reopens again.
The reimport may allocate suffixed material/texture names: use returned bindings,
not guessed names. `reimport-evidence.json` records the mapping and hashes. Start
with seed 731 and a fresh project; this check overwrites its generated source and scene.
`ValidateAsset.cjs` checks the sample's tightly packed GLB, finite geometry, index
bounds, unit normals, nondegenerate winding, UV range and node ID; it is not a general
glTF validator or proof of visual quality. The project contains a transcript of the calls used by this check. Set
`SPITE_FULL_TOOL_REGISTRY=1` to request and save the full MCP registry in
`mcp-tools.json`; it is large and also appears in the transcript. To exercise import again, choose a fresh
output directory or set `SPITE_ASSET_CONSUMER_PROJECT` to a new project name.
