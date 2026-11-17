function Resolve-SpitePython([switch]$Install, [string]$OfflineTools) {
    $pins = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'Toolchain.json') -Raw | ConvertFrom-Json
    $pin = $pins.python
    $artifact = $pin.'windows-x86_64'
    $sdkRoot = Split-Path $PSScriptRoot -Parent
    $key = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($sdkRoot))).Replace('-', '').ToLower().Substring(0,24)
    $defaultCache = Join-Path $env:LOCALAPPDATA 'SpiteSdk/tools'
    $pointer = Join-Path $defaultCache "selections/$key.cache"
    $cache = $env:SPITE_TOOLS_CACHE
    if ($cache) {
        $cache = [IO.Path]::GetFullPath($cache)
        [void][IO.Directory]::CreateDirectory((Split-Path $pointer -Parent))
        [IO.File]::WriteAllText($pointer, $cache, [Text.UTF8Encoding]::new($false))
    } elseif (Test-Path -LiteralPath $pointer) { $cache = [IO.File]::ReadAllText($pointer).Trim() }
    else { $cache = $defaultCache }
    $selection = Join-Path $cache "selections/$key.json"
    $previous = if (Test-Path -LiteralPath $selection) { (Get-Content -LiteralPath $selection -Raw | ConvertFrom-Json).python } else { $null }
    $explicit = $env:SPITE_PYTHON
    if (!$explicit -and $previous.explicit) { $explicit = $previous.path }
    $destination = Join-Path $cache "python/$($pin.version)"
    $candidates = @()
    if ($explicit) { $candidates += $explicit }
    else {
        $candidates += Join-Path $destination $artifact.executable
        if ($env:SPITE_MANAGED_TOOLS -ne '1') {
            if ($previous.path) { $candidates += $previous.path }
            $candidates += @(Get-Command python.exe,python3.exe -All -ErrorAction SilentlyContinue |
                Where-Object { $_.Source -notlike '*\WindowsApps\*' } | Select-Object -ExpandProperty Source)
            $candidates += @(Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA 'Programs/Python/Python*/python.exe') -ErrorAction SilentlyContinue |
                Select-Object -ExpandProperty FullName)
        }
    }
    $rejections = @()
    foreach ($candidate in $candidates | Select-Object -Unique) {
        if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) {
            if ($explicit) { $rejections += "${candidate}: file not found" }
            continue
        }
        try {
            $errorFile = [IO.Path]::GetTempFileName()
            $previousErrorAction = $ErrorActionPreference
            try {
                $ErrorActionPreference = 'Continue'
                $result = & $candidate -I -c 'import sys,struct;print(''.''.join(map(str,sys.version_info[:3])));print(struct.calcsize(''P'')*8)' 2> $errorFile
                $exitCode = $LASTEXITCODE
                $stderr = Get-Content -LiteralPath $errorFile -ErrorAction SilentlyContinue | Select-Object -First 1
            } finally {
                $ErrorActionPreference = $previousErrorAction
                Remove-Item -LiteralPath $errorFile -Force -ErrorAction SilentlyContinue
            }
            if ($exitCode -eq 0 -and $result.Count -eq 2 -and $result[1] -eq '64' -and
                [version]$result[0] -ge [version]$pin.minimum -and [version]$result[0] -lt [version]$pin.maximumExclusive) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
            $rejections += "${candidate}: exit $exitCode; stderr: $stderr; output: $($result -join ', ') (requires 64-bit Python >= $($pin.minimum), < $($pin.maximumExclusive))"
        } catch { $rejections += "${candidate}: $($_.Exception.Message)" }
    }
    $details = if ($rejections.Count) { " Rejected candidates: $($rejections -join '; ')" } else { '' }
    if ($explicit) { throw "Explicit Python is incompatible: $explicit.$details" }
    if (!$Install) { throw "Compatible Python was not found. Run Setup.ps1 -Install; no download performed.$details" }
    if (Test-Path -LiteralPath $destination) { throw "Cached Python failed validation; inspect $destination before replacing it." }
    $parent = Split-Path $destination -Parent
    [void][IO.Directory]::CreateDirectory($parent)
    $stage = Join-Path $parent ('.install-' + [guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($stage)
    try {
        $filename = [IO.Path]::GetFileName(([Uri]$artifact.url).AbsolutePath)
        $archive = Join-Path $stage $filename
        if ($OfflineTools) { Copy-Item -LiteralPath (Join-Path $OfflineTools $filename) -Destination $archive }
        else {
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri $artifact.url -OutFile $archive
        }
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ine $artifact.sha256) { throw 'Python archive checksum mismatch.' }
        $unpacked = Join-Path $stage 'python'
        Expand-Archive -LiteralPath $archive -DestinationPath $unpacked
        [IO.Directory]::Move($unpacked, $destination)
    } finally {
        $resolvedStage = [IO.Path]::GetFullPath($stage)
        if ([IO.Path]::GetDirectoryName($resolvedStage) -eq [IO.Path]::GetFullPath($parent) -and
            [IO.Path]::GetFileName($resolvedStage) -match '^\.install-[0-9a-f]{32}$') {
            Remove-Item -LiteralPath $resolvedStage -Recurse -Force
        }
    }
    return Resolve-SpitePython
}
