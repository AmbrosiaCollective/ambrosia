#!/bin/sh
# Linux entry point; all behavior lives in the shared Python command.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! command -v python3 >/dev/null 2>&1; then
    printf '%s\n' 'Ambrosia needs Python 3.12 or newer on PATH.' >&2
    exit 1
fi
if [ "$#" -eq 0 ]; then
    set -- menu
fi
exec python3 -I "$script_dir/ambrosia.py" "$@"
