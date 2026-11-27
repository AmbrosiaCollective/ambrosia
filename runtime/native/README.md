# Native CPU bootstrap

This is the first part of a native Linux runtime, not a game launcher. The
portable C++20 core maps tiny original AMD64 PE fixtures, applies DIR64
relocations and binds imports through explicit adapters. Linux tests verify a
six-argument Microsoft x64 ABI call, including stack arguments, returning 21.
Synthetic checks require no game data or Windows SDK. Numeric formatting uses
the portable fmt headers; guest execution does not use Wine or Proton.

## Build

Use an existing C++20 compiler, CMake 3.24 or newer, fmt 9.1 or newer, ICU 74
with its uc component, and UTF8 Expat 2.6 or newer. Formatting links fmt headers;
text conversion and bounded package descriptors link ICU and Expat. These commands
do not download dependencies. Choose a build path outside the source/package trees:

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

Local Linux validation uses GCC 13 and Clang 18 with warnings treated as errors.
Original synthetic fixtures exercise the native shared-object entry under
reviewed Python launchers that block network and process execution, hide
credentials/home and mount source read-only. Separate bounded profiles permit
reviewed native workers and read-only local game startup; an interactive menu
has not been verified. Offline CMake separately built
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

## Diagnostic startup

`--trace-startup IMAGE --package PACKAGE` is an explicitly diagnostic CPU mode.
It maps the selected modules, binds real native services and bundled exports,
and assigns non-returning stops only to unresolved imports. Every stop terminates
the isolated process with exit 78 and the missing symbol. Linux signal faults
terminate with exit 79 and a module-relative location. A returned entry produces
diagnostic JSON and exit 3, never game-launch success. Inspection remains NX and
normal bootstrap execution retains strict metadata/import checks.

This mode permits bounded passive metadata for diagnosis while its full semantics
are unfinished. It does not implement Windows exception handling or a complete
retail process environment. It must use the reviewed OS sandbox; its signal
handlers are diagnostics within that isolated process, not containment. It
installs an alternate signal stack and restores previous dispositions on normal
return. Current usage is single-threaded, with process/thread/network creation
denied and GPU/display devices hidden. Generated stop code is sealed RX before
initialization and never returns fabricated API values.

Both native builds execute supplied PE startup code beyond the first clock call
and now stop at `CreateEventW`, after native-service handles and dynamic exports. Stage traces verify module initialization
completes and the executable PE entry runs. WinMain/the main game loop, graphics
and the interactive menu remain unverified. The read-only package and
actual runtime code now guide the next service implementation. Private trace
records and input bytes remain outside Git.

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
source bytes and 392 MiB mappings. The CLI releases its redundant input buffer
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

At the inspection-only milestone the graph contained 1,186 static import entries,
861 unresolved entries
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

`unwind_program.*` decodes direct UNWIND_INFO and restores prologue/body frames
against an explicitly bounded stack view, preserving the caller's input context
on failure. It covers nonvolatile integer/XMM saves, small/large allocations,
frame registers, partial prologues and machine frames. Handler/chain metadata is
retained but never invoked. A bounded chain walker now rejects cycles, excessive
depth and inconsistent frame registers, and pops the caller return only once.
`unwind_epilogue.*` recognizes complete supported AMD64 epilogues before restoring
any copied state: stack adjustments, frame LEA, integer pops, returns and selected
tail jumps. Unsupported patterns do not match. Full CONTEXT adapters, remaining
epilogue cases and exception dispatch are still needed before RtlVirtualUnwind
can be exposed. Version-two epilogue opcodes fail explicitly.

A non-executable RAM image of non-code sections lets both compiler builds decode
all 299,257 unwind programs in the actual executable: 515 have chains and 596
have handlers. All chains validate under both builds, with a maximum depth of
four records. This establishes metadata coverage, not runtime frame-unwind
correctness or game execution. No game bytes are saved in fixtures or Git.

`clock_services.*` supplies UTC GetSystemTimeAsFileTime, monotonic
QueryPerformanceCounter/Frequency and current process/thread IDs through
`host_time.cpp`. FILETIME uses 100 ns ticks since 1601, preserves four-byte output
alignment and performs checked epoch conversion. Performance counts are native
steady-clock nanoseconds with a fixed 1 GHz unit; this does not claim nanosecond
hardware resolution. Linux IDs come from the isolated process/thread namespace.
Only valid output addresses are supported; null outputs fail explicitly, other
bad guest pointers may fault. Successful queries preserve last error. Worker
thread behavior and Windows/macOS adapters remain untested.

`critical_sections.*` owns up to 4,096 native recursive mutexes per runtime.
Guest critical-section objects retain their opaque 40-byte x64 layout, without
embedding host mutexes. Initialize/enter/try/leave/delete/reinitialize and stored
spin-count updates are implemented. Invalid lifecycle, foreign-runtime objects,
unowned leaves, active deletion and unsupported flags fail explicitly. Void API
failures terminate rather than returning invented success. Spin values remain
hints; exact Windows spin scheduling and debugger internals are not reproduced.
Recursive single-thread behavior is verified; contention/fairness and worker
thread teardown remain unrun under the current process/thread-denying profile.
Runtime teardown requires all locks released and guest users finished.

`fls_slots.*` owns 128 reusable process-local FLS indices with separate values
for up to 64 guest contexts. FlsAlloc/GetValue/SetValue/Free and the observed
API-set aliases are implemented. Free invokes callbacks for all non-null context
values; context exit invokes remaining callbacks with guest GS/TLS active and
module mappings retained. Values clear before callback dispatch, callback code
runs outside the registry lock and freed indices cannot carry stale values into
reuse. FlsGetValue clears last error on success, including a null value.

The current guest context represents one primary fiber; actual fiber creation,
switching and worker-thread contention are not implemented or validated. Highly
reentrant callback/index replacement patterns need further coverage. Exit hooks
are owner-thread-bound, run in reverse registration order and are capped at 64.
Guest execution/mappings and the service scope must enclose cleanup. Callback
faults terminate diagnostic execution instead of pretending cleanup succeeded.

`host_stack.cpp` discovers native stack bounds dynamically (Linux pthread
attributes, with Windows/macOS extension points). Guest NT_TIB fields now include
StackBase at 0x08, StackLimit at 0x10 and Self at 0x30; TLS at 0x58 and last error
at 0x68 remain unchanged. Self refers to the guest anchor, never a host TEB.
Linux bounds are the extent reported by pthreads, not an independent probe of
Windows-style committed/guard pages. This does not create a separate guest stack
or supply a complete TEB/PEB, exception list or fiber environment. Tests verify
current-frame containment, generic self traversal, nested contexts and GS restore.
Windows/macOS stack adapters remain unrun.

The bootstrap still rejects exception registration,
delayed imports and other unsupported directories. Its parser is not a security
boundary or support claim for arbitrary PE files. Guest exceptions/unwinding,
guest file/storage services, audio and graphics remain future work. Existing
Vulkan code should be reused behind a platform surface/input adapter.

The next game milestone is a correctly presented interactive menu. Correct world
rendering and stable 30 fps with minimal stutter follow. Performance unlocking
comes later. Local development continues in small commits; GitHub publication is
on hold until explicitly requested.

## CRT initialization and cleanup tables

The scoped service runtime owns bounded on-exit callback arrays. The 24-byte x64
guest table exposes their first/last/end pointers, but foreign pointers are never
followed. Initialization preserves an existing valid batch; execution clears its
guest view before invoking callbacks in reverse order. Callback-time registrations
form a later batch, and callback return values do not interrupt cleanup. Limits
are 128 tables, 4,096 entries per table, 65,536 total pending/executing callbacks
and 32 nested executions. These are explicit local budgets, not Windows maxima.

`_initterm` and `_initterm_e` walk bounded arrays forward, skipping null entries;
the latter returns the first nonzero initializer result. Microsoft x64 wrappers
are registered for ucrtbase, msvcrt and the CRT runtime API set. This does not yet
provide process-global atexit, full CRT startup or automatic process termination
cleanup. Synthetic callback tests do not establish guest exception support.

`InitializeSListHead` initializes the x64 16-byte empty sequenced-list header,
including its format selector, through Win32/API-set and native aliases. It
requires a valid 16-byte-aligned destination and preserves last error. Subsequent
interlocked push/pop/flush operations remain unsupported and unresolved.

## Module thread-notification policy

`DisableThreadLibraryCalls` resolves only exact loaded DLL bases through a scoped
module graph. Unknown/interior/executable handles, inspection-only images and
static-TLS modules fail. Success records a persistent per-image notification flag
and preserves last error; failure reports module-not-found. Process attach/detach
remain active. The notification dispatch boundary checks this flag for thread
reasons, but a worker-thread launcher and thread attach/detach execution are still
unimplemented and blocked by the current sandbox. Synthetic DLL initialization
checks exercise the service ABI, state change, scope restoration and process
detach. They do not establish multithreaded behavior. The graph must outlive its
service scope.

## UCRT standard-stream identities

`__acrt_iob_func` supplies stable, distinct runtime-owned opaque FILE identities
for stdin/stdout/stderr. UCRT's public x64 FILE is pointer-sized; host libc FILE
is never exposed or reinterpreted. Recognition uses exact owned addresses and
rejects foreign or other-runtime identities without dereferencing them. The
accessor preserves last error and follows the scoped runtime across nested calls.
Unsupported indices or access without a scoped runtime stop diagnostics. Other
stream operations remain unresolved; this is not yet a complete stdio adapter.

