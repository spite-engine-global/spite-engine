$ErrorActionPreference = 'Stop'
$sdkArguments = @($args)
try {
    . (Join-Path $PSScriptRoot 'Bootstrap.ps1')
    $python = Resolve-SpitePython
    $env:SPITE_BOOTSTRAP_PYTHON = $python
    & $python -B (Join-Path $PSScriptRoot 'Sdk.py') @sdkArguments
    exit $LASTEXITCODE
} catch { Write-Error $_ -ErrorAction Continue; exit 1 }
