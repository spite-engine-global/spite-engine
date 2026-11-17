# Install the SPITE SDK and its global `spite` command (Windows x64). Non-interactive, no admin.
# Runs in Windows PowerShell 5.1 and PowerShell 7:
#
#   Download this asset through the authenticated GitHub API as shown below.
#
# Environment (all optional):
#   SPITE_VERSION        install this version (for example 0.2.0) instead of the latest release
#   SPITE_HOME           install location (default %LOCALAPPDATA%\Spite)
#   SPITE_GITHUB_TOKEN   GitHub token for a private release repository (GITHUB_TOKEN is read as well)
#   SPITE_RELEASE_REPO   release repository (default spite-engine-global/spite-engine)
#   SPITE_RELEASE_API    GitHub REST API base URL (default https://api.github.com)
#   SPITE_NO_PATH=1      leave the user PATH alone
#
# While the repository is private the one-liner above does not resolve; fetch this script through the
# API with the token (asset ids are listed by GET /repos/spite-engine-global/spite-engine/releases/latest):
#   $h = @{ Authorization = "Bearer $env:SPITE_GITHUB_TOKEN"; Accept = 'application/octet-stream' }
#   irm -Headers $h https://api.github.com/repos/spite-engine-global/spite-engine/releases/assets/<install.ps1 asset id> | iex