## Bounded CRT heap and errno

The scoped runtime owns malloc/calloc/realloc/free allocations and base aliases.
The current local budget is 64 MiB and 65,536 allocations, independent of the
virtual-memory service budget. Zero-size allocation reserves one freeable byte;
calloc checks multiplication overflow and clears storage. Realloc preserves the
old allocation on failure and its byte prefix on successful resize. Null realloc
allocates; resizing a live allocation to zero releases it. Exact ownership checks
reject foreign/double-free pointers before host libc sees them. Runtime teardown
releases outstanding allocations after guest/context cleanup. Concurrent guest
workers and new-handler behavior are not implemented or tested.

Allocation exhaustion reports guest ENOMEM through per-context 32-bit CRT errno;
it does not expose host libc errno or modify Win32 last error. _errno/_get_errno/
_set_errno retain Microsoft ABI and nested-context isolation. Calls outside an
active guest context use a separate native-thread fallback. Aligned allocation,
heap size queries and the Windows process heap remain separate missing services.

## Internal CRT locale lock

The observed _lock_locales/_unlock_locales boundary uses a private runtime-owned
recursive critical section. It enforces real ownership and balanced release,
without consuming guest critical-section registrations or changing host libc
locale. Process/context error values survive lock operations. Missing service
scope, unmatched unlock and invalid runtime teardown fail explicitly. No worker
contention or locale conversion/configuration support is claimed. The internal
zero-argument void ABI is provisional and validated only against current startup;
a pinned contract refresh is still required.

## Runtime-owned pointer encoding

EncodePointer/DecodePointer use a stable reversible full-width XOR/rotation with
a private secret per scoped runtime. Null and arbitrary x64 bit patterns round-
trip without being dereferenced. The transform is local; Windows does not supply
its exact encoding algorithm here. It is obfuscation, not pointer validation or
cryptographic authentication. Win32/API-set aliases preserve CRT/Win32 errors.

The host entropy adapter uses installed standard C++ random_device and requires
positive reported entropy plus a nonzero result. Unavailability fails runtime
construction, with no fixed/address-derived fallback. Secrets never enter reports.
Only Linux was run; other hosts may fail explicitly depending on their standard
library. System-wide/cross-process pointer encoding is not provided.

`_get_stream_buffer_pointers` returns stable addresses of the owned stream's
base/cursor/count fields, with each output optional. Writes through those addresses
update the actual stream state. Each stream has independent initially empty
fields; public FILE identity remains opaque and pointer-sized. Foreign/null
streams fail EINVAL before output changes. This does not yet supply buffering
I/O, flushing or ownership of buffers installed by callers.

## Guest C locale

The guest owns a C locale independent of host LANG/environment. setlocale queries
with null or selects C/default for guest categories 0..5 using stable owned
strings. Other locale names and invalid categories fail EINVAL without changing
the previous locale. The existing recursive locale gate protects selection.

The supported C state supplies codepage-zero, single-byte-length and six empty
NLS-name queries, plus __pctype_func's stable EOF/256-byte classification table.
ASCII upper/lower/digit/space/punctuation/control/blank/hex/alpha masks are owned;
high bytes remain unclassified. No host libc locale or ctype table is exposed.
These properties apply only to the supported C state, not arbitrary locales.
Non-C locales, conversions and Windows NLS are unfinished. Guest category/CRT
property details still require the deferred pinned contract refresh; current
evidence covers synthetic invariants and observed startup.

## Address-to-image lookup

RtlPcToFileHeader searches mapped image ranges in the scoped module graph and
returns/writes the containing image base. Ranges include headers/section gaps and
exclude their upper boundary. Unknown PCs clear the output; no supplied PC is
dereferenced or matched against host mappings. Subtraction avoids overflow.
The lookup enforces graph ownership, works during active sessions and preserves
CRT/Win32 errors. Null output or missing scoped graph stops diagnostics. It does
not implement unwind lookup or dynamic DLL loading.

## Owned CRT startup state

The command engine binds the resolved executable path once before initialization.
Narrow CRT argv uses owned UTF-8 path storage, not host command-line arguments;
borrowed pointers survive through teardown. Application type, text/binary mode
and commit-mode storage belong to the runtime. Invalid type/mode values fail
without replacing valid state. Unicode file modes and actual I/O remain separate.

The CLI forwards no game arguments: argv modes 1/2 publish the path as argv[0]
plus a null terminator; mode0 publishes zero argc/null argv. There is no argument
wildcard to expand because argv[0] is excluded. Supporting forwarded arguments
requires actual parsing/expansion. WinMain's argument tail is owned and empty.
Initial/current guest environments use independent stable empty arrays and never
inherit host environment values. Wide startup, environment mutation and full CRT
entry/termination semantics are unfinished. This is real startup data, not a
complete game entry contract.

## CRT cleanup registration

_crt_atexit/atexit retain correctly typed void callbacks in a runtime-owned list.
Registration does not run guest code. Cleanup clears a batch before invoking it
in reverse order; callback-time registrations form a later explicit batch. Limits
are 4,096 pending/executing callbacks and 32 nested cleanup calls. Callbacks run
outside the registry mutex. No arbitrary host libc atexit registration is used.

After a bootstrap returns, the CLI reactivates its retained guest context for CRT
cleanup before module process detach, while TLS and image mappings are alive.
The execution scope enforces its owner/lifetime and restores native GS. A tiny
original PE fixture verifies attach, entry, cleanup-with-TLS and detach ordering.
Runtime destruction does not run guest callbacks after mappings disappear. Fatal
diagnostic stops terminate immediately. exit/quick_exit/_cexit, stdio flushing
and full process termination remain unresolved; registration does not claim them.

## Context-local CRT locale mode

_configthreadlocale supports query0, enable1 and disable2 with previous-mode
returns. New guest contexts default to shared locale mode. Enabled contexts own
independent C locale/classification snapshots; disabling selects the shared state.
Invalid flags preserve the mode and fail EINVAL. Configuration requires an active
guest context. Process-global default flags and non-C locale state remain
unsupported; the mode contract still needs the deferred pinned CRT refresh.

A private instance of the existing FLS owner handles storage and context-exit
cleanup without consuming guest FLS indices. Its retained state safely outlives
an earlier facade destruction. No host libc locale changes or worker-thread
launches are involved; independent contexts were tested sequentially.

Diagnostic traces mark module initialization, executable entry and returned-entry
cleanup separately. An original PE fixture checks these stages against real TLS/
entry/cleanup/detach order. A stage marker is not interactive-menu evidence.

## Loaded PE module handles

GetModuleHandleA/W return actual mapped image bases for null/root and loaded
basename queries. ASCII case and default DLL extension rules are shared with
module registration; UTF-16 names convert to the native UTF-8 name policy.
Queries do not load source-only images, read new files or increment references.
Unknown modules return null/module-not-found; successful calls preserve errors.
Handles remain stable while graph mappings are retained. Full-path lookups
remain explicitly unsupported. Native service-module handles now use the owned
PE facades described below.

## Native-service PE facades and dynamic exports

The registered ABI adapters now have real owned PE32+ metadata. A canonical
ambrosia-native.dll image owns one RX tail trampoline per unique native target;
named facades publish PE forwarders there. Static imports, GetProcAddress and
module aliases retain the same callable pointer. Headers/export data are read-
only; code pages are RX. No copied system DLL or untrusted setup/runtime is used.
These are fixed mapped views with no entrypoint, imports or TLS, not disk-loadable
replacement Windows DLLs. Public Windows ordinal compatibility is not invented.

The catalog caps each image at 512 KiB/4,096 exports and the aggregate at 128
images/16 MiB. Facades appear lazily for registered modules; arbitrary requested
names cannot create a library. Native address-to-image queries include the
canonical stubs. The eight package modules reported by the startup trace remain
a separate count from these generated service views.

GetProcAddress returns named implemented facade exports or real loaded PE name/
ordinal exports. Read-only forwarder lookup reaches loaded images or registered
services without invoking the package provider or changing dependencies. Missing
exports return procedure-not-found, invalid handles fail separately and native
service public ordinals resolve only explicitly registered aliases; unknown public ordinals fail lookup. This does not supply dynamic
DLL loading, unsupported APIs, Windows SEH or complete system-module behavior.

## Owned events and waits

CreateEventA/W, SetEvent, ResetEvent, WaitForSingleObject, non-alertable
WaitForSingleObjectEx and CloseHandle now operate on runtime-owned event objects.
Manual-reset events release the registered waiter set and remain signaled until
reset. Auto-reset events hand a signal to one waiter or retain one available
signal. A later reset or poll cannot revoke an assigned handoff. The portable
state model is separate from the standard C++ mutex/condition-variable adapter.

Opaque handles never expose host pointers or file descriptors. Named aliases
share state, retain the original mode and survive until the last owned reference
is released. Names are case-sensitive UTF-16; narrow names use the runtime's
UTF-8 policy. Local-prefixed names share the private runtime namespace. Limits
are 4,096 handles, 1 MiB of name text, 64 queued waiters per event and the API's
MAX_PATH name bound. Invalid handles, exhaustion and timeouts report failures.

The 600 CPU checks include deterministic handoff/reset/cancel interleavings,
finite single-thread timeouts, named lifetime and Microsoft ABI error behavior.
They do not establish concurrent guest-thread correctness. Cross-process/global
namespaces, security/inheritance attributes, alertable waits/APCs and other
waitable object types remain unsupported. No worker thread or GPU is started.


