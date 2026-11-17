#!/bin/sh
# Install the SPITE SDK and its global `spite` command (Linux x86-64). Non-interactive, no root.
#
#   Download this asset through the authenticated GitHub API as shown below.
#
# Environment (all optional):
#   SPITE_VERSION        install this version (for example 0.2.0) instead of the latest release
#   SPITE_HOME           install location (default ${XDG_DATA_HOME:-$HOME/.local/share}/spite)
#   SPITE_GITHUB_TOKEN   GitHub token for a private release repository (GITHUB_TOKEN is read as well)
#   SPITE_RELEASE_REPO   release repository (default spite-engine-global/spite-engine)
#   SPITE_RELEASE_API    GitHub REST API base URL (default https://api.github.com)
#   SPITE_NO_PATH=1      leave PATH and the shell profiles alone
#
# While the repository is private the one-liner above does not resolve; fetch this script through the
# API with the token (asset ids are listed by GET /repos/spite-engine-global/spite-engine/releases/latest):
#   curl -fsSL -H "Authorization: Bearer $SPITE_GITHUB_TOKEN" -H "Accept: application/octet-stream" \
#     https://api.github.com/repos/spite-engine-global/spite-engine/releases/assets/<install.sh asset id> | sh

die() {
    printf 'install.sh: %s\n' "$*" >&2
    exit 1
}

note() {
    printf '%s\n' "$*"
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | awk '{print $NF}'
    else
        die 'sha256sum, shasum or openssl is required to verify the download'
    fi
}

# fetch URL OUTPUT ACCEPT: HTTP status on stdout; the body goes to OUTPUT
fetch() {
    if [ -n "$auth" ]; then
        curl -sSL -H "Accept: $3" -H "@$auth" -o "$2" -w '%{http_code}' "$1"
    else
        curl -sSL -H "Accept: $3" -o "$2" -w '%{http_code}' "$1"
    fi
}

# get_ok URL OUTPUT ACCEPT WHAT: download, or stop with an explanation of the HTTP status
get_ok() {
    code=$(fetch "$1" "$2" "$3") || die "cannot reach $1"
    case $code in
        200) ;;
        401) die "$4: GitHub rejected the token (HTTP 401); check SPITE_GITHUB_TOKEN" ;;
        403|429) die "$4: GitHub refused the request (HTTP $code): rate limit or a token without access; set SPITE_GITHUB_TOKEN" ;;
        404)
            if [ -n "$auth" ]; then
                die "$4: not found (HTTP 404); check SPITE_VERSION, SPITE_RELEASE_REPO and that the token can read the repository"
            fi
            die "$4: not found (HTTP 404); a private repository needs a token: set SPITE_GITHUB_TOKEN"
            ;;
        *) die "$4: HTTP $code from $1" ;;
    esac
}

# stdin: release JSON; prints "<asset name> <asset api url>" per asset
asset_table() {
    tr ',' '\n' | awk '
        match($0, /"url" *: *"[^"]*\/releases\/assets\/[0-9]+"/) {
            u = substr($0, RSTART, RLENGTH); sub(/^"url" *: *"/, "", u); sub(/"$/, "", u); url = u; next
        }
        url != "" && match($0, /"name" *: *"[^"]*"/) {
            n = substr($0, RSTART, RLENGTH); sub(/^"name" *: *"/, "", n); sub(/"$/, "", n); print n " " url; url = ""
        }'
}