function Install-Spite {
    $ErrorActionPreference = 'Stop'
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

    $repo = if ($env:SPITE_RELEASE_REPO) { $env:SPITE_RELEASE_REPO } else { 'spite-engine-global/spite-engine' }
    $api = if ($env:SPITE_RELEASE_API) { $env:SPITE_RELEASE_API.TrimEnd('/') } else { 'https://api.github.com' }
    $token = if ($env:SPITE_GITHUB_TOKEN) { $env:SPITE_GITHUB_TOKEN } else { $env:GITHUB_TOKEN }
    $target = 'windows-x86_64'

    $arch = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    if ($env:OS -ne 'Windows_NT' -or $arch -ne 'AMD64') {
        throw "install.ps1: unsupported system; this installer supports Windows x64 (Linux: install.sh)"
    }
    $spiteHome = if ($env:SPITE_HOME) { [IO.Path]::GetFullPath($env:SPITE_HOME) } else { Join-Path $env:LOCALAPPDATA 'Spite' }
    if ($env:SPITE_HOME) { $env:SPITE_HOME = $spiteHome }

    function Stop-Http($what, $status, $message) {
        if ($status -eq 401) { throw "install.ps1: ${what}: GitHub rejected the token (HTTP 401); check SPITE_GITHUB_TOKEN" }
        if ($status -eq 403 -or $status -eq 429) { throw "install.ps1: ${what}: GitHub refused the request (HTTP $status): rate limit or a token without access; set SPITE_GITHUB_TOKEN" }
        if ($status -eq 404 -and $token) { throw "install.ps1: ${what}: not found (HTTP 404); check SPITE_VERSION, SPITE_RELEASE_REPO and that the token can read the repository" }
        if ($status -eq 404) { throw "install.ps1: ${what}: not found (HTTP 404); a private repository needs a token: set SPITE_GITHUB_TOKEN" }
        if ($status) { throw "install.ps1: ${what}: HTTP $status" }
        throw "install.ps1: ${what}: $message"
    }

    # Redirects are followed by hand: Windows PowerShell 5.1 would send the GitHub token on to the storage host
    # behind an asset download, which rejects it.
    function Get-Github($url, $accept, $out, $what) {
        $origin = ([Uri]$url).Host
        for ($hop = 0; $hop -lt 6; $hop++) {
            $request = [Net.HttpWebRequest]::Create($url)
            $request.AllowAutoRedirect = $false
            $request.UserAgent = 'spite-install'
            $request.Accept = $accept
            if ($token -and ([Uri]$url).Host -eq $origin) { $request.Headers['Authorization'] = "Bearer $token" }
            try {
                $response = $request.GetResponse()
            } catch [Net.WebException] {
                $response = $_.Exception.Response
                if (-not $response) { Stop-Http $what 0 "cannot reach ${url}: $($_.Exception.Message)" }
            }
            try {
                $status = [int]$response.StatusCode
                if ($status -ge 300 -and $status -lt 400) {
                    $url = (New-Object Uri -ArgumentList ([Uri]$url), $response.Headers['Location']).AbsoluteUri
                    continue
                }
                if ($status -ne 200) { Stop-Http $what $status '' }
                $stream = $response.GetResponseStream()
                if ($out) {
                    $file = [IO.File]::Create($out)
                    try { $stream.CopyTo($file) } finally { $file.Dispose() }
                    return
                }
                $reader = New-Object IO.StreamReader -ArgumentList $stream, ([Text.Encoding]::UTF8)
                return $reader.ReadToEnd()
            } finally {
                $response.Close()
            }
        }
        Stop-Http $what 0 'too many redirects'
    }

    $work = Join-Path ([IO.Path]::GetTempPath()) ('spite-install-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work | Out-Null
    try {
        $endpoint = if ($env:SPITE_VERSION) { 'tags/v' + $env:SPITE_VERSION.TrimStart('v') } else { 'latest' }
        Write-Host "Looking up the SPITE release ($repo, $endpoint)"
        $json = Get-Github "$api/repos/$repo/releases/$endpoint" 'application/vnd.github+json' $null 'release lookup'
        $release = $json | ConvertFrom-Json
        $tag = [string]$release.tag_name
        if (-not $tag) { throw 'install.ps1: the release has no tag name' }
        $version = $tag.TrimStart('v')

        $archive = @($release.assets | Where-Object { $_.name -like 'SPITE-SDK-*' -and $_.name -like "*-$target.zip" })[0]
        if (-not $archive) { throw "install.ps1: release $tag has no SPITE-SDK archive for $target" }
        $sums = @($release.assets | Where-Object { $_.name -eq 'SHA256SUMS' })[0]
        if (-not $sums) { throw "install.ps1: release $tag has no SHA256SUMS to verify $($archive.name) against" }

        $sumsFile = Join-Path $work 'SHA256SUMS'
        Get-Github $sums.url 'application/octet-stream' $sumsFile 'SHA256SUMS'
        $expected = $null
        foreach ($line in [IO.File]::ReadAllLines($sumsFile)) {
            $parts = $line.Trim() -split '\s+', 2
            if ($parts.Count -eq 2 -and ($parts[1].TrimStart('*') -eq $archive.name)) { $expected = $parts[0].ToLowerInvariant(); break }
        }
        if (-not $expected) { throw "install.ps1: SHA256SUMS does not list $($archive.name)" }

        Write-Host "Downloading $($archive.name)"
        $zip = Join-Path $work $archive.name
        Get-Github $archive.url 'application/octet-stream' $zip "download of $($archive.name)"
        $actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected) { throw "install.ps1: SHA-256 mismatch for $($archive.name): expected $expected, got $actual" }

        Write-Host 'Extracting'
        $extract = Join-Path $work 'extract'
        New-Item -ItemType Directory -Path $extract | Out-Null
        $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
        if (Test-Path -LiteralPath $tar) {
            & $tar -xf $zip -C $extract
            if ($LASTEXITCODE -ne 0) { throw "install.ps1: cannot extract $($archive.name)" }
        } else {
            Expand-Archive -LiteralPath $zip -DestinationPath $extract
        }
        $root = $null
        if (Test-Path -LiteralPath (Join-Path $extract 'sdk-manifest.json')) {
            $root = $extract
        } else {
            $root = (Get-ChildItem -LiteralPath $extract -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'sdk-manifest.json') } | Select-Object -First 1).FullName
        }
        if (-not $root) { throw "install.ps1: $($archive.name) does not contain an SDK (no sdk-manifest.json)" }
        $exe = Join-Path $root 'bin\spite.exe'
        if (-not (Test-Path -LiteralPath $exe)) { throw "install.ps1: $($archive.name) has no bin\spite.exe" }

        & $exe self install --from $root
        if ($LASTEXITCODE -ne 0) { throw "install.ps1: spite self install failed (exit code $LASTEXITCODE)" }
        $spite = Join-Path $spiteHome 'bin\spite.exe'
        if (-not (Test-Path -LiteralPath $spite)) { throw "install.ps1: spite self install did not create $spite" }

        Write-Host ''
        Write-Host "SPITE $version is installed."
        Write-Host "  spite: $spite"
        if ($env:SPITE_NO_PATH) {
            Write-Host '  PATH was left unchanged (SPITE_NO_PATH); call spite by the path above.'
        } else {
            $bin = Split-Path $spite -Parent
            if (@($env:Path -split ';') -notcontains $bin) { $env:Path = "$bin;$env:Path" }
            Write-Host '  New shells find spite on PATH; this shell already does.'
        }
        Write-Host ''
        $report = ''
        $doctor = $null
        try {
            $report = (& $spite doctor --json | Out-String).Trim()
            $doctor = $report | ConvertFrom-Json
        } catch { }
        $failed = @()
        if ($doctor -and $doctor.checks) { $failed = @($doctor.checks | Where-Object { $_.ok -eq $false }) }
        if ($doctor -and $doctor.ok -eq $true) {
            Write-Host 'Ready. Try: spite new games/MyGame'
        } elseif ($doctor -and $doctor.fix -and $doctor.fix.needsAdmin -eq $false) {
            Write-Host "One step left: run 'spite setup' (downloads build tools; no administrator approval needed)"
        } elseif (($doctor -and $doctor.fix -and $doctor.fix.command -eq 'spite setup') -or
                  @($failed | Where-Object { $_.check -eq 'compiler-abi' }).Count -gt 0) {
            Write-Host "One step left: run 'spite setup' (asks for administrator approval once)"
        } else {
            $detail = (@($failed | ForEach-Object { if ($_.error) { [string]$_.error } else { [string]$_.check } }) -join '; ')
            if (-not $detail) { $detail = if ($report) { $report } else { 'no output' } }
            Write-Host "spite doctor reported a problem: $detail"
        }
    } finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Install-Spite