## Private registry key lookup

RegOpenKeyA/W, read-only RegOpenKeyExA/W and RegCloseKey use a bounded guest
configuration store. The five ordinary predefined roots exist; their children
start empty. Host-side configuration can define keys explicitly. Opening never
creates missing keys or reads host settings. ASCII names are case-insensitive;
registry backslashes are logical separators independent of the host filesystem.

Legacy empty-subkey calls return the original handle. Extended calls duplicate
ordinary handles and preserve predefined roots. Closing an opened handle leaves
configured keys and other handles alive. Registry and event directories share
monotonic opaque IDs, so neither can consume the other's handles. CloseHandle
reports success for a registry handle without closing it; RegCloseKey owns that
operation. Registry status returns preserve last-error and CRT errno.

Limits are 4,096 configured keys including roots, 1 MiB of logical path text,
4,096 opened handles, 255 characters per component and 32,767 per path. This is
an in-memory configuration namespace with no persistence or invented Windows
installation metadata. Values, guest key creation/deletion, Unicode case folding,
symbolic links, writes/security changes and 32-bit views remain unsupported.


## Graphics memory configuration policy

D3D12ConfigureVirtualMemoryX validates and retains the observed 32-byte profile:
all bytes zero except the second DWORD, whose supported classes are 3 and 4.
A class describes 16 or 32 GiB of range coverage under the pinned caller model.
Unknown shapes, repeated calls and configuration after successful backend creation
fail with HRESULTs. Caller input stays unchanged; each runtime owns its policy.
A scoped creation gate lets a future backend inspect the plan and mark a successful
device while serializing configuration. Failed creation leaves the gate reusable.

This adapts a pinned measured profile, not a complete proprietary SDK contract.
Policy acceptance allocates no CPU/GPU range and establishes no device capability.
A real backend must validate its capabilities, reserve the range and connect page
mapping before claiming support for those operations. Those paths remain unimplemented
here. The inherited Windows shim is separate; this native boundary does not use
its unconditional-success placeholder, Windows D3D12, Wine or Proton.


## XMem reserve-only windows

The measured five-argument XMemVirtualAlloc ABI can reserve real 64 KiB-aligned,
no-access windows through the shared memory owner. This initial profile accepts
only reserve-only 64 KiB mode and the known raw attribute/protection words.
Raw CPU/GPU/cache policy remains attached to the window for future physical-page
mapping. No physical pages or GPU range are fabricated. Unknown profiles fail.

Window sizes round to guest pages and count against the existing 64 MiB runtime
reservation cap. Whole-region VirtualFree releases both mapping and metadata.
Ordinary VirtualAlloc commit and VirtualFree decommit cannot replace a physical
window's backing semantics. Physical-page allocation/mapping, placeholder splits,
committed/large-page XMem modes and GPU bindings remain unsupported. Actual large
requests must fail within the current CPU-only sandbox until a reviewed resource
profile is explicitly approved; tiny tests do not validate those reservations.


## Explicit sparse-window budgets

Default memory construction keeps the shared 64 MiB cap. Explicit MemoryLimits
separate ordinary/private reservations, physical-page windows and their combined
total. Free releases the correct category; available window capacity never raises
the private cap. Only diagnostic --trace-startup accepts --sparse-windows, which
allows 49 GiB of no-access window reservations while ordinary allocations retain
64 MiB. The flag grants no OS privilege and does not change execution isolation.

Sparse actual runs require the explicitly approved outer-workspace sparse sandbox:
64 GiB address space, 512 MiB data, 8 MiB stack, no locked memory, and a sampled
512 MiB RSS watchdog. The watchdog is not a hard resident-memory cap. Mapping and
resource self-checks fail closed before native code; network, process/thread
creation, credentials and GPU/display remain unavailable. Tiny regression checks
continue under the original strict sandbox. No physical-page backing, GPU address
range, device or rendering support follows from a successful sparse reservation.


## Committed XMem CPU regions

Known measured committed profiles now create private zero-filled CPU mappings
with logical 64 KiB or 2 MiB alignment and rounding. Host pages remain ordinary
OS pages. Metadata retains the complete attribute word, GPU/cache protection,
logical page size and backing kind. Committed regions charge the private budget;
physical windows charge the separate window budget. Normal decommit/recommit and
whole-region release retain the right ownership and accounting.

Only the inherited measured page/attribute/protection combinations are accepted.
Unknown combinations, executable CPU protections, fixed-address replacement and
physical-page mapping remain unsupported. Retaining GPU/cache policy does not
implement GPU visibility, coherency or binding; a real backend must establish
those properties. The Linux host adapter supports this alignment; Windows/macOS
coverage remains unverified.

Diagnostic --boot-memory selects a 2 GiB private allowance alongside 49 GiB of
no-access windows. It grants no OS resources. Its separate approved boot-memory
sandbox raises the data/RSS watchdog bounds to 2 GiB; the earlier approved sparse
profile remains at 512 MiB. Tiny regression tests still use the original strict
profile. The larger approved diagnostic profile described below also passes
subsequent semaphore and committed-allocation startup steps. Successful fixtures
and startup steps do not establish menu or GPU initialization/rendering support.


## Owned semaphores and handle rights

CreateSemaphoreA/W and CreateSemaphoreExA/W now retain real permit counts and
maximums. ReleaseSemaphore validates a positive increment and the maximum before
assigning queued handoffs. Each released waiter owns its permit; later polls
cannot steal it. The portable count model shares the existing standard C++ wait
adapter with events. Both types share the bounded name/handle directory and
reject cross-type name collisions without changing the original object.

Each semaphore handle carries its own specific access mask. Generic read/write/
execute/all requests map to the corresponding semaphore rights. Waiting requires
SYNCHRONIZE; releasing requires SEMAPHORE_MODIFY_STATE. Named aliases can hold
separate rights while sharing counts. Creation reports new/existing status,
release writes its prior-count output only on success, and invalid/closed/type-
confused handles fail. This private access policy does not implement Windows ACLs,
impersonation, inheritance, Global namespaces or cross-process sharing.

Counts use legal signed 32-bit ranges, with overflow-safe release checks. Existing
limits remain 4,096 handles, 1 MiB name text and 64 queued waiters per object.
Deterministic signal interleavings, finite single-thread timeouts and ABI tests
provide CPU coverage; guest-thread contention and APC/alertable waits remain
unrun or unsupported. No new host thread or semaphore service is started.


## Bounded diagnostic private-memory option

Trace mode accepts --private-memory-mib N, a strict decimal value from 1 through
8192 MiB. Missing, signed, overflowing, suffixed, duplicate and conflicting
arguments fail before input loading. Inspection and normal bootstrap modes reject
it. Defaults and the fixed --boot-memory profile stay unchanged. Explicit private
selection cannot combine with that fixed profile; --sparse-windows remains the
independent 49 GiB no-access window option.

The selected cap reaches the real memory owner; combined accounting includes
both private and window allowances. A tiny original PE confirms that the same
2 MiB request fails under 1 MiB and succeeds under 2 MiB. Selecting 8192 alongside
sparse windows allows 56 GiB aggregate virtual reservations. The CLI grants no
OS resources and cannot bypass its execution sandbox.

The separate 8 GiB boot-memory profile is approved and passes fail-closed checks,
including refusal of above-limit writable mappings and protection upgrades. It
retains 64 GiB address space, read-only inputs and the earlier filesystem,
credential, network, exec, process/thread, GPU/display and mapping restrictions.
Its sampled RSS watchdog is not a hard RSS cap. Earlier profiles are unchanged.
Actual diagnostic runs pass the measured memory requests. The next topology
requirement is described below. No window, menu or rendering has been verified.
No GPU/thread or higher-memory integration follows from this option or its
approved profile.

## Logical-processor topology

GetLogicalProcessorInformation now has an owned topology snapshot and an explicit
32-byte Windows x64 record writer. A separate Linux adapter reads online CPUs,
current affinity, physical core/package relationships, NUMA node membership and
available shared-cache geometry. Guest processor bits are dense indices into the
retained host CPU-ID list. This mapping must also govern future affinity/thread
services; those services are not implemented by this change.

Size probes and short buffers report the required byte count without partial
output. Successful calls preserve LastError; invalid lengths fail. Core SMT
flags, cache types and fully-associative cache markers use the guest ABI, with
zeroed padding/reserved fields. Kernel32, KernelBase and the sysinfo API-set
aliases share one callable facade. CRT errno is independent.

The Linux reader bounds metadata text, IDs, selected processors and cache entries.
It refuses incomplete/conflicting relationships, offline selections, changing
online/affinity snapshots and cache fields that cannot fit the guest ABI. More
than 64 selected processors require a future group model and currently fail
explicitly. Unreported caches are permitted by the API contract; missing core,
package or NUMA metadata is not replaced with invented values. Windows/macOS
discovery remains unsupported. The retained snapshot does not track hotplug or
later external affinity changes.

Original sysfs fixtures and direct Microsoft-ABI calls cover these contracts in
the strict CPU-only sandbox. The existing boot profile intentionally hides sysfs:
an actual control trace resolves this import and stops at unavailable topology
metadata. The separate profile with a bounded read-only copy of CPU/NUMA text is
now explicitly approved and passes all isolation checks. It does not expose live
sysfs controls, unique hardware IDs or GPU devices. Both GCC and Clang actual
runs accepted topology and proceeded through subsequent allocation/semaphore steps
to exception-filter registration. Later startup work is described below. No window, rendering or menu
is verified. The earlier profiles remain unchanged.

