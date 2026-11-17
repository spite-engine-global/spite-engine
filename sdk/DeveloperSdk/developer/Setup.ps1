# Downloads, compiler provisioning and PATH changes are explicit.
param([ValidateSet('editor','build')][string]$Profile = 'build', [switch]$Install,
    [switch]$ManagedTools, [Alias('Python')][string]$PythonExecutable,
    [Alias('CMake')][string]$CMakeExecutable, [string]$ToolsCache, [string]$OfflineTools,
    [switch]$AddToPath, [string]$OfflineLayout,
    [string]$CacheDirectory = (Join-Path $env:LOCALAPPDATA 'SpiteSdk/setup/msvc-18.10.4'),
    [string]$CompilerInstallPath = (Join-Path $env:ProgramFiles 'SpiteSdk/BuildTools-18.10.4'))
$ErrorActionPreference = 'Stop'
$previousRefresh = $env:SPITE_REFRESH_TOOLS
try {
    if ($PythonExecutable) { $env:SPITE_PYTHON = $PythonExecutable }
    if ($CMakeExecutable) { $env:SPITE_CMAKE = $CMakeExecutable }
    if ($ToolsCache) { $env:SPITE_TOOLS_CACHE = $ToolsCache }
    if ($ManagedTools) { $env:SPITE_MANAGED_TOOLS = '1' }
    $env:SPITE_REFRESH_TOOLS = '1'
    . (Join-Path $PSScriptRoot 'internal/Bootstrap.ps1')
    $bootstrapPython = Resolve-SpitePython -Install:$Install -OfflineTools $OfflineTools
    $toolArgs = @('--profile', $Profile)
    if ($Install) { $toolArgs += '--install' }
    if ($OfflineTools) { $toolArgs += '--offline-tools', $OfflineTools }
    & $bootstrapPython -B (Join-Path $PSScriptRoot 'internal/Toolchain.py') @toolArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $PSScriptRoot 'sdk.cmd') doctor --profile editor --json | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'SDK platform or Python is incompatible; compiler installation was not attempted.' }
    $doctor = { & (Join-Path $PSScriptRoot 'sdk.cmd') doctor --profile $Profile --json | Out-Host; return ($LASTEXITCODE -eq 0) }
    $ok = & $doctor
    if (!$ok -and $Install -and $Profile -eq 'build') {
        . (Join-Path $PSScriptRoot 'internal/SetupCompiler.ps1')
        $pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'internal/CompilerSetup.json') -Raw | ConvertFrom-Json
        $manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'sdk-manifest.json') -Raw | ConvertFrom-Json
        if ($manifest.toolsetStamp -notlike "msvc$($pin.mscVer)-*") { throw 'Compiler provisioning pin does not match SDK ABI.' }
        $admin = { ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
        $code = Invoke-SpiteCompilerSetup $pin $CacheDirectory $OfflineLayout $CompilerInstallPath $doctor $admin
        if ($code -ne 0) { exit $code }
        $ok = $true
    }
    if (!$ok) { exit 1 }
    if ($AddToPath) {
        $parts = @([Environment]::GetEnvironmentVariable('Path', 'User') -split ';' | Where-Object { $_ })
        foreach ($directory in @((Join-Path $PSScriptRoot 'bin'), $PSScriptRoot)) {
            if ($parts -notcontains $directory) { $parts += $directory }
        }
        [Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User')
        Write-Host 'SDK/bin and SDK root added to your user PATH. Open a new terminal.'
    }
} catch { Write-Error $_ -ErrorAction Continue; exit 1 }
finally { $env:SPITE_REFRESH_TOOLS = $previousRefresh }
