#!/bin/sh
set -eu
sdk_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
spite_install_tools=0
profile=build
add_path=0
apt_offline=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --install) spite_install_tools=1; shift ;;
        --profile) profile=${2:?missing profile}; shift 2 ;;
        --managed-tools) SPITE_MANAGED_TOOLS=1; export SPITE_MANAGED_TOOLS; shift ;;
        --python) SPITE_PYTHON=${2:?missing Python path}; export SPITE_PYTHON; shift 2 ;;
        --cmake) SPITE_CMAKE=${2:?missing CMake path}; export SPITE_CMAKE; shift 2 ;;
        --tools-cache) SPITE_TOOLS_CACHE=${2:?missing cache}; export SPITE_TOOLS_CACHE; shift 2 ;;
        --offline-tools) SPITE_OFFLINE_TOOLS=${2:?missing offline directory}; export SPITE_OFFLINE_TOOLS; shift 2 ;;
        --offline) apt_offline=1; shift ;;
        --add-to-path) add_path=1; shift ;;
        *) echo 'Usage: Setup.sh [--install] [--profile editor|build] [--managed-tools] [--python PATH] [--cmake PATH] [--tools-cache DIR] [--offline-tools DIR] [--offline] [--add-to-path]' >&2; exit 2 ;;
    esac
done
case "$profile" in editor|build) ;; *) echo 'Unknown setup profile' >&2; exit 2 ;; esac
SPITE_REFRESH_TOOLS=1; export SPITE_REFRESH_TOOLS
. "$sdk_root/internal/Bootstrap.sh"
spite_resolve_python
set -- --profile "$profile"
if [ "$spite_install_tools" = 1 ]; then set -- "$@" --install; fi
if [ -n "${SPITE_OFFLINE_TOOLS:-}" ]; then set -- "$@" --offline-tools "$SPITE_OFFLINE_TOOLS"; fi
"$SPITE_BOOTSTRAP_PYTHON" -B "$sdk_root/internal/Toolchain.py" "$@"
if [ "$spite_install_tools" = 1 ] && [ "$profile" = build ] &&
   { ! command -v g++-14 >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; }; then
    command -v apt-get >/dev/null 2>&1 || { echo 'Install GCC14 and Ninja with your system package manager.' >&2; exit 1; }
    set -- install -y --no-install-recommends g++-14 ninja-build
    if [ "$apt_offline" = 1 ]; then set -- "$@" --no-download; fi
    if [ "$(id -u)" = 0 ]; then apt-get "$@"; else sudo apt-get "$@"; fi
fi
sh "$sdk_root/sdk.sh" doctor --profile "$profile" --json
if [ "$add_path" = 1 ]; then
    "$SPITE_BOOTSTRAP_PYTHON" -I -c 'import pathlib,shlex,sys; root=pathlib.Path(sys.argv[1]); profile=pathlib.Path.home()/".profile"; line="export PATH="+shlex.quote(str(root/"bin")+":"+str(root))+":\"$PATH\""; text=profile.read_text() if profile.exists() else ""; profile.write_text(text+"\n# SPITE SDK\n"+line+"\n") if line not in text else None' "$sdk_root"
    echo 'SDK/bin and SDK root added to ~/.profile. Open a new login session.'
fi
