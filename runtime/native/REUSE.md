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

RtlPcToFileHeader's signature/output and containing-image semantics were checked
against the already pinned Wine ntdll loader.c. The native lookup searches only
its owned graph ranges; no Wine implementation or host loader is imported.

CRT startup types/enums/field behavior follow the already pinned Wine
public-domain corecrt_startup.h and data.c references. The constant-success Wine
argv placeholder is not ported: native configuration changes owned argv/argc
state using the actual dynamic image path. No source/header/runtime port.

CRT process cleanup registration/order was checked against the already pinned
Wine exit.c and public callback declarations. The native list uses correctly
typed void Microsoft ABI callbacks and retains its own bounded batches. No Wine
implementation, header or runtime is imported by this component.

Loaded module handle signatures/null-root/unchanged-reference semantics were
checked against the already pinned Wine kernelbase loader.c. Native lookup uses
only its own graph and registry; no implementation/header/runtime is imported.

Native-service PE facades use the existing checked export parser and Mapping
boundary. PE32+/export field layouts were checked against the already pinned
Wine winnt.h, and module/export query contracts against kernelbase loader.c.
The emitted images, canonical tail trampolines, catalog and budgets are original
runtime structures; no system DLL bytes, Wine implementation/header or external
runtime is imported. Facades are fixed in-memory views, not disk-loadable SDK DLLs.

Event creation, signaling, reset, wait and close contracts were checked against
Microsoft SDK API documentation at
`656e9656bfaef34dd29cd7ee9e1146aec17e924c`. Wine kernelbase sync.c at
`ff2631c6ed3d1721cd050d951671c307ade32b90` was read to check signatures and
creation last-error behavior. The owned signal model, handle directory and
standard C++ waiting adapter import no external implementation, header or runtime.


Registry open/extended-open/close contracts were checked against Microsoft SDK
API documentation at `656e9656bfaef34dd29cd7ee9e1146aec17e924c`, including
case-insensitive lookup and the distinct legacy/extended empty-subkey rules.
The private configured-key store, shared opaque ID allocator and Microsoft ABI
adapters import no external source, headers or system registry implementation.


The strict observed graphics-memory configuration validator is adapted from
Xbox2Windows src/d3d12_xs_shim.cpp at
`481a1c62c2c4306e5b0320ffe811ef2816f5809b` under MIT. Its source-owned range
notes inform the derived coverage policy. See LICENSE.Xbox2Windows for the
preserved notice. The portable owned policy and scoped creation gate replace
Windows process-global state; no D3D12 GPU implementation or SDK header is ported.
The 32-byte shape is a measured profile with incomplete SDK semantics.


The native XMem reservation boundary follows the inherited xmem.cpp and
xmem_exe_calls.md five-argument ABI, reserve-only page mode and raw policy words.
It reuses the existing owned Mapping/MemoryServices implementation. No Windows
placeholder API or physical-page implementation is ported. Attribute meaning
and GPU/cache mapping remain incomplete; raw values are retained, not guessed.


Separate private/window/aggregate budgets and sparse diagnostic selection reuse
the owned memory boundary. Pinned Linux v6.17 mmap/mprotect/mremap text at
`e5f0a698b34ed76002dc5cff3804a61c80233a7a` informed the external sandbox's
resource review only. No kernel source is ported into the runtime or executed.


Committed XMem logical-page alignment and the exact known policy combinations
follow the inherited xmem.cpp/xmem_exe_calls.md reference. CPU backing reuses
Mapping with ordinary host pages; the owned metadata does not port Windows
placeholder, privileged large-page, GPU-cache or physical-page implementations.


Semaphore count/access/name contracts follow Microsoft SDK API documentation at
`656e9656bfaef34dd29cd7ee9e1146aec17e924c`. Wine kernelbase sync.c and server
semaphore.c at `ff2631c6ed3d1721cd050d951671c307ade32b90` were read for
creation status, generic access mappings, validation order and release limits.
The portable count model, owned typed directory and C++ wait adapter import no
external implementation, header or runtime.

## Portable buffer formatting dependency

