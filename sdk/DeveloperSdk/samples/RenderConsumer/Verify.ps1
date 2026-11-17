param(
    [Parameter(Mandatory)][string]$Sdk,
    [Parameter(Mandatory)][string]$ProjectRoot,
    [Parameter(Mandatory)][string]$ControlCli
)
$ErrorActionPreference = 'Stop'
$env:SPITE_USAGE_DISABLE = '1'
$sdk = (Resolve-Path -LiteralPath $Sdk).Path
& "$sdk/sdk.cmd" new $ProjectRoot --content-only | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Project creation failed.' }
$project = & "$sdk/sdk.cmd" project $ProjectRoot | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Project launch information failed.' }
$cli = (Resolve-Path -LiteralPath $ControlCli).Path
$session = "sdk-stripe-$PID"
$build = Join-Path $project.ProjectRoot 'build'
$hostArgs = $project.HostArgs
$shaderBin = Join-Path $project.ContentRoot 'shaders/bin'
$shaderSource = Join-Path $project.ContentRoot 'shaders/source'

function Invoke-Request($request, [switch]$ExpectFailure) {
    $wire = $request | ConvertTo-Json -Depth 5 -Compress
    $reply = & $cli request --session $session $wire | ConvertFrom-Json
    if (!$reply -or ([bool]$reply.ok -eq [bool]$ExpectFailure)) {
        throw "Unexpected reply to ${wire}: $($reply | ConvertTo-Json -Depth 8 -Compress)"
    }
    return $reply
}

function Assert-StripePixel($path, [bool]$expected) {
    $env:SPITE_STRIPE_CAPTURE = $path
    try {
        $pixel = & powershell.exe -NoProfile -Command '
            Add-Type -AssemblyName System.Drawing
            $image = [Drawing.Bitmap]::FromFile($env:SPITE_STRIPE_CAPTURE)
            try {
                if ($image.Width -lt 32 -or $image.Height -lt 32) { throw "View is too small" }
                $color = $image.GetPixel([int]($image.Width * 0.35), [int]($image.Height * 0.05))
                $biased = $image.GetPixel([int]($image.Width * 0.65), [int]($image.Height * 0.05))
                "$($color.R),$($color.G),$($color.B);$($biased.R),$($biased.G),$($biased.B)"
            } finally { $image.Dispose() }
        '
        if ($LASTEXITCODE -ne 0 -or !$pixel -or (($pixel -eq '255,0,255;0,255,255') -ne $expected)) {
            throw "Unexpected stripe pixel in ${path}: $pixel"
        }
    } finally {
        Remove-Item Env:SPITE_STRIPE_CAPTURE
    }
}

function Assert-HdrPixel($path, [string]$dominant) {
    $env:SPITE_HDR_CAPTURE = $path
    try {
        $pixel = & powershell.exe -NoProfile -Command '
            Add-Type -AssemblyName System.Drawing
            $image = [Drawing.Bitmap]::FromFile($env:SPITE_HDR_CAPTURE)
            try {
                if ($image.Width -lt 32 -or $image.Height -lt 32) { throw "View is too small" }
                $c = $image.GetPixel([int]($image.Width * 0.15), [int]($image.Height * 0.45))
                "$($c.R),$($c.G),$($c.B)"
            } finally { $image.Dispose() }
        '
        if ($LASTEXITCODE -ne 0 -or !$pixel) { throw "Cannot read HDR witness in ${path}: $pixel" }
        $rgb = @($pixel.Split(',') | ForEach-Object { [int]$_ })
        $channel = if ($dominant -eq 'red') { 0 } else { 1 }
        $other = 1 - $channel
        if ($rgb.Count -ne 3 -or $rgb[$channel] -lt 40 -or
            $rgb[$channel] -le ($rgb[$other] + 20) -or $rgb[$channel] -le ($rgb[2] + 20)) {
            throw "Expected $dominant HDR witness in ${path}, got $pixel"
        }
    } finally { Remove-Item Env:SPITE_HDR_CAPTURE }
}

function Get-Exposure {
    $result = (Invoke-Request @{query = 'tableRows'; args = @{
        service = 'AutoExposureSingleton'; table = 'exposure'; limit = 100
    }}).result
    $row = @($result.rows | Where-Object { $_.view -eq 0 })
    if ($row.Count -ne 1) { throw "Expected exposure row for view 0: $($result | ConvertTo-Json -Depth 8 -Compress)" }
    return $row[0]
}

function Wait-ExposureIdle {
    $last = $null
    $stable = 0
    for ($i = 0; $i -lt 1200; ++$i) {
        $row = Get-Exposure
        if (!$row.enabled -or $row.frozen) { throw "Exposure is disabled or frozen: $($row | ConvertTo-Json -Compress)" }
        if ($row.converged -and $null -ne $last -and $row.recordedRuns -eq $last) { ++$stable } else { $stable = 0 }
        if ($stable -ge 8) { return $row }
        $last = $row.recordedRuns
    }
    throw "Exposure did not settle within 1200 queries: $($row | ConvertTo-Json -Compress)"
}


$tools = & "$sdk/sdk.cmd" tools --profile build | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'Run SDK setup for compatible Python and CMake.' }
$python = $tools.python.path
& $python -B -c 'import pathlib,sys; sdk=pathlib.Path(sys.argv[1]); build=pathlib.Path(sys.argv[2]); source=pathlib.Path(sys.argv[3]); sys.path.insert(0,str(sdk / "internal")); import Sdk; args=Sdk.configure_arguments(build); Sdk.run([Sdk.cmake(),"-S",source,"-B",build,"-DSPITE_SDK="+str(sdk),"-DCMAKE_BUILD_TYPE=Release",*args],source); Sdk.run([Sdk.cmake(),"--build",build],source)' $sdk $build $PSScriptRoot
if ($LASTEXITCODE -ne 0) { throw 'Module build failed' }
$module = Join-Path $build 'RenderConsumer.dll'

