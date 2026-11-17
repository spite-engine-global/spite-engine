#!/bin/sh
set -eu
sdk_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$sdk_root/internal/Bootstrap.sh"
spite_resolve_python
exec "$SPITE_BOOTSTRAP_PYTHON" -B "$sdk_root/internal/Sdk.py" "$@"
