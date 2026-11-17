# Native compiler provisioning helpers; dot-source for fixture tests, never installs on import.
function Assert-SpiteBootstrapper($Path, $Pin) {
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Pin.sha256) { throw 'Compiler bootstrapper SHA-256 mismatch.' }
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw 'Compiler bootstrapper must have a valid Microsoft Authenticode signature.'
    }
    if ((Get-Item -LiteralPath $Path).VersionInfo.ProductVersion -ne $Pin.productVersion) { throw 'Compiler bootstrapper version differs from the tested pin.' }
}

function Get-SpiteCompilerBootstrapper($Pin, [string]$Cache, [string]$OfflineLayout) {
    if ($OfflineLayout) {
        $path = Join-Path $OfflineLayout 'vs_BuildTools.exe'
        if (!(Test-Path -LiteralPath (Join-Path $OfflineLayout 'Catalog.json')) -or
            !(Test-Path -LiteralPath (Join-Path $OfflineLayout 'ChannelManifest.json'))) {
            throw 'Offline layout is incomplete: Catalog.json and ChannelManifest.json are required.'
        }
    } else {
        [void][IO.Directory]::CreateDirectory($Cache)
        $path = Join-Path $Cache 'vs_BuildTools.exe'
        if (!(Test-Path -LiteralPath $path)) {
            $partial = "$path.$([guid]::NewGuid().ToString('N')).download"
            try {
                [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
                Invoke-WebRequest -UseBasicParsing -Uri $Pin.bootstrapperUrl -OutFile $partial
                Assert-SpiteBootstrapper $partial $Pin
                [IO.File]::Move($partial, $path)
            } finally {
                if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
            }
        }
    }
    Assert-SpiteBootstrapper $path $Pin
    return [IO.Path]::GetFullPath($path)
}

function Get-SpiteInstallArguments($Pin, [string]$InstallPath, [string]$OfflineLayout) {
    $options = @('--quiet', '--wait', '--norestart', '--installPath', $InstallPath)
    foreach ($component in $Pin.components) { $options += '--add', $component }
    if ($OfflineLayout) {
        $channel = Join-Path $OfflineLayout 'ChannelManifest.json'
        $options += '--noWeb', '--noUpdateInstaller', '--channelUri', $channel, '--installChannelUri', $channel,
            '--installCatalogUri', (Join-Path $OfflineLayout 'Catalog.json')
    }
    return $options
}

function Invoke-SpiteCompilerProcess([string]$Bootstrapper, [string[]]$Options) {
    # Start-Process on PS5.1 joins arguments; quote each value, refuse embedded quotes.
    $quoted = foreach ($option in $Options) {
        if ($option.Contains('"') -or $option.EndsWith('\')) { throw 'Installer argument contains unsupported quoting or a trailing separator.' }
        '"' + $option + '"'
    }
    $process = Start-Process -FilePath $Bootstrapper -ArgumentList ($quoted -join ' ') -WindowStyle Hidden -PassThru -Wait
    return $process.ExitCode
}

function Invoke-SpiteCompilerSetup($Pin, [string]$Cache, [string]$OfflineLayout, [string]$InstallPath,
    [scriptblock]$Probe, [scriptblock]$IsAdministrator) {
    if (& $Probe) { return 0 }
    if (!(& $IsAdministrator)) {
        Write-Error 'Compiler provisioning requires an elevated PowerShell. Rerun Setup.ps1 -Install as administrator; no automatic UAC/restart is performed.' -ErrorAction Continue
        return 740
    }
    # A dedicated new instance avoids silently upgrading another development installation.
    if (Test-Path -LiteralPath $InstallPath) {
        Write-Error 'Provisioning destination already exists but failed ABI validation. Choose a new -CompilerInstallPath; existing installations are preserved.' -ErrorAction Continue
        return 1
    }
    $bootstrapper = Get-SpiteCompilerBootstrapper $Pin $Cache $OfflineLayout
    $code = Invoke-SpiteCompilerProcess $bootstrapper (Get-SpiteInstallArguments $Pin $InstallPath $OfflineLayout)
    if ($code -eq 3010 -or $code -eq 1641) {
        Write-Warning "Compiler installation requires reboot ($code). Restart explicitly, then rerun setup; ABI validation has not passed."
        return $code
    }
    if ($code -ne 0) {
        Write-Error "Compiler installer failed/cancelled (exit $code). See TEMP dd_bootstrapper/dd_setup logs; no setup success recorded." -ErrorAction Continue
        return $code
    }
    if (!(& $Probe)) {
        Write-Error 'Installer returned success but the required native compiler ABI still fails. Inspect doctor output; setup did not succeed.' -ErrorAction Continue
        return 1
    }
    return 0
}
