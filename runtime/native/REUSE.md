# Native runtime source provenance

The inherited repository license and credits continue to apply. New native code
must retain applicable notices when adapting source. Pin origins and keep tests
and host-specific changes clear.

| Files | Origin and adaptation |
|---|---|
| `pe_exports.cpp`, `module_graph.cpp` | Adapted from the inherited `runtime/src/pe_loader.cpp` export parsing, forwarder resolution and dependency-ordering logic at Ambrosia commit `17176bd3a29b5851d573e768a0e365ac707489b9`. Project GPL-3.0-or-later terms continue. Windows host-DLL fallbacks and TLS-helper DLLs are excluded from the native adapters. |
| `forwarder_cases.h` | The seven-case `TARGETS` matrix from [Xbox2Windows](https://github.com/R0Y00/Xbox2Windows/blob/939e5142458007405bbc7a2a6d14bad292fcae62/tests/intake/forwarders_check.py), adapted from Python inventory assertions to C++ native module checks. MIT license reproduced in `LICENSE.Xbox2Windows`. |
| `memory_services.cpp`, `service_runtime.cpp` | Overflow-safe size rounding and memory API-set aliases adapted from inherited `runtime/shims/xmem.cpp` at `fca8458b3ca608d633071f82bd29f97983eb51b6`, under the project's existing license. Its Windows allocation delegation and console/GPU flag stripping are excluded. Native reservation ownership, backing-page discard and ABI/error integration are new. |
| `unwind_table.cpp` | Range/order/header validation adapted from inherited `runtime/src/pe_loader.cpp::register_unwind` at `f732bb5c455e8d55202011cdde99cc7a3a10ff08`, under the project's existing GPL terms. The native component owns decoded records and uses standard C++ lookup. Unlike Windows host registration, it rejects partial/indirect records explicitly and does not install an exception dispatcher. |

Wine's [x64 lookup behavior](https://github.com/wine-mirror/wine/blob/ff2631c6ed3d1721cd050d951671c307ade32b90/dlls/ntdll/unwind.c)
and [runtime-function layout](https://github.com/wine-mirror/wine/blob/ff2631c6ed3d1721cd050d951671c307ade32b90/include/winnt.h)
were consulted as pinned ABI references for the table work. No Wine source,
headers or runtime dependency is incorporated by that component.

Memory behavior is checked against Microsoft SDK API contracts at documentation
revision `656e9656bfaef34dd29cd7ee9e1146aec17e924c`:
[VirtualAlloc](https://github.com/MicrosoftDocs/sdk-api/blob/656e9656bfaef34dd29cd7ee9e1146aec17e924c/sdk-api-src/content/memoryapi/nf-memoryapi-virtualalloc.md),
[VirtualFree](https://github.com/MicrosoftDocs/sdk-api/blob/656e9656bfaef34dd29cd7ee9e1146aec17e924c/sdk-api-src/content/memoryapi/nf-memoryapi-virtualfree.md)
and [GetLastError](https://github.com/MicrosoftDocs/sdk-api/blob/656e9656bfaef34dd29cd7ee9e1146aec17e924c/sdk-api-src/content/errhandlingapi/nf-errhandlingapi-getlasterror.md).
This is an explicitly limited implementation, not complete Windows conformance.

No upstream Python script, pefile dependency, setup/build helper, binary or game
data is imported. Original native fixtures supply the PE bytes and execute the
cases in isolation. Reused cases keep their original technical meaning; native
execution coverage does not claim retail-game validation.

The unwind-program component consults the same pinned Wine x64 behavior reference
for opcode meaning, stack offsets and partial-prologue semantics. Its checked
span-based decoder, owned operations and bounded stack restoration are implemented
in the native core; no Wine source/header or runtime is imported. It remains a
component; checked chain restoration and selected epilogues now have original
synthetic coverage. Guest CONTEXT adapters and exception handling are unfinished.

Clock and identity adapters follow Microsoft API contracts at SDK documentation
revision `656e9656bfaef34dd29cd7ee9e1146aec17e924c`, including
[FILETIME](https://github.com/MicrosoftDocs/sdk-api/blob/656e9656bfaef34dd29cd7ee9e1146aec17e924c/sdk-api-src/content/minwinbase/ns-minwinbase-filetime.md)
and [QueryPerformanceCounter](https://github.com/MicrosoftDocs/sdk-api/blob/656e9656bfaef34dd29cd7ee9e1146aec17e924c/sdk-api-src/content/profileapi/nf-profileapi-queryperformancecounter.md).
No external implementation or header is imported by these adapters.

Critical-section behavior follows pinned Microsoft SDK documentation at the same
revision, including [InitializeCriticalSectionEx](https://github.com/MicrosoftDocs/sdk-api/blob/656e9656bfaef34dd29cd7ee9e1146aec17e924c/sdk-api-src/content/synchapi/nf-synchapi-initializecriticalsectionex.md)
and [EnterCriticalSection](https://github.com/MicrosoftDocs/sdk-api/blob/656e9656bfaef34dd29cd7ee9e1146aec17e924c/sdk-api-src/content/synchapi/nf-synchapi-entercriticalsection.md).
The existing pinned Wine winnt.h is an x64 field-layout reference. No Wine code,
header, native handle or debugger implementation is imported by this component.

FLS adapters follow the pinned Microsoft [FlsAlloc](https://github.com/MicrosoftDocs/sdk-api/blob/656e9656bfaef34dd29cd7ee9e1146aec17e924c/sdk-api-src/content/fibersapi/nf-fibersapi-flsalloc.md)
and [FlsFree](https://github.com/MicrosoftDocs/sdk-api/blob/656e9656bfaef34dd29cd7ee9e1146aec17e924c/sdk-api-src/content/fibersapi/nf-fibersapi-flsfree.md)
contracts. Pinned Wine kernelbase/ntdll thread sources at
`ff2631c6ed3d1721cd050d951671c307ade32b90` inform last-error and exit-cleanup
behavior only. The native owned registry/context hooks import no Wine code,
headers or runtime. These components do not provide fiber switching.

The TIB field layout follows the already pinned Wine winnt.h NT_TIB64 declaration.
Native Linux stack discovery calls installed pthread APIs; glibc sources at
`ef321e23c20eebc6d6fb4044425c00e6df27b05f` were read to verify bounds semantics.
No glibc/Wine implementation or header is imported into host_stack.cpp.

CRT callback table layout and initialization/cleanup ordering were checked against
Wine exit.c, data.c, msvcrt.h and public-domain CRT header declarations at
`ff2631c6ed3d1721cd050d951671c307ade32b90`. They are behavior/layout references;
no Wine implementation, header or runtime is imported by the owned C++ tables
and Microsoft ABI adapters. The initializer routines retain forward ordering,
null skipping and first-error propagation.

Sequenced-list head initialization follows the pinned Microsoft SDK
InitializeSListHead contract and the existing Wine x64 SLIST_HEADER layout.
Wine ntdll sync.c at the same pinned revision verifies the 16-byte format
selector. These are references only; no implementation/header is ported.

Module notification policy follows Microsoft DisableThreadLibraryCalls at the
same pinned SDK revision. Wine kernelbase/ntdll loader.c at the existing Wine
revision provides failure/status and per-module notification-state references.
The native graph owns its policy and scopes; no Wine implementation or header
is imported, and no Wine/Proton runtime is used.

UCRT's opaque pointer-sized FILE and standard-stream indices follow Wine's
public-domain corecrt_wstdio.h/stdio.h declarations at the existing pinned Wine
revision. Its file.c was read for accessor behavior. No header, implementation
or host FILE layout is imported by the owned stream identities.

CRT allocation and errno behavior were checked against Wine heap.c/errno.c at
`ff2631c6ed3d1721cd050d951671c307ade32b90`. They are behavior references only;
no implementation or header is ported. Owned allocation tracking uses host libc
behind checked size/ownership limits, and guest errno has separate storage.

Stream-buffer field access follows the already pinned Wine file.c
_get_stream_buffer_pointers declaration and address-return semantics. The native
fields/identity checks are independently owned; no implementation/header port.
