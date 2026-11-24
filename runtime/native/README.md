# Native CPU bootstrap

This is the first part of a native Linux runtime, not a game launcher. The
portable C++20 core maps tiny original AMD64 PE fixtures, applies DIR64
relocations and binds imports through explicit adapters. Linux tests verify a
six-argument Microsoft x64 ABI call, including stack arguments, returning 21.
No Wine, Proton, Windows SDK, external packages or game data are involved.

## Build

Use an existing C++20 compiler and CMake 3.24 or newer. These commands do not
download dependencies. Choose a build path outside the source/package trees:

```sh
cmake -S runtime/native -B ../ambrosia-native-build -DBUILD_TESTING=ON
cmake --build ../ambrosia-native-build --parallel 2
ctest --test-dir ../ambrosia-native-build --output-on-failure
```

The parent runtime configuration also accepts `-DAMBROSIA_NATIVE_HOST_ONLY=ON`
to avoid all inherited SDK/GPU targets. On non-x64 hosts the explicit bootstrap
configuration fails clearly; existing portable targets remain available.
These commands are not an OS sandbox. Follow your reviewed compiler/test
isolation policy rather than executing unchecked repository builds on the host.

The local Linux validation compiled with GCC 13 and Clang 18, warnings treated
as errors. Each build's native shared-object entry passed 41 checks under a
Python launcher that blocks network, process creation and execution, hides
credentials/home and mounts source read-only. Offline CMake separately built
the CLI and test executable. Those standalone executables have not been run.
Windows/macOS builds and the parent CMake configuration remain unverified.

## Boundaries

- `pe_image.*`: checked little-endian PE decoding, section layout, relocations,
  names/ordinals, explicit unresolved-import failure and entry dispatch.
- `host.*`: RAII memory/protection/cache operations and dynamic native paths.
  Windows uses VirtualAlloc/VirtualProtect; POSIX uses mmap/mprotect. Guest ABI
  code does not call those APIs directly. No page is writable and executable.
- `main.cpp`: narrow inspect/bootstrap CLI, with wide-character Windows paths.
  There is no native host-DLL fallback or success-return stub for missing imports.

Explicit package/work paths are supported. Otherwise the image's parent is the
package root and the work root follows host cache conventions, falling back to
the system temporary directory. Resolution canonicalizes existing inputs,
preserves host Unicode paths and rejects a work root overlapping the package or
executable. Resolution creates nothing and does not change the current directory.
Guest Windows path syntax/case rules are a later service-layer task.

The bootstrap deliberately caps images/files at 64 MiB and rejects DLLs, TLS,
exception registration, delayed imports and other unsupported directories.
It accepts only its no-argument/uint64 bootstrap entry contract. Its parser is
not a security boundary or support claim for arbitrary PE files. Full guest
TLS/thread state, bundled DLL lifecycle, exceptions/unwinding, storage/audio and
graphics remain future work. Existing Vulkan code should be reused behind a
platform surface/input adapter; no second renderer is introduced here.

The next game milestone is a correctly presented interactive menu. Correct world
rendering and stable 30 fps with minimal stutter follow. Performance unlocking
comes later. Local development continues in small commits; GitHub publication is
on hold until explicitly requested.
