#!/bin/sh
# Sourced by the SDK launcher and setup before Python is available.
spite_resolve_python() {
    pin_version=3.14.10
    selection_key=$(printf '%s' "$sdk_root" | sha256sum | cut -c1-24)
    default_cache="${XDG_CACHE_HOME:-$HOME/.cache}/spite/tools"
    pointer="$default_cache/selections/$selection_key.cache"
    if [ -n "${SPITE_TOOLS_CACHE:-}" ]; then
        mkdir -p "$SPITE_TOOLS_CACHE" "$default_cache/selections"
        cache=$(CDPATH= cd -- "$SPITE_TOOLS_CACHE" && pwd)
        printf '%s\n' "$cache" > "$pointer"
    elif [ -f "$pointer" ]; then cache=$(cat "$pointer")
    else cache=$default_cache; fi
    saved_python_file="$cache/selections/$selection_key.python"
    saved_explicit_file="$cache/selections/$selection_key.python-explicit"
    saved_python=$(cat "$saved_python_file" 2>/dev/null || true)
    if [ -z "${SPITE_PYTHON:-}" ] && [ "$(cat "$saved_explicit_file" 2>/dev/null || true)" = 1 ]; then
        SPITE_PYTHON=$saved_python
        export SPITE_PYTHON
    fi
    cached_python="$cache/python/$pin_version/bin/python3"
    probe='import sys,struct; assert (3,11,8)<=sys.version_info[:3]<(3,15) and struct.calcsize("P")==8'
    if [ -n "${SPITE_PYTHON:-}" ]; then
        "$SPITE_PYTHON" -I -c "$probe" >/dev/null 2>&1 || { echo 'Explicit SPITE_PYTHON is incompatible' >&2; return 1; }
        SPITE_BOOTSTRAP_PYTHON=$SPITE_PYTHON; export SPITE_BOOTSTRAP_PYTHON
        return 0
    fi
    if [ -x "$cached_python" ] && "$cached_python" -I -c "$probe" >/dev/null 2>&1; then
        SPITE_BOOTSTRAP_PYTHON=$cached_python
    elif [ "${SPITE_MANAGED_TOOLS:-0}" != 1 ] && [ -n "$saved_python" ] && [ -x "$saved_python" ] && "$saved_python" -I -c "$probe" >/dev/null 2>&1; then
        SPITE_BOOTSTRAP_PYTHON=$saved_python
    elif [ "${SPITE_MANAGED_TOOLS:-0}" != 1 ] && command -v python3 >/dev/null 2>&1 && python3 -I -c "$probe" >/dev/null 2>&1; then
        SPITE_BOOTSTRAP_PYTHON=$(command -v python3)
    elif [ "${spite_install_tools:-0}" = 1 ]; then
        [ "$(uname -m)" = x86_64 ] || { echo 'Python cache requires Linux x86-64' >&2; return 1; }
        [ ! -e "$cache/python/$pin_version" ] || { echo 'Cached Python is invalid; inspect it before replacement' >&2; return 1; }
        mkdir -p "$cache/python"
        stage=$(mktemp -d "$cache/python/.install-XXXXXXXX")
        (
            trap 'rm -rf -- "$stage"' EXIT HUP INT TERM
            filename='cpython-3.14.10+20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz'
            archive="$stage/$filename"
            if [ -n "${SPITE_OFFLINE_TOOLS:-}" ]; then cp "$SPITE_OFFLINE_TOOLS/$filename" "$archive"
            elif command -v curl >/dev/null 2>&1; then
                curl --fail --location --proto '=https' --tlsv1.2 --output "$archive" 'https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.14.10%2B20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz'
            elif command -v wget >/dev/null 2>&1; then
                wget -O "$archive" 'https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.14.10%2B20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz'
            else echo 'Python provisioning needs curl or wget, or --offline-tools' >&2; exit 1; fi
            printf '%s  %s\n' 'c2ff2e97422f5d3896a16e0317fd212ba977bbf2688d93a6f7a03d817506bd30' "$archive" | sha256sum --check >&2
            tar -xzf "$archive" -C "$stage"
            # mkdir owns the destination; never merge into another setup's installation.
            mkdir "$cache/python/$pin_version"
            mv "$stage/python" "$cache/python/$pin_version/runtime"
            ln -s runtime/bin "$cache/python/$pin_version/bin"
        ) || return 1
        SPITE_BOOTSTRAP_PYTHON=$cached_python
        "$SPITE_BOOTSTRAP_PYTHON" -I -c "$probe" >/dev/null 2>&1 || return 1
    else echo 'Compatible Python is missing. Run Setup.sh --install; no download performed.' >&2; return 1
    fi
    export SPITE_BOOTSTRAP_PYTHON
}