main() {
    repo=${SPITE_RELEASE_REPO:-spite-engine-global/spite-engine}
    api=${SPITE_RELEASE_API:-https://api.github.com}
    token=${SPITE_GITHUB_TOKEN:-${GITHUB_TOKEN:-}}
    auth=

    [ "$(uname -s)" = Linux ] || die "unsupported system $(uname -s); this installer supports Linux x86-64 (Windows: install.ps1)"
    case $(uname -m) in
        x86_64|amd64) target=linux-x86_64 ;;
        *) die "unsupported CPU $(uname -m); SPITE ships for x86-64 only" ;;
    esac
    for tool in curl tar awk sed tr; do
        command -v "$tool" >/dev/null 2>&1 || die "$tool is required"
    done

    if [ -z "${SPITE_HOME:-}" ]; then
        SPITE_HOME=${XDG_DATA_HOME:-${HOME:?HOME is not set}/.local/share}/spite
    fi
    case $SPITE_HOME in
        /*) ;;
        *) SPITE_HOME=$PWD/$SPITE_HOME ;;
    esac
    export SPITE_HOME
    if [ -n "${SPITE_NO_PATH:-}" ]; then
        export SPITE_NO_PATH
    fi

    work=$(mktemp -d "${TMPDIR:-/tmp}/spite-install.XXXXXX") || die 'cannot create a temporary directory'
    trap 'rm -rf "$work"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    if [ -n "$token" ]; then
        auth=$work/auth.hdr
        (umask 077 && printf 'Authorization: Bearer %s\n' "$token" >"$auth")
    fi

    if [ -n "${SPITE_VERSION:-}" ]; then
        endpoint=tags/v${SPITE_VERSION#v}
    else
        endpoint=latest
    fi
    note "Looking up the SPITE release ($repo, $endpoint)"
    get_ok "$api/repos/$repo/releases/$endpoint" "$work/release.json" application/vnd.github+json 'release lookup'
    tag=$(sed -n 's/.*"tag_name" *: *"\([^"]*\)".*/\1/p' "$work/release.json" | head -n 1)
    [ -n "$tag" ] || die 'the release has no tag name'
    version=${tag#v}
    assets=$(asset_table <"$work/release.json")

    archive_line=$(printf '%s\n' "$assets" | awk -v suffix="-$target.tar.gz" '
        substr($1, 1, 10) == "SPITE-SDK-" && length($1) > length(suffix) && substr($1, length($1) - length(suffix) + 1) == suffix { print; exit }')
    [ -n "$archive_line" ] || die "release $tag has no SPITE-SDK archive for $target"
    archive_name=${archive_line%% *}
    archive_url=${archive_line#* }
    sums_url=$(printf '%s\n' "$assets" | awk '$1 == "SHA256SUMS" { print $2; exit }')
    [ -n "$sums_url" ] || die "release $tag has no SHA256SUMS to verify $archive_name against"

    get_ok "$sums_url" "$work/SHA256SUMS" application/octet-stream SHA256SUMS
    expected=$(awk -v n="$archive_name" '$2 == n || $2 == "*" n { print $1; exit }' "$work/SHA256SUMS")
    [ -n "$expected" ] || die "SHA256SUMS does not list $archive_name"

    note "Downloading $archive_name"
    get_ok "$archive_url" "$work/$archive_name" application/octet-stream "download of $archive_name"
    actual=$(sha256_of "$work/$archive_name")
    [ "$actual" = "$expected" ] || die "SHA-256 mismatch for $archive_name: expected $expected, got $actual"

    note 'Extracting'
    mkdir "$work/extract"
    tar -xzf "$work/$archive_name" -C "$work/extract" || die "cannot extract $archive_name"
    root=
    if [ -f "$work/extract/sdk-manifest.json" ]; then
        root=$work/extract
    else
        for candidate in "$work"/extract/*/; do
            if [ -f "${candidate}sdk-manifest.json" ]; then
                root=${candidate%/}
                break
            fi
        done
    fi
    [ -n "$root" ] || die "$archive_name does not contain an SDK (no sdk-manifest.json)"
    [ -f "$root/bin/spite" ] || die "$archive_name has no bin/spite"
    chmod +x "$root/bin/spite"

    "$root/bin/spite" self install --from "$root" || die 'spite self install failed'
    spite=$SPITE_HOME/bin/spite
    [ -x "$spite" ] || die "spite self install did not create $spite"

    note ''
    note "SPITE $version is installed."
    note "  spite: $spite"
    if [ -n "${SPITE_NO_PATH:-}" ]; then
        note '  PATH was left unchanged (SPITE_NO_PATH); call spite by the path above.'
    else
        note "  If spite is not on PATH, add $SPITE_HOME/bin to your shell PATH."
        note "  For fish: fish_add_path '$SPITE_HOME/bin'"
    fi
    note ''
    report=$("$spite" doctor --json 2>/dev/null) || true
    compact=$(printf '%s' "$report" | tr -d ' \t\r\n')
    case $compact in
        *'"ok":true,"profile"'*) note 'Ready. Try: spite new games/MyGame' ;;
        *'"check":"compiler-abi","ok":false'*) note "spite doctor reported a problem: compiler ABI probe failed; use the exact compiler and libstdc++ toolset stamp shown by 'spite doctor'." ;;
        *'"needsAdmin":false'*) note "One step left: run 'spite setup' (downloads build tools; no administrator approval needed)" ;;
        *'"command":"spitesetup"'*) note "One step left: run 'spite setup' (asks for administrator approval once)" ;;
        *)
            problems=$(printf '%s\n' "$report" | sed -n 's/.*"error": *"\([^"]*\)".*/\1/p')
            note "spite doctor reported a problem: ${problems:-${report:-no output}}"
            ;;
    esac
}

main "$@"