## Top-level exception-filter registration

SetUnhandledExceptionFilter atomically replaces a process-owned callback value
and returns its previous value. Null selects default handling. All guest contexts
using one Runtime share this state; separate Runtime instances remain independent.
Repeated registration preserves the prior pointer, and registration neither
dereferences nor invokes it. Successful calls preserve LastError and CRT errno.
Kernel32, KernelBase and the errorhandling API-set aliases share one native facade.

The original PE import fixture checks the full-width previous pointer and opaque
argument through the Microsoft x64 ABI. Separate guest contexts verify that the
state belongs to the process rather than one thread context. No guest-thread
concurrency run is claimed. Native signal dispositions remain unchanged.

This implements registration, not exception dispatch. UnhandledExceptionFilter,
RaiseException and Linux-to-Windows exception/continuation translation remain
unsupported. Diagnostic faults still terminate without invoking guest callbacks
or pretending execution can resume. Actual startup passed registration and
reached CRT allocation-mode initialization. No window or menu is verified.

## CRT allocation-failure policy

_set_new_mode/_query_new_mode and _set_new_handler/_query_new_handler retain
process-owned atomic state. _callnewh invokes the current Microsoft-ABI callback
with the full requested size and normalizes its result to zero or one. Default
mode is zero with no handler; setters return the previous value without changing
successful Win32/CRT error state. Separate Runtime instances remain independent.

When enabled, failed malloc/calloc/realloc attempts call the handler after the
heap lock is released. A nonzero result retries allocation; zero or no handler
returns null with guest ENOMEM. A handler may free other storage or clear itself.
Zero-byte malloc/calloc requests normalize to one byte; overflow and requests
beyond the x64 CRT maximum fail without invoking the handler. Failed realloc
retains the original block. Retry loops remain under execution time/resource
bounds rather than inventing an API-specific retry count.

The Runtime constructor accepts an explicit CRT heap budget for bounded use and
deterministic tiny fixtures; the default remains 64 MiB, independent of virtual-
memory caps. Tests recover allocations from a 64-byte heap, including calloc
zeroing and realloc prefix preservation. No host/guest concurrency coverage is
claimed. Invalid-mode parameter dispatch and guest C++ exception propagation
remain unsupported and stop explicitly rather than returning fabricated results.

Actual startup accepted allocation-mode initialization and proceeded into CRT
math. No window, rendering or interactive menu is verified.

## Float tangent through the guest ABI

tanf uses the portable standard C++ float overload of tan through a Microsoft
x64 facade. It retains signed zero and calculated results, restores native errno,
and applies default guest CRT domain/range error codes. Non-finite inputs follow
the documented default domain policy; quiet NaNs are not converted into invented
floating-point traps. Host floating-point status reflects the actual calculation.
Successful calls preserve guest CRT errno and Win32 LastError. The API-set,
UCRTBase and MSVCRT module aliases share one entrypoint.

An original PE fixture passes pi/4 in XMM0 and receives the calculated float in
XMM0. Analytic values, signed zero, NaN/infinity, masked invalid status and error
isolation have CPU coverage. This does not certify bit-identical Windows libm
argument reduction, custom _matherr callbacks, unmasked guest SEH or all rounding/
denormal modes. Those remain separate compatibility work. Actual GCC and Clang startup now
passed the tangent call and reached command-line handling; no menu is verified.

## Process-owned command lines

GetCommandLineA/W return stable strings owned by the same startup state as CRT
argv[0]. They contain the quoted selected executable and no forwarded game
arguments, matching the existing empty WinMain argument tail. They never inherit
the native launcher's flags, environment or command line. The narrow form uses
the bootstrap's explicit UTF-8 policy; the wide form uses Windows UTF-16 code
units rather than the host's wchar_t. Legacy ANSI code pages are not emulated.

Binding validates and constructs all strings before publishing pointers. NUL,
unrepresentable quotes, malformed UTF-8 and oversized UTF-16 lines fail without
partially binding the process. The wide limit includes both quotes and the NUL
terminator. Binding is one-shot; later CRT argc/program-name changes do not alter
the original process command line. Callers must not modify or free these buffers.

Unicode/non-BMP names, spaces, stable lifetime, independent runtime state and an
original PE import have CPU coverage. Successful getters preserve error state;
an unbound process fails explicitly. Future forwarded arguments must update argv,
command-line quoting and the WinMain tail together. Actual startup now passes
command-line retrieval and proceeded into CRT string operations. No window or
menu is verified.

## Stateless strings and C-locale classification

The native registry covers ordinary length, compare, copy, bounded copy, append,
span/search and C-locale case/collation operations required by the executable.
Narrow lengths count bytes; wide operations use Windows UTF-16 code units rather
than host wchar_t. Copies return the original destination, strncpy/wcsncpy pad
their requested extent, and strncat terminates its bounded append without padding.
Comparison orders unsigned units and honors prefixes and explicit count limits.

The existing owned C-locale table supplies classification for bytes and EOF.
Case conversion and insensitive comparisons fold ASCII; C-locale collation uses
the corresponding unit comparison. No host locale/NLS state is borrowed. These
operations must be extended with locale selection before non-C locales can be
supported. Custom invalid-parameter handling, secure copies and tokenizers remain
separate requirements. Ordinary non-secure buffer contracts still apply.

Synthetic cases cover all registered operations, surrogate units, padding and
termination, returned pointers, unsigned ordering, EOF/high-byte handling and
error-state preservation. An original PE import exercises strlen through the
Microsoft ABI. Both actual builds now pass the string stage and reach atan2f.
No window, rendering or interactive menu is verified.

## Portable numeric math

The math facade now covers 35 imported standard numeric functions, using explicit
float/double overloads of the portable C++ library. Microsoft x64 entrypoints
preserve floating-point argument order and width, including mixed floating-point
and pointer arguments to modf/modff. Host long and long double are not substituted
for Windows types. CRT-specific classifiers, user math-error callbacks, lroundf
and nexttowardf remain separate unsupported imports.

Calculations retain real floating-point status, restore native errno, and map
libm domain/range errors to guest CRT errno 33/34. Successful calls preserve guest
CRT errno and LastError. Tangent additionally retains its documented non-finite
input policy. Windows libm bit identity, all rounding/denormal modes and every
exceptional-value policy are not certified; custom _matherr handling is absent.
These adapters do not manufacture success for unsupported CRT services.

Typed analytic checks exercise every registered function and alias, signed zero,
infinite arguments, domain/range errors and guarded pointer outputs. An original
PE fixture passes atan2f arguments in XMM0/XMM1; a swapped-argument control fails
its numeric and PE checks. GCC and Clang each pass 915 checks. Actual startup now
passes these calls and reaches CreateMutexA. No window or menu is verified.

## Owned process-local mutexes

CreateMutexA/W return opaque handles in the same bounded typed directory as
events and semaphores. Initial ownership applies only to a new object. Existing
names return an independent handle with ERROR_ALREADY_EXISTS and retain their
original owner. Narrow names use the runtime's existing UTF-8 policy; UTF-16
names and Local prefixes share the same process-local identity. Security/
inheritance attributes and cross-process Global names still stop explicitly.

Single-object waits acquire ownership and count recursive acquisitions. Only
the owning guest context can release it; each acquisition needs one release.
Guest thread identity is independent of host thread ID. One guest exit callback
per directory abandons all mutexes owned by that context, so the next waiter
receives WAIT_ABANDONED and ownership exactly once. Weak cleanup records preserve
last-close semantics and remain safe if a runtime is destroyed first. Object/
owner records are bounded to 4096 and blocking waiters to 64 per mutex.

Distinct synthetic guest contexts exercise ownership, finite timeout, recursion,
80 simultaneous abandoned objects, repeated acquisition, name/type collisions,
last-close and teardown. A small original PE calls CreateMutexA through the ABI.
GCC and Clang each pass 932 checks; a collapsed-recursion control fails four
intended checks. Host-thread wakeup/race scheduling is untested under the current
thread-blocking profile. CreateMutexEx/OpenMutex, multiple-object waits and full
Windows security remain unsupported. Actual startup now reaches CRT formatted
output; no window, rendering or interactive menu is verified.

## Windows argument slots and portable buffer formatting

__stdio_common_vsprintf decodes the guest's consecutive eight-byte argument
slots into typed values for the installed fmt header-only printf formatter.
A Windows va_list is never passed to host libc. Integer widths follow LLP64,
including promoted narrow arguments and I32/I64/I modifiers. Floating arguments
and Windows long double use double. Narrow string width/precision count bytes;
UTF-16 strings/characters currently cover C-locale ASCII only. The adapter corrects
fmt 9's precision-zero integer behavior, preserving alternate octal and signs.

Supported options include standard snprintf sizing, legacy termination for
nontruncated output, the narrow API's legacy-wide-specifier flag and modern
rounding. Floating conversions require modern rounding and FE_TONEAREST; legacy
rounding and other FPU modes stop explicitly. Standard snprintf returns the full
required count and terminates a nonempty destination. Legacy/default truncation,
explicit locales, pointer/count/positional conversions, non-ASCII wide encoding
errors, invalid-parameter dispatch and other options remain explicit stops.

