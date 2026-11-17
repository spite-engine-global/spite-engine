# Run the shipped Windows consumer samples from an extracted SDK.
param([Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
$sdk = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))

if (!$IsWindows -or !(Test-Path -LiteralPath (Join-Path $sdk 'sdk-manifest.json') -PathType Leaf)) {
    throw 'Run VerifySdk.ps1 from the extracted Windows SDK with PowerShell 7.'
}
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputRoot)
if ($output -eq $sdk -or $output.StartsWith("$sdk\", [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose an output folder outside the SDK installation.'
}
if (Test-Path -LiteralPath $output) { throw "Output path already exists: $output" }

$assetOutput = Join-Path $output 'asset'
$assetSample = Join-Path $sdk 'samples/AssetConsumer'
$savedSdk = $env:SPITE_SDK
$savedAssetOutput = $env:SPITE_ASSET_OUTPUT
$savedProject = $env:SPITE_ASSET_CONSUMER_PROJECT
[void][IO.Directory]::CreateDirectory($assetOutput)
Push-Location -LiteralPath $output
try {
    $env:SPITE_SDK = $sdk
    $env:SPITE_ASSET_OUTPUT = $assetOutput
    $env:SPITE_ASSET_CONSUMER_PROJECT = 'EditorProject'
    & node (Join-Path $assetSample 'MakeAsset.cjs')
    if ($LASTEXITCODE -ne 0) { throw 'GLB generation failed.' }
    & (Join-Path $assetSample 'BakeAsset.ps1')
    & node (Join-Path $assetSample 'CheckEditor.cjs')
    if ($LASTEXITCODE -ne 0) { throw 'Editor import and scene roundtrip failed.' }
    & (Join-Path $sdk 'samples/RenderConsumer/Verify.ps1') -Sdk $sdk -ProjectRoot (Join-Path $output 'render')
    "PASS: SDK asset and rendering samples. Evidence in $output"
} finally {
    $env:SPITE_SDK = $savedSdk
    $env:SPITE_ASSET_OUTPUT = $savedAssetOutput
    $env:SPITE_ASSET_CONSUMER_PROJECT = $savedProject
    Pop-Location
}