$run = & $cli run --exe (Join-Path $sdk 'bin/SpiteGame.exe') --detach --session $session --timeout 180 --wait 30 '--' @hostArgs |
    ConvertFrom-Json
if (!$run.ok) { throw 'Game host did not start' }
try {
    $load = @{command = 'loadModule'; args = @{path = $module}}
    $rows = @{query = 'tableRows'; args = @{service = 'RenderGraph'; table = 'passes'; limit = 10000}}
    $filtered = @{query = 'tableRows'; args = @{service = 'RenderGraph'; table = 'passes'; filter = 'SdkColorStripe'}}
    $null = Invoke-Request $load -ExpectFailure
    if ((Invoke-Request $filtered).result.matched -ne 0) { throw 'Failed load left a render pass' }

    $compiled = Join-Path $build 'compiled'
    & "$sdk/bin/ShaderCompiler.exe" --sources (Join-Path $PSScriptRoot 'shaders') --output $compiled --spirv-only
    if ($LASTEXITCODE -ne 0) { throw 'Shader compile failed' }
    Copy-Item "$compiled/*.spv","$compiled/*.reflect.json" $shaderBin
    $null = Invoke-Request $load
    $passes = (Invoke-Request $rows).result.rows
    $composite = [Array]::IndexOf($passes.pass, 'Composite')
    $stripe = [Array]::IndexOf($passes.pass, 'SdkColorStripe')
    if ($composite -lt 0 -or $stripe -le $composite -or !$passes[$stripe].active -or $passes[$stripe].culled) {
        throw 'The active stripe pass must follow Composite'
    }

    $null = Invoke-Request @{command = 'setConfig'; args = @{path = 'graphics.aa'; value = 'taa'}}
    $camera = (Invoke-Request @{command = 'createEntity'; args = @{name = 'HdrWitnessCamera'; world = 'play'}}).result.id
    $null = Invoke-Request @{command = 'addComponent'; args = @{
        id = $camera; world = 'play'; component = 'CameraComponent'; value = @{autoExposure = $true}
    }}
    $null = Invoke-Request @{command = 'bindCamera'; args = @{view = 0; id = $camera; world = 'play'}}
    $null = Wait-ExposureIdle
    $hdrCapture = @{command = 'screenshot'; args = @{path = 'sdk-hdr-red.png'; settle = 8}}
    Assert-HdrPixel (Invoke-Request $hdrCapture).result.path 'red'
    $before = Wait-ExposureIdle
    $null = Invoke-Request @{command = 'renderConsumerSetHdrColor'; args = @{view = 0; color = 1}}
    $after = Wait-ExposureIdle
    if ($after.recordedRuns -le $before.recordedRuns) {
        throw "Exposure did not wake for changed HDR picture: before=$($before.recordedRuns), after=$($after.recordedRuns)"
    }
    $hdrCapture.args.path = 'sdk-hdr-green.png'
    Assert-HdrPixel (Invoke-Request $hdrCapture).result.path 'green'
    $beforeRepeat = Wait-ExposureIdle
    $null = Invoke-Request @{command = 'renderConsumerSetHdrColor'; args = @{view = 0; color = 1}}
    $afterRepeat = Wait-ExposureIdle
    if ($afterRepeat.recordedRuns -ne $beforeRepeat.recordedRuns) {
        throw "Repeated HDR preset woke exposure: before=$($beforeRepeat.recordedRuns), after=$($afterRepeat.recordedRuns)"
    }


    $capture = @{command = 'screenshot'; args = @{path = 'sdk-stripe-active.png'; settle = 8}}
    $active = (Invoke-Request $capture).result.path
    Assert-StripePixel $active $true

    $fragment = Join-Path $shaderSource 'sdk_stripe.frag.slang'
    Copy-Item (Join-Path $PSScriptRoot 'shaders/sdk_stripe.frag.slang') $fragment
    Add-Content -LiteralPath $fragment -Value 'bad invalid syntax'
    $spirv = Join-Path $shaderBin 'sdk_stripe.frag.spv'
    $hash = (Get-FileHash -LiteralPath $spirv).Hash
    try {
        $rejected = Invoke-Request @{command = 'reloadShaders'} -ExpectFailure
        if (!($rejected.details.failed -like 'sdk_stripe.frag.slang:*') -or
            (Get-FileHash -LiteralPath $spirv).Hash -ne $hash) {
            throw 'Invalid shader did not preserve the running fragment'
        }
        $capture.args.path = 'sdk-stripe-rejected.png'
        Assert-StripePixel (Invoke-Request $capture).result.path $true
    } finally {
        Copy-Item (Join-Path $PSScriptRoot 'shaders/sdk_stripe.frag.slang') $fragment -Force
    }

    $null = Invoke-Request @{command = 'unloadModule'; args = @{name = 'RenderConsumer'}}
    if ((Invoke-Request $filtered).result.matched -ne 0) { throw 'Unload left a render pass' }
    $capture.args.path = 'sdk-stripe-unloaded.png'
    Assert-StripePixel (Invoke-Request $capture).result.path $false
    "PASS: HDR temporal and exposure witness, coplanar depth-bias witness, rejected shader rollback, and owner cleanup. Capture: $active"
} finally {
    & $cli request --session $session 'shutdown' | Out-Null
}
