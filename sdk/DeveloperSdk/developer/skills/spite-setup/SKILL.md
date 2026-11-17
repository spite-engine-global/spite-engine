---
name: spite-setup
description: Use when a SPITE command fails on missing tools, or a project must switch to another SDK version.
---

# SPITE setup

`spite` is the global launcher on the user PATH. On Windows its default is `%LOCALAPPDATA%\Spite\bin\spite.exe`; on Linux use the install location reported by the installer. In an older shell, use that absolute path or the user-authorized PATH update. Project commands run from the project root (any subfolder works).

## Diagnose

- `spite doctor --profile build` checks everything C++ work needs. Use `--profile editor` for scene-only work. `--json` gives the verdict as data, including remediation appropriate to the failure.
- Without `--fast`, doctor ends by building a small module and loading it in the SDK host, which needs a Vulkan GPU. `--fast` skips that probe (detection only).
- `spite tools --profile build` prints the selected Python, CMake and compiler paths with versions and hashes.
- `spite version` shows the `spite` and the SDK this directory uses and where the choice came from. `spite list` shows installed SDKs.
- The editor and game need a GPU with Vulkan 1.3, even in headless mode. GPU drivers are not part of the SDK; `spite setup` cannot fix a failing `module-load` check.

## Fix

Never install tools or change PATH on your own. Report what `doctor` says is missing. Run `spite setup` only when the user asks.

- `spite setup` installs Python, CMake and Ninja (no admin) and, when no Visual Studio with an MSVC 14.51+ toolset exists, the C++ compiler (one administrator prompt, about 1-3 GB, 5-20 minutes). Exit code 3010 means restart Windows first.
- Windows: tell the user a UAC prompt will appear and do not start it while nobody can watch the screen. Without an interactive desktop, setup prints the command for an administrator terminal: ask the user to run it, then run `spite doctor`.
- Run it with a command timeout of at least 30 minutes, or in the background and poll `spite doctor --fast`. If it times out, `spite setup` again waits for the running installer (`--no-wait` only reports it). Never start a second Visual Studio installer by hand.
- Linux: when setup prints a `sudo ...` command, ask the user to run it. Do not work around it.
- Linux's documented baseline is Ubuntu 24.04 with the matching GNU C++/libstdc++ ABI. A GCC major version alone does not prove a match. Automatic setup does not support NixOS; inspect the actual doctor failure instead of repeatedly suggesting setup. Runtime loader visibility and the compiler environment are separate checks.
- Offline: `spite setup --offline-tools <dir of tool archives>` (or `SPITE_OFFLINE_TOOLS`) and `--layout <Visual Studio layout>`. `spite --offline <command>` or `SPITE_OFFLINE=1` stops `spite` from installing SDKs.
- Point at existing tools with `SPITE_PYTHON` and `SPITE_CMAKE`. Choose a cache with `SPITE_TOOLS_CACHE`. `SPITE_MANAGED_TOOLS=1` makes later commands use only the pinned tools.
- Do not run the SDK's `Setup.ps1`, `Setup.sh`, `sdk.cmd` or `sdk.sh` by hand: they are internals of `spite`.
- Failure codes and explicit paths are in `<sdk>/guides/Setup.md`. `<sdk>` is `SdkRoot` from `spite project`.

## Switch the project to another SDK

Do this only after the user picks the version.

1. `spite update` installs the newest release and makes it the default; `spite install <version>` installs a specific one. Both download from GitHub.
2. `spite upgrade` re-pins the project to the version `spite` selects (the newest installed) and rebuilds native projects; scene-only projects need no compiler. `spite upgrade --to <version>` picks one. The old pin is saved as `.spite/sdk.json.bak`.
3. Check that `.spite/sdk.json` shows the new `version`, `apiStamp` and `toolsetStamp`. `spite` never upgrades a project by itself, and it prints a note when a newer SDK is installed.
4. Binaries built against the old SDK do not match its ABI: every native module must be rebuilt (`upgrade` does this; `spite build` after any other change).
5. Upgrade refreshes unchanged SDK-owned `AGENTS.md`, `CLAUDE.md` and both skill trees using `.spite/sdk-managed-files.json` hashes. It preserves and reports edited SDK-owned files and leaves unrelated user additions alone. Review preserved paths and merge desired guidance manually; do not delete the marker to force overwrites.

## Check the SDK itself

`spite verify --output <new-directory-outside-the-SDK>` builds, bakes, loads, saves, ships and runs a sample game. It needs a GPU and a compiler, and it takes time. Run it only when asked.
