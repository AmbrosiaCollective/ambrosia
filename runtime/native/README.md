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
as errors. Each build's native shared-object entry passed 578 checks under a
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

The catalog caps each image at 512 KiB/4,096 exports and the aggregate at 64
images/16 MiB. Facades appear lazily for registered modules; arbitrary requested
names cannot create a library. Native address-to-image queries include the
canonical stubs. The eight package modules reported by the startup trace remain
a separate count from these generated service views.

GetProcAddress returns named implemented facade exports or real loaded PE name/
ordinal exports. Read-only forwarder lookup reaches loaded images or registered
services without invoking the package provider or changing dependencies. Missing
exports return procedure-not-found, invalid handles fail separately and native
service public ordinal requests stop explicitly. This does not supply dynamic
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
allows 48 GiB of no-access window reservations while ordinary allocations retain
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

Diagnostic --boot-memory selects a 2 GiB private allowance alongside 48 GiB of
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
independent 48 GiB no-access window option.

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
command-line retrieval and reaches strlen. No window or menu is verified.
