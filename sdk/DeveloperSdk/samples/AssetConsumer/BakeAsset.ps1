param([string]$RunName = ('bake-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')))
$ErrorActionPreference = 'Stop'
if ($RunName -notmatch '^[a-zA-Z0-9-]+$') { throw 'RunName may contain letters, digits and hyphens.' }
if (-not $env:SPITE_SDK -or -not $env:SPITE_ASSET_OUTPUT) {
    throw 'Set SPITE_SDK and SPITE_ASSET_OUTPUT before running this sample.'
}
$sdk = (Resolve-Path $env:SPITE_SDK).Path
$output = (Resolve-Path $env:SPITE_ASSET_OUTPUT).Path
$baker = (Resolve-Path (Join-Path $sdk 'bin/SpiteBake.exe')).Path
$source = (Resolve-Path (Join-Path $output 'sdk_checker_bollard.glb')).Path
$proof = (New-Item -ItemType Directory (Join-Path $output $RunName)).FullName
$baked = Join-Path $proof 'assets_baked'
$time = [Diagnostics.Stopwatch]::StartNew()
& $baker $source $baked 2>&1 | Tee-Object -FilePath (Join-Path $proof 'bake.log') | Out-Host
$exit = $LASTEXITCODE
$time.Stop()
$files = @(Get-ChildItem $baked -File -Recurse -ErrorAction SilentlyContinue)
[ordered]@{
    baker = $baker
    source = $source
    sourceSha256 = (Get-FileHash $source -Algorithm SHA256).Hash
    output = $baked
    exitCode = $exit
    elapsedMs = $time.ElapsedMilliseconds
    files = @($files | ForEach-Object {
        [ordered]@{
            name = [IO.Path]::GetRelativePath($baked, $_.FullName)
            size = $_.Length
            sha256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
        }
    })
} | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 (Join-Path $proof 'bake-proof.json')
if ($exit -ne 0 -or $files.Count -eq 0) { throw "SDK bake failed with exit $exit. Evidence in $proof" }
Get-Content (Join-Path $proof 'bake-proof.json')
