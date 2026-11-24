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
as errors. Each build's native shared-object entry passed 264 checks under a
Python launcher that blocks network, process creation and execution, hides
credentials/home and mounts source read-only. Offline CMake separately built
the CLI and test executable. The shared CLI implementation is tested in-process;
those standalone executables have not been run.
Windows/macOS builds and the parent CMake configuration remain unverified.

## Boundaries

- `pe_image.*`: checked little-endian PE decoding, section layout, relocations,
  names/ordinals, explicit unresolved-import failure and entry dispatch.
- `host.*`: RAII memory/protection/cache operations and dynamic native paths.
  Windows uses VirtualAlloc/VirtualProtect; POSIX uses mmap/mprotect. Guest ABI
  code does not call those APIs directly. No page is writable and executable.
- `main.cpp` and `native_command.*`: inspect/bootstrap CLI, with wide-character Windows paths.
  There is no native host-DLL fallback or success-return stub for missing imports.
- `guest_thread.*` and `host_tls.cpp`: aligned, independent TLS storage and a
  scoped Linux GS adapter. Native C++ TLS remains on FS. Other host thread
  adapters fail explicitly until implemented.
- `module_execution.cpp`: explicit dynamic module lifetime, ordered TLS/DLL
  notifications and mapping ownership through detach.
- `import_registry.*`, `memory_services.*` and `service_runtime.*`: explicit
  service names, bounded memory semantics and scoped Microsoft-ABI adapters.
- `unwind_table.*`: owned, checked x64 runtime-function metadata and range lookup.
  This component does not register tables, unwind frames or dispatch exceptions.
- `pe_exports.*` and `module_graph.*`: shared export/forwarder resolution,
  explicit in-memory module registration, dependency ordering and graph rollback.
  [Reuse provenance](REUSE.md) records the inherited implementation and licensed
  peer fixture cases.

Explicit package/work paths are supported. Otherwise the image's parent is the
package root and the work root follows host cache conventions, falling back to
the system temporary directory. Resolution canonicalizes existing inputs,
preserves host Unicode paths and rejects a work root overlapping the package or
executable. Resolution creates nothing and does not change the current directory.
Guest Windows path syntax/case rules are a later service-layer task.

## TLS and module lifetime

Mapping never runs callbacks. `Image::start()` creates an execution session,
copies/zero-fills the TLS template and delivers TLS callbacks before DLL process
attach. Failed DLL attach performs detach cleanup. Explicit close or destruction
delivers detach before releasing the mapping. The session retains its mapping if
the original image handle is moved or destroyed. Only one session per image is
allowed, and it must remain on its creating host thread.

Executable sessions expose the no-argument/uint64 bootstrap entry. DLL entries
use their separate Microsoft x64 lifecycle ABI and dynamic-load reserved argument.
TLS persists across calls within a session and starts fresh in a new session.
The guest GS base is restored after every call, including nested host scopes.

## Shared module graphs

`ModuleGraph` accepts explicitly provided module bytes and an optional shim
resolver. It does not search the host for replacement DLLs. A synthetic caller
can use the API as follows, with buffers it already owns:

```cpp
ModuleGraph modules(shim_resolver);
modules.add("app.exe", executable_bytes);
modules.add("provider.dll", provider_bytes);
modules.load("app.exe");
auto execution = modules.start();
auto result = execution.run_bootstrap();
```

Exports resolve by exact name or ordinal; module basenames use ASCII case folding.
Forwarders can target names, ordinals or other forwarders. Loops fail explicitly.
Dependencies initialize first and detach in reverse order. Every module receives
a stable TLS slot, with the root at zero, in one shared thread context. All TLS
blocks exist before the first callback. A failed attach cleans up initialized
dependencies, and a new graph session starts with fresh TLS values.

Current limits: 64 registered modules, 64 MiB of input bytes, 128 MiB of mappings,
64 snapshotted callbacks per module and a 1 MiB TLS block. The guest anchor exposes
a TLS vector at GS:[0x58] and last error at GS:[0x68], not a complete Windows TEB. Cyclic imports,
recursive package discovery, dynamic module insertion, worker threads, thread
notifications and static process startup are not implemented. The strict tests
create no threads and do not validate concurrent behavior.

## Package-backed CLI

The CLI now feeds the module graph through `package_modules.*`. It indexes only
the explicitly selected package directory, matches DLL basenames with ASCII case
folding and reads their bytes on demand. Unreferenced DLL contents and asset
archives are not read. Explicit shim bindings take precedence over package
discovery; there is no host system-DLL fallback.

```text
ambrosia-host --inspect IMAGE --package PACKAGE --work WORK
ambrosia-host --run-bootstrap IMAGE --package PACKAGE --work WORK
```