The native formatter uses [fmt 9.1](https://github.com/fmtlib/fmt/tree/9.1.0)
through its header-only printf API. Validation uses the installed libfmt-dev
9.1.0 package. No dependency source or build/install script is copied or run;
CMake requires an existing fmt package. The upstream license is retained in
LICENSE.fmt. Windows argument decoding, LLP64 widths, byte-oriented string
bounds/padding and the zero-integer-precision correction are local adapters.

The six-argument common formatter ABI and snprintf wrapper selection follow the
already pinned public-domain Wine stdio.h. The supported UCRT option families
also appear in pinned Wine msvcrt.h's UCRTBASE_PRINTF_MASK. This is reference
use only; no Wine formatter code/runtime is imported. Tests and actual option
observations do not certify all Windows CRT formatting or rounding behavior.

## Owned runtime interface prefix

runtime_interface.* and gdk_threading.* adapt the manual identity/runtime-pin and
reference-count pattern from inherited runtime/shims/xgameruntime.cpp at
2c9152e. Project license/credits remain. Windows long is replaced by explicit
uint32 counts and the native component owns each object per Runtime. No host C++
virtual ABI, global singleton leak or private-slot zero-success handlers are
copied. The public threading class identifier/slot positions follow that same
source; unsupported slots stop before returning any invented value.


## Native task queue contract reference

The existing MIT-licensed third_party/xtaskqueue snapshot of Microsoft
libHttpClient at c5007fd9cbdc785518ea6dccc5751b125d451cc1 supplies the public
mode values, callback/queue API declarations and creation/duplication/close
lifetime contracts. runtime/shims/xtaskqueue/pch.h maps invalid handles to E_HANDLE
and enables unique duplicate handles. Its Windows thread-pool/timer implementation
and Windows callbacks are not compiled or copied into the native runtime.
worker_pool.* and task_queues.* are portable C++ implementations of the bounded
scheduler and owned-handle subset. No claim is made to port the entire Microsoft
queue or XAsync implementation. Guest callback dispatch remains unsupported.


## Application lifecycle registry adaptation

app_lifecycle.* adapts OpenScarlett's MIT plm_notifications.h registry pattern at
022a8b25148cc08412df8ac7e752b9efed4cd819. LICENSE.OpenScarlett retains its notice
and permission. The separate-kind callback/context storage, registration handles
and snapshot delivery are retained conceptually, with Ambrosia's explicit guest
ABI, runtime ownership/shared ID allocator, unaligned output support and bounded
resources. Removal marks entries disabled, skips canceled snapshot entries and
waits for other-thread callbacks; delivery requires a reviewed guest execution
scope. Windows global registries, telemetry, watchers and system integration are
not imported. No external source is executed. Existing archived Victory/TomBox
code was inspected for ABI comparison only; none is copied into this component.


## Offline GDK networking interface

gdk_networking.* adapts inherited runtime/shims/xgameruntime.cpp's networking
class/v2 IDs, slot order, 16-byte hint layout and explicit offline query policy
at e05cbc8. Project license/credits remain. It uses the native runtime-owned
interface prefix and fixed-width/unaligned-safe buffers, excluding Windows globals,
XAsync callbacks and certificate handling. Unsupported slots remain nonreturning
rather than copying the inherited private E_NOTIMPL shortcuts. No host-interface
query, fabricated internet/UDP availability or certificate bypass is imported.


## Winsock lifecycle and public ordinal metadata

The WSADATA x64 layout is cross-checked with the pinned archived console-host
xbtp declaration and Xenora's408-byte compatibility layout. The local lifecycle
implementation includes no host socket, loopback transport, Windows system call
or external runtime. Installed Wine9.0 export metadata verifies public ordinals
111/112/115/116; only metadata is read, no DLL is loaded, executed or copied.
Reference hashes/package version are recorded locally. Native sparse PE export
emission extends the existing project builder and retains its original license.


## Secure wide formatting extension

The UTF-16 secure buffer adapter reuses this project's Windows slot decoder and
fmt9.1 numeric rendering. It introduces no host wchar_t/va_list reinterpretation
or external formatter implementation. The existing pinned public-domain Wine
stdio declaration independently records the six-argument common secure printf
shape; inherited UCRT headers define legacy-wide option4 and modern-rounding32.
Wide literals/fields use explicit16-bit units and the runtime's C-locale subset.
No locale package, external code download or complete UCRT implementation is used.


## Invalid-parameter callbacks

Pinned public-domain Wine stdlib.h declarations and Wine errno.c at
ff2631c6ed3d1721cd050d951671c307ade32b90 document the handler ABI, previous-pointer
exchange and thread-local-before-global/no-info dispatch contracts. These are
reference use only; no external handler/Watson implementation is copied or run.
The native component uses runtime atomic state and guest context-owned local
pointers; default fatal behavior stays an explicit unsupported Watson boundary.


## Initial thread suspension

Pinned Wine kernelbase-thread.c at ff2631c6ed3d1721cd050d951671c307ade32b90
separates stack commitment/reservation and CREATE_SUSPENDED, and wraps the native
resume previous-count result. This is contract reference only; no Nt thread,
signal-suspension or external implementation is copied/run. Ambrosia extends
its existing C++ condition-variable startup gate before guest session activation.


## Host-backed priority portability policy

Installed Linux man-pages sched(7) and glibc resource/scheduling declarations
record per-thread Linux nice values, SCHED_OTHER and privilege/limit constraints.
Pinned Wine kernelbase-thread.c supplies Windows get/set error/return contracts
only. No scheduler/kernel/Wine implementation is copied or run. The local host
adapter applies and verifies bounded nice requests; the portable layer records a
guest level only on host success. This is explicitly not a real-time scheduler.


## Native CPU affinity mapping

Thread group/mask adapters reuse the existing processor topology's saved dense
CPU identity table. Pinned Wine kernelbase-thread.c records GROUP_AFFINITY query/
set contracts and installed Linux sched(7)/sched.h document thread affinity calls.
No external scheduler, kernel or compatibility implementation is copied/run.
Local portable mask conversion is separate from sched_getaffinity/setaffinity;
set results and rollback are verified against the host rather than cached success.


`gdk_users.{h,cpp}` uses the inherited Windows user shim and pinned public GDK
interface lists for ABI facts (class identity and base slot order). Its owned
interface, subscription directory and queue retention are original native code;
no Windows shim implementation, fake account, XAsync provider or foreign code is
compiled or copied. Broader user/async/event behavior remains separate.


`game_input.{h,cpp}` uses inherited GameInput v0 declarations and a pinned public
export definition for interface/ordinal ABI facts. Its native counted identity,
empty-device collection and callback directory are original; SDL/Windows code is
not copied or compiled. Native interactive input remains a separate backend.


`storage_factory.{h,cpp}` uses pinned console DirectStorage ABI facts from cached
VIctory and xbox-emulator declarations: X2 identity and console configuration/
queue slots differ from the generic PC table. Its counted identity and actual
staging allocation are original native code. No external I/O/decompression code
is copied or compiled; future imports must retain their applicable licenses.


`text_conversion.{h,cpp}` links the installed ICU74+ Unicode core through its
public UTF conversion C API; Windows facades live in `nls_services.cpp`. The
system dependency's MIT notice is retained as LICENSE.ICU. ABI/error facts come
from pinned Microsoft SDK documentation. No external implementation is copied.


`readonly_files` and `file_services` implement original shared path/Win32 metadata
policy; `host_readonly_files` contains Linux descriptor confinement only. Public
pinned Microsoft API contracts and installed Linux UAPI declarations are references.
No external filesystem implementation is copied. No Windows/Wine runtime is used.

Task107's file-open signature/console file GUID and Close slot are compared with
the pinned Task045 public console ABI references. No implementation is copied;
Linux descriptor confinement remains behind the portable host-file boundary.
The existing runtime-owned interface pin/lifetime model is extended for real
file descriptors, with final guest release/Close disposing native resources.

Task108 follows the pinned official CreateFileW contract for read-only existing
opens, symmetric read sharing and metadata-only access. Linux O_DIRECT is requested
for the observed no-buffering flag and verified from descriptor status. Original
shared source ownership and opaque handle integration reuse the existing portable
object directory; no external implementation or host file handle is exposed.

Task109 follows pinned official GetFileTime/GetFileSizeEx contracts. Original
checked FILETIME conversion and descriptor-backed statx metadata add no external
implementation or dependencies. Linux timestamps stay behind the portable host
file boundary; Windows ABI output/error policy stays in file services.

Task110 follows the pinned official ReadFile contract for synchronous byte counts,
cursor and error behavior. Original portable ownership/serialization code calls
the narrow Linux pread/O_DIRECT adapter, with alignment reported by statx. No
external read implementation, fake payload/completion or host handle is exposed.

Task111 follows the pinned ReadFile synchronous OVERLAPPED contract for explicit
offsets, output status/count, EOF and cursor updates. A private bounded observer
and hash-verified local analysis identify the actual retry condition without
copying game code into source or changing a guest condition flag. Native reads
remain real; event/async behavior is not fabricated.

Task112 uses the installed Expat2.6.1 UTF8 library for original bounded package
descriptor parsing. Its MIT notice is reproduced in LICENSE.Expat. No parser or
package implementation is copied from another project. Pinned primary Microsoft
XPackage contracts and existing pinned public console ABI tables supply interface
facts; the process identity comes from matching local descriptors, not embedded
title values. Public upstream license notices remain intact.

Native graphics frontend uses public interface identities and vtable order from
Microsoft DirectX-Headers commit61c7718954ec73dbdd43c723240fe1a0452a3724 (MIT).
See LICENSE.DirectX-Headers. No SDK executable or game-derived declarations are
embedded. The initial frontend supports only explicit implemented methods and
stops on unsupported slots; it is not complete D3D12 conformance.

The console command-list identity is a public ABI fact from
[FiveStar's XboxDevice test support](https://github.com/ThatRecompDev/five-star-recomp/blob/323ea3000b9e6a49dbfda5c36ed0c1022fde90ae/tests/FiveStar.Loader.Tests/Support/XboxDevice.cs)
and its d3d12x-shim documentation at the same revision. Its source snapshot and
GPL-3.0-or-later notices remain pristine; no implementation is imported.

`graphics_recording` uses published object-size, buffer-bound offsets and table
capacity facts from
[XCL's title_list.rs](https://github.com/ZigZagBoi1/ScarlettPort/blob/48ba23a91a83121f8d0625a633e88fa463d5a0ca/runtime/xcl/crates/xcl/src/hle/d3d12x/title_list.rs).
That GPL-2.0-only source is a layout reference, not incorporated implementation.
The original native wrapper separates writable guest caches from C++ ownership
and uses this project's unchanged canonical `runtime/src/command_list.h` IR.
Equal owned write/end bounds select virtual-method fallback; changed bounds
terminate before Close/Reset instead of silently discarding unknown commands.
CPU recording lifetime does not imply GPU submission support.

The cache-barrier range validation and whole-memory ALL/COMMON fallback in
`graphics_recording.cpp` adapt the policy of `extended_cache_barrier` and
`global_cache_barrier` in [TuxBox commands.rs](https://github.com/brcly/TuxBox/blob/8661492d74886bc99ab7816745999bc02e2f393d/runtime/d3d12/src/commands.rs).
Apache-2.0 terms are preserved in LICENSE.TuxBox. This adaptation replaces its
Windows COM submission with canonical CPU IR recording, retains original request
metadata and adds bounded ownership, transition and atomic failure handling.
No TuxBox runtime, compiler or shader code is built or executed. Shader compiler
notices are unrelated to this command-recording policy and no shader source is used.

`frame_timing.cpp` adapts the timer-grid, current-origin polling, token-in event
and finite-timeout policy from [FiveStar FramePacer.cs](https://github.com/ThatRecompDev/five-star-recomp/blob/323ea3000b9e6a49dbfda5c36ed0c1022fde90ae/src/FiveStar.Loader/Shims/D3D12X/Present/FramePacer.cs).
Its copyright notice and GPL-3.0-or-later terms are retained; the project's GPL
license applies. The C++ component adds checked integer arithmetic, explicit time
inputs, bounded configuration and immutable wait plans. Existing host adapters
supply the monotonic clock and sleep. No Windows timer, C# runtime or upstream
executable is imported or run. Timer tokens do not certify GPU completion, and
presented-token/GPU throttling remains separate from this timer implementation.
