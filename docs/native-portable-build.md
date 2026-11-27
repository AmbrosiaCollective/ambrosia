# Isolated native builds with the shared command IR

`scripts/build_native_portable_isolated.py` extends the compiler-only profile with
one existing source file: `runtime/src/command_list.h`. The helper derives that
sibling path from `--source runtime/native`. It exposes the file read-only inside
an immutable `/portable` mount, with no writable scratch alias. No whole source
parent, game input, GPU device, home directory, credential or network is added.
The original compiler and runtime profiles stay unchanged.

The fixed guard runs before each compiler command. Its 25 checks cover private
PID namespace, exact namespace-local environment, hidden host paths/devices,
blocked sockets, read-only source/header/directory, file identity, scratch access
and resource bounds. A missing-header omission fails before compilation. Compiler
children remain allowed; this helper must never launch a guest or test payload.
It never executes its compiled output.

From the repository root, choose fresh output directories:

```sh
python3 -I scripts/build_native_portable_isolated.py \
  --source runtime/native --output reports/compiler-self-check --self-check
python3 -I scripts/build_native_portable_isolated.py \
  --source runtime/native --output reports/compiler-ir-probe --probe-portable
python3 -I scripts/build_native_portable_isolated.py \
  --source runtime/native --output reports/compiler-syntax \
  --syntax-unit graphics_devices.cpp --syntax-unit tests.cpp
python3 -I scripts/build_native_portable_isolated.py \
  --source runtime/native --output reports/compiler-build --shared-only
```

The fixed portable probe is syntax-only. C++ changes still need the changed units
and `tests.cpp` checked before full builds. `--cmake` builds the standalone offline
CMake targets; its include path is explicitly `/portable` in the namespace.
Ordinary CMake builds default `AMBROSIA_PORTABLE_INCLUDE_DIR` to the sibling `src`
directory, so source paths remain independent of a developer's checkout location.

Bounds remain 180 seconds per command, 2 GiB address space, 45/50 CPU seconds,
128 descriptors and zero core dumps. Normal scratch files are capped at 64 MiB;
CMake alone uses the existing 256 MiB archive budget. Returned regular single-link
ELF64 files remain capped at 64 MiB and are opened without following symlinks.
These process limits are not an aggregate memory cap across compiler children.

`build.json` records command results, native-file hashes, the canonical header
hash and guard hash. Before/after input checks must match before any artifact is
returned. Before loading an artifact through a reviewed guest/test launcher,
verify all current native/header/guard and artifact hashes as well as successful
terminal commands. Sources must stay frozen while any build is live. A compiler
error remains a failure even in syntax-only modes with no expected artifacts.