Formats/strings and fields are bounded to 32768, argument slots to 128 and final
output to 1 MiB. These are runtime budgets, not full CRT size limits. Successful
calls preserve host errno, guest errno and LastError. Format parsing, byte bounds,
numeric results, signed zero/nonfinite output, standard truncation guards and a
six-argument original PE call have CPU coverage. Both compilers pass 948 checks;
a destination-bound control fails two intended checks. Seven unsupported-mode
probes stop rather than returning invented results. Full Windows printf parity
and all floating-point environments are unverified.

Offline CMake builds the CLI/test executable with existing fmt 9.1 headers; no
package is downloaded. GCC/Clang actual startup now reaches _time64. No window,
rendering, frame loop or menu is verified. The compiler-only profile has a
separately reviewed 120-second wall budget for larger translation units; tests
retain the strict execution profile and no compiler output is run by that helper.

## CRT epoch time

_time64 uses the existing portable host UTC clock and checked integer conversion
from FILETIME ticks to nonnegative Unix seconds. Epoch validation precedes
subsecond truncation, and the full tick range fits the signed 64-bit result.
Calendar/timezone conversion limits are kept separate. An optional output receives
exactly eight bytes; native time_t layout and host timezone are never exposed.
Host clock failure returns -1 and sets guest errno to EINVAL. _difftime64 converts
operands before subtraction, avoiding signed integer overflow. Successful calls
preserve native errno, guest CRT errno and LastError.

Synthetic checks cover epochs/subseconds, full-range conversion, guarded output,
optional null output, real-clock bounds, difference ordering and a PE import.
Other imported calendar, local-time and strftime services remain unsupported.
Both compilers pass 956 checks. Actual startup now reaches rand_s; no window or
menu is verified. This is UTC startup support, not complete Windows time parity.

## OS-backed CRT random values

rand_s requests a complete unsigned 32-bit value from a dedicated host entropy
boundary and copies exactly four bytes into the guest output. Linux uses
getentropy; other host implementations currently fail explicitly. It never
substitutes CRT PRNG state, pointer secrets, fixed seeds or constants. A successful
call returns zero and preserves native errno, guest errno and LastError. Null
output and OS-source failure stop explicitly until full invalid-parameter and
CRT error policy are modeled; failure cannot return success or an uninitialized
sample. Existing strict isolation permits this OS entropy call without new host
access or dependencies.

Guarded output, aliases, error preservation and a small original PE have CPU
coverage. Random outputs are allowed to be zero or repeated; these checks do
not claim a statistical entropy audit. An injected source failure and null probe
stop, while the same valid normal-source probe returns. Both compilers pass
961 checks. Actual startup now reaches GetTickCount64. No window/menu is verified.

## System uptime

GetTickCount64 and GetTickCount read a dedicated native boot-time clock, not a
wall clock or runtime/process-relative timer. Linux uses CLOCK_BOOTTIME so system
suspend contributes to elapsed uptime; the Windows host boundary delegates its
native API. Millisecond conversion validates seconds/nanoseconds and rejects
overflow. The 32-bit facade truncates modularly; both calls preserve host errno
and guest error state. Host clock failure/unsupported hosts stop explicitly.

Synthetic tests cover unit boundaries, malformed/overflow values, uptime beyond
32 bits, monotonic native-clock bounds, aliases/errors and an original PE import.
Both compilers pass 969 checks. Suspend/resume and a live 32-bit wrap were not
forced; Windows compilation remains unverified. Actual startup now reaches
executable TLS cleanup registration. No window or interactive menu is verified.

## Executable CRT TLS cleanup

_register_thread_local_exe_atexit_callback retains one typed process-owned TLS
cleanup callback. Registration replaces the slot; null clears it. Cleanup consumes
it before ordinary process callbacks, with null module/reserved and process-detach
reason zero. The CLI already retains guest TLS and mapped modules through this
stage; PE loader TLS notifications remain a separate lifetime boundary.

A TLS destructor may register an ordinary callback for the current cleanup;
callbacks registered by ordinary callbacks retain the existing later-batch
policy. Directory locks never span guest code; recursion/callback budgets remain.
Replacement/clearing/once behavior, aliases/errors, original PE registration and
full CLI attach/entry/TLS/ordinary/detach order have CPU coverage, including GS
restoration. Both compilers pass 979 checks; wrong-reason control fails two checks.
Worker-thread detach/creation and full Windows CRT exit parity remain unverified.
Actual startup now reaches GetStartupInfoW. No window/menu is verified.

## Owned startup-info record

GetStartupInfoW copies a fully initialized 104-byte Windows x64 launch record.
The native launcher currently supplies no desktop/title, inherited standard
handles or STARTF flags. Its default show policy is therefore SW_SHOWDEFAULT.
A configured show mode sets STARTF_USESHOWWINDOW before executable binding;
binding freezes the launch settings. This reports launch metadata and does not
claim to create or display a window. Other native host launch details are not
inherited or exposed.

Explicit alignment fields prevent reserved/padding bytes from carrying native
memory into the guest. Tests cover nondefault settings, validation/freeze,
independent runtimes, guest output edits, exact layout/guarded copy, preserved
errors and an original PE query. Both compilers pass 988 checks; a zero-show
control fails four intended assertions. Layout references are pinned FFI and
primary launch-ownership documentation; Windows SDK conformance is unverified.
Actual startup now reaches AcquireSRWLockExclusive. No window/menu is verified.

## Slim reader/writer locks

The seven Win32 SRW initialize/acquire/try/release APIs use owned portable mutex
and condition-variable state. Static zero initialization registers lazily; the
opaque eight-byte guest cell never contains a native pointer. Shared acquisitions
coexist and require balanced releases; exclusive acquisitions exclude readers
and writers. Try calls return the Windows BOOLEAN result for actual contention.
Blocking predicates prefer waiting writers to new readers without promising FIFO.
SRW counters do not confer mutex-style recursive ownership or abandonment.

Address records/readers are bounded to 4096 and blocking waiters to 64 per lock.
Malformed/unowned nonzero state, wrong-mode release and reinitialization while
held fail explicitly. Exclusive recursion/upgrading retains the API's deadlock
boundary rather than fabricated success. Same-thread shared recursion can deadlock
behind a queued writer and must not be relied on. Directory locks do not span
blocking waits; guest state and native side tables remain separate per runtime.

Both compilers pass 1005 checks, covering exclusion/coexistence, count/record
limits, static/explicit init, error/alias behavior and a PE BOOLEAN call with real
contention. Constant-success control fails three intended assertions. Offline
CMake links both targets. Host-thread wakeup/contention and queued-writer scheduling
remain untested under the thread-blocking profile. Actual startup now reaches
GetModuleFileNameW; no window or interactive menu is verified.

## Recorded module source origins

Mapped modules may carry an explicit absolute source path. The native CLI records
the selected executable's canonical path; lazy package loads retain their indexed
source filename case through an optional metadata provider. Origin queries neither
load source-only images nor reread DLL contents. Pure in-memory graphs remain
supported but cannot invent a file-backed origin. Source metadata is independent
of guest-writable argv/program-name/command-line buffers.

GetModuleFileNameA/W resolve the root or exact loaded module bases and copy that
origin. Capacities/counts use UTF-8 bytes or Windows UTF-16 units under the existing
bootstrap policy. Truncation terminates, returns capacity and sets error 122;
unknown/interior handles report 126 without writing a filename. Zero capacity
returns zero/error 122. Successful calls preserve native/CRT errno and existing
LastError. Native service facades have no file-backed source and stop explicitly.

The path spelling remains the dynamically selected native source path. A Windows
virtual drive namespace, legacy ANSI pages and guest filesystem operations are
separate compatibility work. Synthetic checks cover Unicode/non-BMP counts,
source case, loaded-only lookup, no-extra-read behavior, buffers/guards and a PE
query. Both compilers pass 1022 checks. Actual startup now reaches LoadLibraryExW;
no window or interactive menu is verified.

## Counted dynamic library references

LoadLibraryA/W and Ex variants can retain process-pinned native service facades
or already initialized package modules. FreeLibrary balances only these extra
references; non-owning handle queries do not grant a release. Static-import graph
modules and the native catalog remain pinned through process cleanup, with real
PE handles/exports rather than host DLLs or arbitrary pseudo-handles. Extra
reference totals cap at 4096 across at most 128 handles and recover after release.

The subset supports basename/default-extension/case/trailing-space lookup and
flags 0, system-only 0x800 or default-directory 0x1000. Missing modules return
126, null names/reserved file arguments return 87, unowned releases return 6.
New package-image activation, full paths, extension-suppressed names, resource
mappings and other flags stop explicitly. Native Windows system directories are
never searched. Full Windows search/unload semantics are not implemented.

Both compilers pass 1035 checks, including balanced repeated loads, bounded state,
real catalog exports, initialized package reuse and an original PE Ex call.
Coalesced-reference control fails two assertions. Private nonreturning observers
identify a required xgameruntime.dll bootstrap call; those observers are not
production GDK implementations. Actual production startup currently reports that
missing DLL and exits 133. Native GDK services remain required; no window, menu or
rendering is verified. Do not replace this failure with a fabricated success.

## Native offline GDK bootstrap

The owned xgameruntime facade now exposes the initialization variants, cleanup,
error reporting and an explicit unsupported-class query boundary. Initialization
retains process-owned references and records requested SDK revision pairs; final
cleanup clears the session without underflow. Limits are 4096 references and 64
revision pairs. Error reports retain the latest owned code/text within the string
budget instead of returning an empty success. Runtime instances remain independent.

