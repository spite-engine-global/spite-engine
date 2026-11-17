# Native prerequisites and explicit provisioning

This is the provisioning reference. Day to day, `spite doctor` (or `spite doctor --json`)
checks the machine; `spite setup` fixes it: Python, CMake and Ninja are pinned downloads
(Toolchain.json) in the shared tools cache, then the compiler by the cheapest path with one
administrator approval (Visual Studio with an MSVC 14.51+ toolset, `minimumToolset` in
CompilerSetup.json, needs nothing). The `Setup.ps1`/`Setup.sh` scripts below are the older
standalone provisioning entry points and use the same pins. Microsoft Build Tools are
licensed under the Visual Studio terms and downloaded from Microsoft; the SDK does not
redistribute them.

Setup checks by default and never installs or downloads implicitly. Use the editor
profile to check Python and the package without CMake or a native compiler.
`doctor --profile build` also resolves CMake, builds a small module with the selected compiler
and loads it in the SDK host (the `compiler-abi` and `module-load` checks; the load needs a
Vulkan GPU and driver).
No setup command builds the engine or restores its vcpkg dependencies.

## Windows

`Setup.ps1 -Install` first checks the ABI. Compatible tools require no installation.
If missing, use an elevated Windows PowerShell to provision a dedicated Microsoft
Build Tools installation. `internal/CompilerSetup.json` pins the bootstrapper version,
Microsoft URL, SHA-256 and components. `internal/SetupCompiler.ps1` verifies the
hash, Microsoft Authenticode signature and version before installing. The required
MSVC toolset, Windows SDK and ABI are recorded in that JSON and sdk-manifest.json.

Use `-CacheDirectory <folder>` to reuse the verified bootstrapper and
`-CompilerInstallPath <new-folder>` to choose its dedicated destination. An existing
incompatible destination is preserved. Setup never silently upgrades another instance.
Code 740 requires elevation; 1602/5004 mean cancellation; 3010/1641 require an explicit
reboot followed by another setup run. A zero installer exit still requires doctor
to pass. Vendor logs are `%TEMP%/dd_bootstrapper*`, `dd_client*` and `dd_setup*`.

For offline machines, create a Microsoft layout on a connected host using the pinned
bootstrapper and every component in CompilerSetup.json (`--layout <folder> --add
<component>`). Keep `vs_BuildTools.exe`, Catalog.json, ChannelManifest.json and all
vendor payloads in that layout. Use `Setup.ps1 -Install -OfflineLayout <layout>`.
Installation uses local channel/catalog files with `--noWeb --noUpdateInstaller`;
incomplete or corrupt layouts fail explicitly. Setup never reboots the machine.

`spite setup` waits for a running Visual Studio installation it can identify (a vs_* installer process, or a `setup.exe`
under Visual Studio/Installer or started by a vs_* process). An elevated `setup.exe` with a hidden path and no such parent
cannot be linked to Visual Studio, so setup warns once and goes on instead of blocking on a guess.

## Linux

The baseline is Ubuntu 24.04. `Setup.sh --install` uses configured apt repositories
for missing GCC 14 and Ninja; root/sudo is needed for that step.
Python and CMake downloads are portable and do not need root.
`--offline` adds apt's `--no-download` cache mode and fails on unavailable packages.
Failed installation does not proceed to doctor or report success.

Builds use Ninja and g++-14. Existing caches using another generator require a new
build directory; SDK commands never delete them. Match the exact GCC/libstdc++ ABI
in the native Linux package. Doctor reports the expected stamp and probe error. If the
compiler is present but the probe fails, inspect that error and select a matching toolset;
repeating setup cannot resolve an ABI mismatch. Other distributions require manual setup;
generic package commands do not establish compatibility. Windows binaries cannot establish Linux compatibility.

Graphics drivers, display servers and system runtime libraries remain platform
prerequisites. Python and CMake are resolved outside the SDK. Windows PowerShell 7 and Node are needed only by the optional visual suite.

## Portable tool resolution

The supported ranges are Python 3.11.8 through 3.14.x (64-bit) and CMake 4.2.2
through 4.2.x. `internal/Toolchain.json` pins the downloadable versions, URLs and
SHA-256 hashes. Windows Python comes from python.org, CMake from Kitware releases,
and native Linux Python from Astral's python-build-standalone release. The archives
retain their upstream licenses. No package is fetched during ordinary SDK commands.

Selection order is an explicit path, a pinned shared cache entry, then a compatible
global installation. Cached selections preserve explicit choices across sessions.
Each resolution checks the version and records the executable hash. A changed
selected binary requires rerunning setup before using it. `spite tools` prints the
selection. New projects record Python; successful native builds record both tools
under `.spite/sdk.json`. Compiler ABI validation remains separate and strict.

Windows accepts `-Python <exe>`, `-CMake <exe>`, `-ToolsCache <directory>` and
`-ManagedTools`. Linux accepts `--python`, `--cmake`, `--tools-cache` and
`--managed-tools`. The environment equivalents are `SPITE_PYTHON`, `SPITE_CMAKE`,
`SPITE_TOOLS_CACHE` and `SPITE_MANAGED_TOOLS=1`. Managed mode skips automatic global
fallback; explicit paths still take priority. For CI, keep that environment setting
on every SDK invocation. Normal setup prefers reuse and needs no network when tools
are already compatible.

Defaults are `%LOCALAPPDATA%/SpiteSdk/tools` on Windows and
`${XDG_CACHE_HOME:-~/.cache}/spite/tools` on Linux. An explicit ToolsCache choice is
recorded per SDK installation for future sessions; SPITE_TOOLS_CACHE overrides it.
Downloads are extracted
through private staging and published without replacing an existing cache. Failed
or corrupt caches require inspection; setup does not erase them automatically.

For offline provisioning, put the archives named in Toolchain.json in a directory,
then use `-Install -OfflineTools <directory>` or `--install --offline-tools <directory>`.
The same pinned checksums apply. Compiler payloads are separate: Windows uses
`-OfflineLayout`; Linux `--offline` means apt `--no-download` for compiler packages
and requires `--offline-tools` or already installed tools to avoid tool downloads.

`-AddToPath` changes only the Windows user's PATH. `--add-to-path` appends a quoted
SDK path entry to ~/.profile. Neither option modifies machine-wide PATH; no PATH
changes happen by default. Remove obsolete entries when retiring an SDK installation.
# CMake compatibility

CMake 4.2.2 is the pinned version; CMake 4.3.1-msvc1 was also qualified by building the external RenderConsumer with MSVC 14.51. If CMake is rejected, clear `SPITE_CMAKE` and run `Setup.ps1 -Install -Profile build` to install the pinned version.

SDK CLI activity reporting is enabled by default; see [Anonymous SDK activity](../README.md#anonymous-sdk-activity) for its scope and opt-out.
