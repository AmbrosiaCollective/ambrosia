#!/bin/sh
# macOS terminal entry point; accepts the same commands as Linux and Windows.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec sh "$script_dir/ambrosia.sh" "$@"