The observed Ex/Ex2 mode 2 with null options is supported. Other modes/options
stop until modeled. This initializes only the offline bootstrap context, not the
whole GDK: QueryApiImpl resolves only explicitly supplied native classes. Other
class requests clear the output and stop with private diagnostic evidence.
No online Gaming Services client or claim of working async queues is included here.

Both compilers pass 1052 checks, including limits/cleanup, report ownership,
dynamic/static PE facade identity, errors and a four-argument original PE call.
Coalesced-initialization control fails six assertions; four unsupported-mode/query
probes stop explicitly. Actual startup now initializes the bootstrap and requests
XThreadingImpl, rather than failing to find the runtime DLL. Class/queue services
remain required. No window, rendering, frame loop or interactive menu is verified.


## Native GDK interface lifetime and thread properties

RuntimeInterface supplies the manual Windows GUID/IUnknown prefix used by the
owned XThreadingImpl object. QueryInterface supports declared IDs with null-on-
failure outputs, AddRef/Release use fixed ULONG widths, and a runtime pin retains
the backing object through process cleanup. Guest reference counts cap at 4096.
This avoids relying on the native compiler's C++ virtual layout or process globals.

Threading slots 27/29/30 implement mutable per-guest-thread sensitivity and its
blocking assertion. Flags are independent between guest contexts even on the same
host thread; native scheduling priority is not changed. Async/task-queue/private
slots remain nonreturning diagnostic boundaries, not dummy success methods.
Returning the known threading identity does not claim all its methods are ready.

Both compilers pass 1067 checks, including interface IDs/references, guarded
unaligned outputs, per-runtime/per-thread state, error preservation and an
original PE query plus bool-returning vtable call. Unsupported slot/assertion/
reference-limit probes stop explicitly. Final control fixtures use unconditional
cleanup so an intentionally broken getter cannot mask later checks by leaving a
thread marked sensitive. Actual startup reaches slot 12, XTaskQueueCreate.
No queue, host worker thread, rendering or interactive menu is verified yet.


## Native task queue creation and scheduler

Task queues own opaque, nonreused handles and separate work/completion dispatch
modes. Creation, duplication and close are available through flat exports and
XThreadingImpl slots 12/15/17. Duplicates retain the same queue after the original
handle closes. Each runtime allows 128 open queue handles. Manual/immediate-only
queues do not create workers; thread-pool modes lazily start two shared native
workers. Failed worker startup publishes no handle. Native adapters can schedule
at most 1,024 pending jobs; shutdown drains accepted work and joins both workers.
The owner must shut down off-worker and outlive all jobs.

This is a creation/lifetime foundation. Guest submission, dispatch, delayed work,
termination, process-queue selection and composite ports remain unsupported and
stop explicitly. The native scheduler only accepts host adapters. Before enabling
guest callbacks, establish per-worker guest TLS, module notifications, fault
handling and shutdown before module unmapping. Workers never borrow the caller's
guest context; Linux worker entry explicitly clears inherited GS. Serialized dispatch behavior is not claimed from creation alone.

The ordinary suite tests threadless queue state under the original strict
sandbox. A separate ambrosia_native_queue_tests entry exercises real workers,
concurrency, pending-job bounds, drain/join and all mode pairs under the reviewed
32-task thread profile. ambrosia_native_queue_denied_probe verifies transactional
failure under the original thread-denying sandbox. No interactive menu claim is
made from queue creation or synthetic scheduling checks.


## Application lifecycle subscriptions

The app-notify contract, minappmodelx, kernelx and xgameruntime exports share a
per-runtime registry for state and constrained-change subscriptions. Registration
stores the Windows byte-BOOLEAN callback and context, returns a nonreused opaque
ID and does not fabricate an initial event. Null callbacks/output fail87; the
shared128-registration budget reports8 on exhaustion. Unregister is kind-specific,
prevents future delivery and waits for an active callback on another thread. A
callback can unregister itself; the delivery snapshot retains its entry until it
returns. Entries removed before their turn are skipped. New registrations wait
until the next batch. Callback delivery is serialized with at most8 nested batches.

The explicit host entry Runtime::notify_app_lifecycle requires this runtime's
active guest scope/TLS; its execution owner must keep callback modules mapped.
Synthetic tests verify state/constrained values, contexts, cancellation during a
batch and cross-thread draining under the reviewed thread profile. This component
adapts OpenScarlett's MIT registry pattern; its license is retained separately.
Linux suspend/window-event wiring is pending. Registration success does not claim
host suspend/resume integration, game callbacks delivered, or an interactive menu.


## Offline networking provider

The owned XNetworkingImpl/base and v2 IDs share the bounded Windows interface
identity implementation. Slot15 and the flat connectivity query return the
16-byte GDK hint: provider initialized, connectivity None, unknown cost/interface,
and no data-limit/roaming flags. This is an explicit offline-only provider policy,
not detection of host interfaces or a claim of internet access. Slot3 and the flat
preferred UDP-port query clear the two-byte port and return network-unreachable;
no socket or port is acquired. HRESULT queries preserve error channels and accept
unaligned output buffers. Unsupported async, registration, security/certificate,
configuration and statistics slots stop explicitly before returning any value.
No notification is silently discarded and no certificate is accepted.

This advances class provisioning and synchronous offline queries only. Queue
completion/TLS support is still needed before change callbacks or async failures
can be delivered faithfully. Network denial remains part of the execution profile.
An interface/hint test alone proves neither menu startup nor online functionality.


## Explicit native ordinals and Winsock initialization

Named native services can declare a public ordinal alias. Static imports and
GetProcAddress resolve those aliases to the same canonical pointer as the name.
Facade PE export tables retain explicit sparse ordinal slots and forwarders;
holes stay zero. Private automatic image ordinals avoid explicit slots. Facade
ordinal lookups accept only declared public aliases, without guessing OS ordinals
from alphabetic export order. Registry entries/aliases share the 4,096-entry budget;
images retain their 512 KiB bound and at most 4,096 named exports. Zero public
ordinal aliases and multiple public ordinals for one name are unsupported.

The ws2_32 facade declares WSAStartup (115), WSACleanup (116), WSAGetLastError (111) and
WSASetLastError (112). These bindings were checked against installed Wine export
metadata, without loading/executing that DLL or using Wine to run the game.
WSAStartup initializes an owned offline compatibility-library session and fills
all 408 bytes of Windows x64 WSADATA, including zero vendor pointer/padding and
explicit offline description/status. Winsock 2 versions negotiate up to 2.2;
legacy 1.x requests are rejected. Initialization references cap at 4,096 and each
successful startup needs cleanup. Extra cleanup reports SOCKET_ERROR and
WSANOTINITIALISED. Winsock last error shares the active guest's Win32 error slot.

No host socket or transport is initialized. Socket I/O and other Winsock calls
remain explicit unsupported imports; successful library startup does not mean
network access exists. Synthetic tests include real PE ordinal calls and dynamic
lookup, not a claim of a working game menu or network connection.


## Guest worker module sessions

ModuleGraph::thread_snapshot captures the generation of a fully initialized
process session. Its run method requires a fresh host worker, clears inherited
GS, installs independent static TLS blocks from every loaded module template,
and grants that worker access to the sealed catalog. TLS/DLL thread-attach
notifications run in dependency order under a recursive loader lock. Entry runs
outside that lock; detach runs in reverse order, followed by thread-context exit
cleanup while mappings and catalog access remain valid. DLL notification policy
is atomic and disabling it shares the loader lock. Loading/activation remains
owner-only; read access is rejected outside the registered worker scope.

Snapshots retain mappings and reject closed/restarted generations. Process close
stops accepting new workers and refuses detach while any worker remains active;
the launcher must join its workers before closing the process session. At most64
worker scopes can be registered, independently of stricter OS task limits.
Thread creation during process initialization, guest exception unwinding and
Windows guard-page stack growth remain unsupported. The host launcher must bound
native stacks and retain its runtime service scope around the complete worker.

This section supplies guest thread initialization/lifetime; the native thread
handle layer below supplies CreateThread and its OS-thread implementation. Synthetic real-worker PE tests do
not prove an actual game worker or interactive menu has started.


## Native thread handles and launcher integration

CreateThread with null security attributes and flags zero or CREATE_SUSPENDED creates a real native
thread, publishes its native ID and returns an opaque waitable handle. The
worker has its own runtime/module scope, static TLS, module callouts and64KiB
alternate signal stack. It uses the native stack policy; requested commitment is
checked against actual usable bounds, with an8MiB per-request ceiling. Exact
Windows reserve/commit/guard growth and reservation flags are not implemented. No thread is faked when native creation or stack validation fails.

GetThreadId and GetExitCodeThread query owned thread objects. Completion becomes
signaled after guest entry, thread detach and TLS cleanup; the actual DWORD result
is retained, with STILL_ACTIVE while running. Closing a handle does not cancel
its worker. Completed jobs are joined/reaped, live jobs cap at64, waiters at64,
and handles share the4,096-object directory budget. Stricter OS task limits still
apply. The launcher and runtime owner join workers before process cleanup/detach;
shutdown rejects new creation. This requires cooperative worker completion on
normal executable return. Arbitrary SuspendThread of a running thread, ExitThread/termination and other thread APIs remain unsupported until implemented explicitly.