These are usage forms for original synthetic PE inputs. If omitted, PACKAGE is
the image's directory and WORK follows the existing dynamic cache-root policy.
The executable may reside outside the package. Neither mode creates a work
directory or writes package files. Results contain numeric counts, not host paths.

`--inspect` maps and binds dependencies without guest callbacks or entry execution.
`--run-bootstrap` performs the tested synthetic lifecycle and entry call. A DLL
root is rejected before initialization in this mode. Invalid arguments return 2;
input/loading failures return 1; success/help returns 0. Failures produce no partial
success JSON. This is not a supported retail game-launch command.

Provider limits are 4,096 directory entries, 64 DLL entries and the graph's 64 MiB
aggregate source budget. Empty/oversized reads, invalid basenames, ambiguous DLL
case variants and DLL symlinks fail explicitly. The portable reader assumes a
stable directory: these checks are not race-proof filesystem containment. Keep
the reviewed OS sandbox, read-only package access and hidden credentials/home.

## Initial native services

The CLI's explicit registry binds VirtualAlloc, VirtualFree, GetLastError and
SetLastError through kernel32/kernelbase and their registered API-set aliases.
Missing symbols and ordinals remain unresolved; there is no system DLL fallback.
`services::Scope` must enclose graph initialization, entry and detach. Each
runtime owns its reservations and destroys them on teardown. Runtime/memory
objects and their scopes remain confined to one host thread; concurrency is untested.

Supported allocations reserve at a 64 KiB-aligned base and round sizes/ranges to
the host page size. Commit preserves existing data; decommit retains addresses
and discards contents so recommit observes zeros. Release requires the exact
reservation base and size zero. The default budget is 64 MiB across at most
1,024 reservations. Only no-access, read-only, read/write and execute/read
protections are accepted. No page is writable and executable.

Fixed-address reservations, protection changes on committed pages, other flags
and VirtualProtect are unsupported. These return explicit failure or remain
unresolved rather than silently stripping console/GPU flags. Native error
adapters set failure codes and preserve the previous last error on success.
Active guest contexts keep that cell independent from the native-thread fallback;
nested scopes restore the previous context and GS base. Native TLS stays on FS.
The ABI bridge keeps native TLS resolution in a non-inlined host call because a
GCC shared-library build otherwise lost a live guest argument in synthetic tests.

Inspection allows a 128 MiB file and 256 MiB image, with graph budgets of 256 MiB
source bytes and 384 MiB mappings. The CLI releases its redundant input buffer
before mapping. API defaults and bootstrap execution retain the original 64 MiB
per-file/image/source and 128 MiB graph-mapping limits. Unsupported directories
report their index and required service. These budgets do not imply a supported
retail startup contract.

The actual supplied executable and seven requested bundled DLLs now complete
inspection through the native engine under strict isolation. Inspection retains
checked unwind tables and bounded resource, security, debug and load-configuration
metadata, reporting their unsupported-service bits. Resource/certificate contents
remain opaque; signatures are not authenticated. Security-directory extents use
file offsets. Code and data pages in inspected images are read-only and NX.
All image/graph execution entry points reject inspection mode, even when imports
resolve. Missing inspected IAT cells stay null, never success-return stubs.

The measured graph contains 1,186 static import entries, 861 unresolved entries
and 304,274 unwind records. These are entry counts including DLL dependencies,
not unique APIs or a boot percentage. Inspection success does not mean the game
can execute. JSON includes `inspection_only`, `unresolved_imports`,
`unwind_records` and `unsupported_directories` (a bit mask of directory indices).
Raw private diagnostics and game data remain outside Git.

The separate unwind-table component validates order, non-overlap, image bounds
and direct header references. Tiny fixtures cover empty records and exclusive
function ends. A read-only probe validates all 299,257 records in the supplied
executable's table and samples three lookups under both compiler builds. This
does not validate the unwind bytecode or implement RtlVirtualUnwind. Indirect
records, partial entries and invalid references fail explicitly. The loader's
exception execution restriction remains in place until the required machinery
exists; this parser is not a substitute for it.

The bootstrap still rejects exception registration,
delayed imports and other unsupported directories. Its parser is not a security
boundary or support claim for arbitrary PE files. Guest exceptions/unwinding,
guest file/storage services, audio and graphics remain future work. Existing
Vulkan code should be reused behind a platform surface/input adapter.

The next game milestone is a correctly presented interactive menu. Correct world
rendering and stable 30 fps with minimal stutter follow. Performance unlocking
comes later. Local development continues in small commits; GitHub publication is
on hold until explicitly requested.