Per-worker diagnostic stacks reuse the process fault handlers/ranges and do not
replace shared signal dispositions. Faults still stop instead of resuming through
unimplemented guest SEH. Synthetic tests cover real PE worker entry/arguments,
TLS/errors/IDs/stacks, waits/results, close-without-cancel, repeated reaping and
actual CLI teardown; these alone do not prove a running game menu.


## Secure UTF-16 formatted buffers

__stdio_common_vswprintf_s decodes Windows argument slots and writes bounded
UTF-16 buffers. Integer/float conversions reuse the existing fmt numeric renderer,
LLP64 decoding and modern rounding policy. Wide literals, explicit wide strings
and wide characters retain UTF-16 code units, including embedded NULs; widths and
string precision count those units. Legacy wide option4 selects wide lower-case
s/c and narrow upper-case S/C; without it unqualified selection follows the
standard option mode. Explicit h/l overrides apply. Narrow-to-wide conversion
supports the invariant C-locale ASCII subset; other multibyte decoding stops.

Supported secure options are4 and32. A nonempty destination must include room
for the terminator; count excludes that NUL. A range failure clears the first
unit and stops for unimplemented secure invalid-parameter dispatch. Null strings,
%n, unsupported conversions/options, explicit locales and invalid parameters also
stop before guessed success. No truncating secure behavior, invalid-parameter
handler emulation or complete UCRT locale support is claimed. Limits retain128
argument slots,32,768 format/field/string units and1Mi output units. Synthetic
fixtures verify the measured GUID conversion shape and original PE six-argument
calls; actual game startup is recorded separately.


## CRT invalid-parameter handlers

Global setter/getter APIs exchange an atomic per-runtime callback. Thread-local
setter/getter APIs retain a separate pointer in each guest ThreadContext, with
no inheritance on a new guest worker. Dispatch prefers the current guest-local
handler, otherwise the runtime-global handler. The five-argument Windows callback
receives original UTF-16 metadata pointers, unsigned32 line and uintptr reserved
value under active guest TLS. No-info dispatch passes null/zero metadata. Native
errno is restored around callbacks; guest errors remain the CRT caller's policy.

A returning handler lets the CRT API report its failure. Invalid _set_new_mode
invokes it then returns-1/EINVAL without changing mode. Secure wide formatter
invalid/range/null-string/%n paths clear a valid nonempty destination's first unit,
set EINVAL/ERANGE, invoke the handler and return-1. Unsupported format/locale/
encoding features remain explicit capability stops rather than validation errors.
Default no-handler Watson/fast-fail termination and the noinfo-noreturn fallback
stop explicitly; guest SEH/Watson reporting is not emulated. Callback-providing
modules must remain mapped for the process lifetime. Other CRT APIs still need
individual validation-path integration; handler registration alone does not make
every secure CRT operation complete or prove the menu is running.


## Initially suspended guest threads

CREATE_SUSPENDED (flag4) starts a real native thread and publishes its ID/handle,
then blocks it at the existing startup gate before guest TLS, module callouts and
entry. ResumeThread on that owned handle reports the previous initial count and
releases it once. Further resumes report0 and do not call entry again. Invalid
or non-thread handles return0xffffffff/ERROR_INVALID_HANDLE. Stack requests use
the same checked native minimum and8MiB ceiling; no OS signal-based suspension
or Windows guard-growth emulation is used. Arbitrary SuspendThread is unsupported.

Normal shutdown refuses an unresumed thread with an explicit unmodeled-termination
stop rather than fabricating an exit code or hanging on that startup gate. Resume
all workers and let them finish before normal process teardown. Creation/close/
wait/result semantics otherwise stay shared with ordinary native threads. Tests
verify no early callback/entry, exact resume counts, one attach/detach and real
entry/result under the same bounded-thread profile; menu evidence is separate.


## Native priority adapter

SetThreadPriority validates a live owned thread handle and the normal-process
Windows levels -15,-2,-1,0,1,2,15. Under Linux SCHED_OTHER these map to nice
adjustments +19,+10,+5,0,-5,-10,-20 relative to that worker's inherited native
baseline, clamped to[-20,19]. This is a portability policy rather than an exact
Windows scheduler/CPU-share or real-time guarantee. No scheduler class is changed.
GetThreadPriority returns the last successfully applied guest level, initially0.
A host permission denial returns false/ERROR_ACCESS_DENIED and leaves that level
unchanged. Lowering nice may require privileges/limits that the isolation profile
does not grant; no elevated execution, silent success or priority bypass is used.

The narrow host adapter observes policy/nice, requests a change and verifies the
result. Only a live owned native ID reaches it, with completion held so the ID
cannot exit/recycle during the syscall. Other policies/hosts report unsupported.
Completed handles retain their last successful guest level but cannot change an
exited thread's scheduling; closed/non-thread handles fail. Worker creation checks
SCHED_OTHER baseline availability before publishing a handle. Ordinary thread/
stack/callback/teardown controls remain unchanged. Host nice changes on synthetic
workers and truthful failure paths are verified separately from game/menu evidence.


## Native thread CPU affinity

GetThreadGroupAffinity, SetThreadGroupAffinity and SetThreadAffinityMask reuse the
saved ProcessorInformation mapping: guest bitN corresponds to processor_ids[N].
The supported topology remains one dense group of1..64 allowed CPUs. Native IDs
may exceed63; they are translated through the table, never shifted/truncated into
a guest mask. Queries copy the real owned thread's native CPU set into the16-byte
Windows GROUP_AFFINITY layout. Group0 is supported; other groups, zero/outside
masks and nonzero reserved fields fail without a scheduling change.

Setters apply and verify the actual Linux CPU set and return the previous guest
mask only on success. Verification/rollback use fixed stack masks after mutation;
failed verification restores the old set, and unverified rollback stops. Guest
input and previous-output buffers may alias. The narrow host adapter accepts only
validated live owned IDs; the current-thread pseudo-handle additionally requires
active guest TLS. Completion stays locked through another worker's syscall to
prevent ID recycling. Completed/stale/non-thread targets fail before scheduling.

This adds real native affinity without changing isolation profiles, touching
unrelated host processes or pretending additional Windows processor groups exist.
Topology/affinity hotplug and masks outside the saved catalog are unsupported.
Synthetic tests query actual child CPU sets, restrict one mapped CPU, restore the
original set, verify reversed dense ordering, guard/error behavior and an unchanged
caller mask. Actual game progress is separate from CPU-affinity test success.


The service contract metadata catalog now allows128 modules including its reserved
canonical module. The original64-module cap was reached when affinity contract
aliases were added. Facades remain lazy; the16MiB aggregate executable-mapping
budget,512KiB image cap, registry entry budget and game module graph limits are
unchanged. A larger catalog is not permission to allocate more executable memory.


## Reopening native guest threads

OpenThread returns a new independently closable handle to a live owned worker
or an owner lifetime registered from the active guest owner context. Handles
retain the shared completion object, requested rights and inheritance marker.
Queries, waits, resume, scheduling and affinity enforce their respective rights;
zero/unknown/ended IDs cannot open unrelated or recycled native threads. Generic,
maximum and security access mappings remain explicit unsupported boundaries.
No child-process inheritance implementation is claimed.

The owner context installs one host completion observer after guest cleanup and
restoration of native TLS. The CLI records its actual entry result before worker
drain and process cleanup; handles stay STILL_ACTIVE and unsignaled until cleanup
finishes. An owner exit without a recorded result stops explicitly. A worker can
reopen that registered owner; first registration from another host thread is
unsupported. One Runtime models one owner guest lifetime. Worker ID lookup stays
available throughout shutdown joins so draining callbacks can reopen themselves.


GetCurrentProcess returns the exact full-width Windows guest pseudo-handle -1
across the process/thread API contracts. It owns no kernel handle or host file
descriptor. Each consuming API must validate this guest identity explicitly;
this facade does not enable arbitrary host-process access or missing process APIs.


SetProcessPriorityBoost and SetThreadPriorityBoost recognize supported current-
process/live-thread targets and report FALSE/ERROR_NOT_SUPPORTED. Windows dynamic
boost control has no native-host backend here; Linux nice control stays separate
and real. No boost flag or scheduler success is invented. Invalid targets and
missing thread SET rights report their own handle/access errors first. The thread
pseudo-handle requires an active guest context. Callers may handle the unsupported
capability through the API's normal error path. Process/thread contracts extend
through l1-1-2; other services are not implied.


GetSystemTimePreciseAsFileTime uses the native high-resolution UTC source and
existing checked100ns FILETIME conversion. Linux verifies CLOCK_REALTIME reports
at most1us resolution; Windows has a direct native adapter, currently untested.
Other hosts fail explicitly. Resolution is not a promise of absolute clock
accuracy or synchronization. Eight-byte outputs may be unaligned; null outputs
retain the diagnostic invalid-parameter behavior of the ordinary UTC service.
Sleep yields at0, performs real non-alertable native sleeps for finite milliseconds,
and waits indefinitely at INFINITE, including across spurious wakes. It preserves
caller errors and guest TLS. Infinite sleeps need process termination to end;
normal worker-joining shutdown cannot cancel them. Alertable SleepEx/APCs are
not implied. Existing sandbox wall watchdogs continue to bound diagnostic runs.


GetUserDefaultGeoName uses the isolated runtime's explicit neutral-world policy:
UTF16 "001" (UN M49 world), including its terminator. This describes no user's
country and reads no host location/profile/registry/network data. A zero-capacity
query returns4; short buffers and invalid parameters fail without partial writes.
Outputs may be unaligned and successful calls preserve error channels. User-
configured territories, language preference and the wider Windows geography
catalog are not implemented. This policy is distinct from the CRT's C locale.


## Native XUser identity and subscriptions

The base XUserImpl exposes a counted runtime-owned IUnknown identity, one-user
capacity and bounded change-event registrations. Each unique64-bit token retains
its actual guest callback/context and shared queue state after the queue handle
closes. Removing an inactive registration completes immediately, including when
wait=false; repeated removal is complete. No initial event or signed-in account
is fabricated. The provider currently has no user-state transitions or event
submission path. Adding users and dispatching notifications require guest async
queue support; those slots stop explicitly. A null/default process queue also
stops rather than claiming a registration without queue ownership. Invalid queues,
missing callback/output and the128-entry bound return typed failures. Only the
base interface is advertised; extension revisions remain unsupported.


## Native GameInput empty-device provider

GameInputCreate resolves by name and the public GameInput ordinal1. It returns
one counted interface per runtime. In the CPU-only profile there is no host input
backend and the actual device collection is empty. Timestamp queries return real
monotonic microseconds; reading queries clear output and report no reading/device.
No virtual controller, neutral reading, host device or window is invented.

Reading/device/guide/layout callback registrations retain actual kind, filters,
threshold, routine and context under128 bounded unique tokens. Enumerating the
empty collection generates no callbacks in any enumeration mode. Stop disables
the record; unregister completes immediately because none can be in flight.
Invalid callbacks/buffers/devices/modes and exhaustion fail before publication.
The default focus policy has no OS state in this profile; other policies stop.
Dispatcher/device creation, historical reading and OEM support remain explicit
boundaries. Native window/keyboard/controller integration and real event delivery
must be implemented before interactive-menu verification; this CPU-only provider
does not satisfy that goal. No additional device/display permission is consumed.


strtok_s implements the Windows three-argument byte tokenizer with caller-owned
continuation storage and no hidden per-process/thread state. Each call uses its
current delimiter set, skips delimiter runs and terminates tokens in place.
Exhaustion leaves a stable pointer to the terminating NUL; an empty delimiter set
keeps the remaining string. Packed continuation storage is copied safely. Null
delimiter/context or absent initial/continuation text sets guest EINVAL and invokes
the real invalid-parameter handler before returning NULL if the handler returns.
Input/continuation stay unchanged on that error; native errno is preserved. This
is the Windows API, not the four-argument bounds-checking Annex K variant.


## Native console storage factory

DStorageGetFactory in xcrdapi.dll uses the known console X2 factory identity and
slot order, which differs from the PC DirectStorage factory. It returns a counted
runtime-owned interface; unknown layouts fail. SetStagingBufferSize allocates,
replaces and releases actual owned memory transactionally, bounded to256MiB per
factory. Debug flags are retained as policy for later request execution. Methods
verify the interface belongs to the current runtime. I/O-worker affinity reports
unsupported rather than changing the caller or nonexistent worker threads.

Files, status arrays, queue variants and requests remain explicit stops until
their native ownership/read/execute paths exist. No queue or completed request is
invented. No new host integration is needed for this factory; inputs stay RO.
Staging allocation alone does not establish data loading or menu progress.


## Native UTF conversion

MultiByteToWideChar and WideCharToMultiByte use the installed ICU Unicode core
behind thin Windows ABI facades. GetACP reports the explicit native UTF8 process
policy (65001), and CP_ACP resolves to it; guest CRT C locale is separate. Other
code pages stop explicitly. The wrappers implement exact positive counts and
embedded NULs, count-minus-one including NUL, size queries, unaligned buffers,
strict invalid-character flags and replacement behavior. UTF8 default-character
arguments and unsupported flags return documented errors. Successful calls
preserve caller error channels; insufficient output publishes no partial result.

Input processing is bounded to16Mi units per call, with a guest allocation error
before oversized reads. Temporary aligned conversion storage prevents aliasing
ICU UChar with guest UTF16 and avoids ICU convenience terminators changing a
Windows explicit-count result. ICU74+ is a portable installed build dependency;
no runtime setup, host locale profile or online service is used. The retained
LICENSE.ICU describes that dependency; no ICU implementation is copied here.


## Confined read-only file metadata

GetFileAttributesA/W use dynamically configured package and executable roots.
DOS-style volume hints and ordinary relative paths address one read-only asset
namespace; native absolute origins must remain inside those configured roots.
Separators, dot components,UTF8/UTF16 and ASCII case fallback are handled in shared
C++ code. Exact Unicode spelling works; broader Windows Unicode case folding,
per-drive working directories,network/URI namespaces and reparse behavior are
not implemented. Ambiguous case matches,malformed names and root escapes fail.

The narrow Linux adapter pins directory descriptors and resolves every component
with openat2 BENEATH/NO_MAGICLINKS/NO_SYMLINKS/NO_XDEV. Symlinks and nested mounts
are denied rather than following targets through a check/open race. Directory
fallback scans are bounded4096 entries; ordinary exact lookups need no scan.
Other host adapters remain explicit extension points. Returned flags describe the
virtual read-only policy and real file/directory kind,not invented host archive/
hidden/system attributes. No file writes,networking or data-read capability is
introduced. File content and DirectStorage queue execution remain separate work.

Console storage OpenFile uses the shared confined read-only namespace and a real
owned native descriptor. The public console file identity and IUnknown are
accepted; arbitrary IDs are rejected. Guest references retain the descriptor;
Close/final release close it. A bounded runtime pin keeps object addresses from
being reused (32 open descriptors, 1024 lifetime objects). Descriptor metadata
survives pathname replacement. File-information ABI slots and storage request
queues remain explicit unsupported paths; this does not claim asset streaming.

CreateFileA/W now open existing read-only source handles, with ordinary or actual
Linux O_DIRECT access for FILE_FLAG_NO_BUFFERING. The same namespace tracks
read-sharing by native file identity, including hard links and DirectStorage
opens; metadata-only access does not restrict data sharing. One32-source budget
spans both APIs. Handles use the existing opaque kernel-object directory and
CloseHandle ownership; native owners can retain a source after handle close.
Writes, creation, asynchronous/security/template semantics and other flags fail
with explicit Windows errors. File data I/O remains a separate startup capability.

GetFileTime and GetFileSizeEx query the owned source descriptor. Portable time
values use signed Unix seconds plus nanoseconds, then checked1601-epoch/100ns
conversion. Linux statx supplies real access/write and available birth timestamps;
ctime is never substituted for creation. An unavailable requested birth timestamp
fails explicitly. Optional packed outputs are staged until validation; invalid/
closed/wrong-type handles and insufficient time-query rights cannot modify them.
File data I/O and direct-read alignment remain separate capabilities.

Synchronous ReadFile now copies real source bytes using native positional reads
and a serialized per-object cursor. EOF/short/zero reads report actual counts;
failed calls do not advance the cursor. Read operations retain their source and
make file waits reflect busy/idle state. Each call is bounded64MiB. Direct reads
require authoritative Linux statx alignment metadata and validate buffer, offset
and length before O_DIRECT I/O; unavailable alignment fails without fallback.
Synchronous OVERLAPPED offsets with a null event are supported, including
completed status/count/next-offset fields and file-cursor updates. Explicit-offset
EOF reports ERROR_HANDLE_EOF. Event-backed and asynchronous completion remain
unsupported and return an explicit error without publishing a completion. No streaming/rendering/menu claim follows
from these individual file services.

The process package context reads bounded MicrosoftGame.Config/AppxManifest.xml
descriptors through the same read-only namespace. Expat parses memory only; DTDs,
entities, excessive depth/elements/attributes and invalid identities fail closed.
Identifiers are validated without truncation and both descriptors must agree on
name, publisher and four-component version when present. Absent metadata means
unpackaged; a valid loose identity supports the owned XPackage identity methods.
Package installation, chunks, DLC, licensing and online state are separate services
and are not inferred from descriptor presence. The process identity is immutable.


### Owned placeholders

The sparse allowance now includes 1 GiB of headroom above the measured two
24 GiB physical windows for an additional no-access placeholder reservation.
This is an internal reservation budget: the reviewed native boot sandbox retains
64 GiB address space, 8 GiB data and its sampled RSS watchdog. It is not a hard
RAM cap or permission to exceed the external profile.

A placeholder owns one aligned Mapping. Reserve-only replacements add checked,
nonoverlapping child-window metadata without charging or owning the span twice.
Generic commit/decommit cannot change it, and populated-parent/partial release
remains explicitly unsupported. Raw policy is retained and pages remain
inaccessible until physical mapping is actually implemented. Owner metadata and
accounting are serialized; the Windows VirtualAlloc2 splitting implementation is
not used. See [native graphics host](../../docs/native-graphics-host.md) and task reports
for measured boot coverage.

Preserve-placeholder prefix/suffix splitting now records explicit aligned segments
under the same Mapping. Window replacements require an exact segment. Split
metadata is bounded and transactional; no additional address reservation or budget
charge is created. Coalescing, middle three-way splits, populated-window conversion
and partial release remain unsupported. Task162 startup completed five replacements
and reached XMemMapPhysicalPages. Task163 added leased physical-window aliases,
partial unmap/remap and checked rollback through the existing sealed backing.
Both compiler builds complete five actual mapping calls, then reach Xbox device
slot133 resource creation. CPU alias coherence does not prove GPU visibility or
a rendered frame; native GPU resource integration remains the next boundary.
