# Native graphics host

The Linux guest runtime does not inherit host GPU nodes or display credentials.
A separate trusted service owns host integration. Game code and ordinary tests
stay in their reviewed sandbox under the default outer tool sandbox.

Current boot frontier: both GCC and Clang create and close 581 CPU command lists,
record 10 cache barriers, configure the frame timer and schedule two events, then
pass the first descriptor-increment query, split the placeholder and replace its
five windows and map their physical pages, then stop at Xbox device slot 133.
The menu and GPU submission
remain unverified. The sections below retain the staged investigation; later
sections describe the current implementations.

The first service, `scripts/query_native_graphics_host.py`, reports Vulkan API
version, device class, queue availability and selected Vulkan 1.2/1.3 features.
It accepts no payload or entrypoint, creates no logical device and submits no GPU
work. Names, vendor/device IDs, UUIDs and driver identifiers are omitted. This is
a hardware readiness check, not evidence of rendering or an interactive menu.

The current adapter targets the installed NVIDIA ICD explicitly. Other driver
selection belongs behind this host adapter; it must not leak into guest ABI code.
The existing portable Vulkan execution core remains the renderer foundation.
Windows COM/bootstrap/surface bindings still need separation for native Linux.

The service creates user/PID/IPC/UTS namespaces, drops capabilities, hides home,
credentials, game files and unrelated private data, clears the environment and
mounts sources/system libraries read-only. Only fixed accessible GPU character
nodes, a bounded immutable CPU/NUMA snapshot and a maximum 32-byte copy of the
NVIDIA module's `live` state are exposed. There is no live GPU sysfs mount.

All 43 isolation checks must pass before driver loading. Network, process/exec,
anonymous shared memory, memfd/SysV IPC and direct writable/executable mappings
remain denied. Only exact glibc pthread clone flags are admitted, under a hard
32-task limit. File-backed sharing within this private/read-only namespace is
allowed for the driver. No GPU descriptor or host window handle leaves it.

Limits are 64 GiB address space, 8 GiB data and sampled process-tree RSS, eight
CPUs, 8 MiB stack/file, zero locked memory, 64 descriptors, 40/45 CPU seconds and
30 seconds wall time. The RSS watchdog is not a hard RAM cap. `--self-check`
validates controls without loading a driver. Optional syscall tracing applies
only to this fixed trusted service; raw traces are private evidence.

Example from the repository root, using the separately reviewed host integration:

```sh
python3 -I scripts/query_native_graphics_host.py \
  --evidence reports/native-graphics --label capability-check --self-check
python3 -I scripts/query_native_graphics_host.py \
  --evidence reports/native-graphics --label hardware-capabilities
```

Labels must be fresh; evidence is never overwritten. Never use host-service
permissions to launch the guest runtime, tests or arbitrary downloaded code.
Original guest and compiler profiles remain unchanged.

## Owned logical-device bootstrap

`scripts/create_native_graphics_host.py` is a separate fixed service. It retains
all 43 controls and Task136 resource bounds, then creates one Vulkan 1.3 device,
enables the checked renderer feature subset, obtains one graphics/compute queue
and destroys the device before its instance. It submits no work and accepts no
guest payload or arbitrary Vulkan command. Host handles stay private.

The hardware bootstrap passes on native Linux. Thirty CPU-only ABI, feature
selection and failure/cleanup checks pass without loading a driver, and public
C declarations confirm the queue/device create-info layouts. A real device and
queue are prerequisites; they do not establish D3D12 coverage or menu rendering.
The guest-facing device still needs a typed transport and COM interface backed
by this real ownership. Preserve the metadata-only service and original guest
profiles. Device names and identifying hardware fields remain omitted.

## Owned device file service

`scripts/run_native_graphics_host.py` keeps the real device in its namespace.
Supply a fresh private `--root` under `reports`, plus `--evidence` and a fresh
`--label`. It creates fixed request/response files and an unused empty frame file
for the initial client profile. The host mounts request read-only and response
writable in an immutable directory. Eight additional checks pin file identity,
type, size and access directions; all 51 checks precede driver loading.

The 64-byte little-endian `AMBGFX01` protocol has only four operations: create,
destroy, query owned device and close service. It reuses the presentation packet
layout with all payload/geometry/reserved fields zero. Responses carry a numeric
ownership ID, status and public Vulkan API version. IDs are never reused during
the service lifetime. Unknown/released IDs and duplicate live creation are rejected.
At most one device is live, with 64 lifetime IDs, 1024 requests and a 20-second
service lifetime under the original 30-second parent watchdog. Driver failures
are not acknowledged as successful creation. Remaining devices close on exit.

The first isolated client reuses the unchanged 57-check presentation file profile;
its frame is unused. It verifies 11 real protocol operations, including stale and
foreign ownership rejection and recreation, without direct GPU access. This is
transport/device ownership coverage, not game COM coverage, GPU work or a menu.
Using graphics and presentation together still requires a reviewed combined
endpoint profile. Never send host pointers, CPU payloads or arbitrary commands.

## Native client seam

`host::Graphics` in `runtime/native/host_graphics.h` owns one explicit service
session. Its Linux file adapter is private to `host_graphics.cpp`; other hosts
can supply an adapter without changing guest ABI code. The public seam carries
only `GraphicsDevice` numeric ownership and API version. It opens no driver,
network socket or display and derives paths from the caller's endpoint root.

The adapter validates exact packet sizes, regular single-link endpoints, fresh
zeroed response state, exclusive client ownership, matching sequence/operation,
zero unused fields, ownership IDs and API version. Exchanges have a two-second
deadline and a 1024-request budget. Duplicate creation and foreign/released IDs
are rejected locally. Device release is acknowledged before local ownership is
cleared. Session destruction requests service shutdown, which closes any device
left live. If the service has died, destruction remains bounded and its host
lifetime watchdog owns cleanup. Never infer a game frame from device ownership.

## Combined guest profile

`scripts/run_menu_graphics_isolated.py` accepts explicit `--presentation-root`
and `--graphics-root` alongside the reviewed source, review and evidence paths.
The roots must be private and separate from one another and read-only inputs.
The guest sees only presentation's three files and graphics request RW/response
RO, each pinned by device/inode, exact bounded size and regular single-link type.
Directories and metadata stay read-only. The original presentation controls are
imported from a fixed read-only module without changing its restrictions.

All 65 checks precede entry. The response-writable omission is rejected before
client execution. GCC and Clang native clients pass their 31 window and seven
real device transport checks with both service mounts. The guest still sees no
GPU/display socket or credential. Source/game files stay read-only; network,
process/exec, WX, arbitrary shared mapping and resource controls are retained.
This launcher and every guest/test payload run only in the default outer tool
sandbox. Only the separately reviewed fixed host services use host integration.
The profile is ready for frontend/game probes; fixtures establish no game frame.

## Initial guest device frontend

Run modes accept explicit `--graphics-root DIR`. `GraphicsDevices` owns the manual
80-slot Windows ABI frontend independently of the host compiler's virtual layout.
Public IDs/order come from pinned DirectX-Headers. QueryInterface, AddRef, Release
and GetNodeCount use runtime-scoped, mutex-protected ownership. Unsupported slots
stop with their number. This identifies inherited interfaces; it does not claim
all their methods are implemented. Foreign-runtime objects are rejected before
access. Final release acknowledges real backend destruction before object removal.

The existing memory plan is frozen only after actual creation succeeds and retained
as a future resource/address-mapping constraint. Creating a logical device does
not reserve that plan's contiguous GPU VA range. Explicit adapters are unsupported.
Successful facade calls preserve thread error state. No resource/shader/submission
support or complete D3D12 conformance is claimed by this initial frontend.

Both native game builds now pass device creation with a real host owner. Their
next observed blocker is an unsupported 64 KiB XMem commit request, after which
the bounded diagnostic run times out. No game-rendered frame or menu is verified.

## Timeline fence service

The separate `scripts/run_native_fence_host.py` retains all 51 host controls and
resource bounds. It adds only real Vulkan timeline semaphore create/counter/host
signal/destroy calls. No queue command or shader is submitted. The earlier
metadata, device bootstrap and ownership services remain unchanged.

Packet operations 5..8 create, query, host-signal and destroy a fence. The value
occupies bytes24..31 as little-endian uint64; all other unused fields remain zero.
Creation names the owned device; subsequent calls name the owned fence. Fence IDs
are never reused across device recreation. Limits are 32 live and 128 lifetime
fences under the existing service/request lifetime. Fences die before their device.
Foreign/released IDs fail. Vulkan timelines cannot rewind, so decreasing signals
are rejected; equal signals are already satisfied. UINT64_MAX is reserved and
rejected. These explicit limits do not imply full D3D12 fence conformance.

The isolated client verifies real counters, high values, host signals, ownership,
recreation and cleanup with no direct GPU access. CPU ABI/protocol and public C
layout checks precede hardware use. Native guest CreateFence wiring is separate;
this service alone does not render or verify a game frame/menu.

## Native guest fence frontend

Device slot36 creates an owned ID3D12Fence with a real host timeline semaphore.
The 11-slot guest ABI implements identity/refcounts/GetDevice/completed/hostsignal.
A child retains its parent; final release destroys the semaphore before releasing
the parent reference. Counters come from Vulkan, including values above32bits.
Equal signals succeed, reserved values fail, and decreasing signals stop explicitly.
Shared flags, event completion and other unimplemented methods stop explicitly.
There is no GPU queue submission or complete D3D12 fence conformance claim.

Slot numbers must be checked against the pinned SDK independently of fixtures.
An initial mistaken slot26 assignment also existed in its fixture, so a synthetic
pass missed it and the actual diagnostic faulted. The mapping is corrected:
slot26 is GetCustomHeapProperties and slot36 is CreateFence. Six independent public
SDK slot checks now pass. Corrected GCC/Clang builds pass1516 strict and14 real
fence checks each; both game runs stop cleanly at the unimplemented heap-properties
call, with no fault. Fence support is upcoming coverage, not progress past slot26.

## Custom heap properties

Device slot26 returns the 20-byte custom equivalent of default/upload/readback
heaps. Packet operation9 queries the owned selected device's integrated/discrete
class through the existing Vulkan properties entrypoint. The mapping matches the
portable Vulkan renderer's UMA policy. Other classes/types/node masks are explicit
unsupported cases; metadata does not establish resource allocation or GPU coherence.

The adapter accepts the SDK C-style receiver-first call and compiler-generated
Microsoft aggregate-return call. Exactly one incoming pointer must match the owned
device before either pointer is accessed. The other pointer is the result buffer.
The first receiver-first-only prototype stopped on the game's layout before any
result write. An independently compiled return-by-value fixture exposed the hidden
buffer-first form; both actual builds confirm that layout for all three queries.

Canary fixtures verify exact result bounds, returned pointer, fields and error
preservation for both layouts. Both final native builds pass1516 strict and17 live
device/heap checks; six CPU class cases, existing26 timeline and24 protocol checks,
and seven independent SDK slot checks pass. Both games now stop cleanly at
CreateCommandQueue slot8. No game frame or menu is verified by these checks.

## Initial command queue ownership

Device slot8 creates a queue frontend only after verifying its real device owner,
whose Vulkan creation already obtained a graphics/compute queue. Direct/compute/
copy descriptors with normal priority, no flags and a single-node mask are supported.
Up to eight logical frontends retain independent parent references over that host
queue. Creation does not claim independent physical queues, submission ordering,
timestamps or command execution; unimplemented methods stop explicitly. Future
submission must reuse VulkanQueue/QueueHolds and true backend ordering.

The public 19-slot queue ABI currently implements identity/refcounts/GetDevice.
Descriptor bytes are copied once and retained. Output/IID/descriptor guards run
before access; foreign-runtime objects reject before dereference. The last child
lease releases the real backend. Both builds pass1516 strict and15 live queue
checks, including three kinds,parent retention and backend recreation. Thirteen
independent SDK/fixture index checks pass. Actual game direct-queue creation now
succeeds; the next stop is CreateFence's unsupported flags/initial-value contract.
No queue work or game frame/menu is verified by ownership checks.

## Shareable timeline creation

The fixed host service queries external timeline-semaphore properties for only
OPAQUE_FD. Export/import support and compatible handle type are required before
shared creation. It enables only `VK_KHR_external_semaphore_fd` when supported and
chains an actual `VkExportSemaphoreCreateInfo` into shared semaphore creation.
Earlier device services select no extensions by default. Their existing CPU
regression coverage passes. No descriptor is exported or transferred by creation.

Request flag1 on fence-create operation5 selects shared creation; other request
flags remain invalid. Existing host/guest controls and resource bounds remain.
The isolated client has exercised real exportable counters/signals/lifetime with
21 operations;23 CPU external-chain/capability checks and pinned C layouts pass.
Native shared-fence frontend validation follows separately. This establishes
exportable object creation, not Windows shared handles, cross-process imports,
GPU queue work or a game frame/menu.

Native SHARED flag1 now reaches actual exportable semaphore creation. Its creation
flags are retained; other flag bits remain unsupported. Both builds pass1516 strict
and17 native fence checks, including shared counters/signals/release. The actual
game creates a shared fence on both compilers and reaches device slot 48. The
initial PC method-name interpretation was incorrect: the inherited Xbox adapter
and [retail boot chain](retail_boot_chain.md#32-device-slot-48-creates-a-command-queue)
identify this as CreateCommandQueueX. Public PC order does not establish Xbox
semantics. No shared handle export/import, GPU queue work or game frame/menu is
claimed. Descriptor-transfer support remains separate work.

## Xbox command-queue override

The console frontend routes device slot 48 to the existing CreateCommandQueue
owner, matching the inherited Xbox adapter and retail boot-chain notes. This is
CreateCommandQueueX on Xbox; the public PC method at that index is unrelated.
Both compiler builds pass 20 real queue checks, including console argument guards
and independent parent references. Both actual games reach the compute-queue
request with DISABLE_GPU_TIMEOUT flag 1, which remains explicitly unsupported at
this checkpoint. No heap import, queue work or game frame/menu is established.

## Queue policy and descriptor readback

The PC queue path accepts only flag 0 or DISABLE_GPU_TIMEOUT bit 1 and retains
its descriptor unchanged. Queue slot 18 GetDesc returns exactly 16 bytes through
either explicit C-style output or compiled Microsoft aggregate-return ABI. This
policy metadata does not remove sandbox deadlines or change OS/driver reset
behavior. Queue execution remains explicitly unsupported.

Both compiler builds pass 22 live queue checks and 1516 strict checks; an unknown
PC flag 2 is rejected. Both actual games now create two queue frontends and one
shared fence, then reach another Xbox descriptor with third word 2. Its meaning
is not established by the public PC enum. A dedicated console descriptor mapping
needs investigation; do not silently mask those words or claim GPU scheduling.
No game frame or interactive menu is verified.

## Console queue descriptor translation

Xbox slot48 uses its own adapter. The [official FramePacing sample](https://github.com/microsoft/Xbox-GDK-Samples/blob/b1e77b6ac10f95b07296025fda171ca2ddc42dbe/Samples/Graphics/FramePacing/FramePacing.cpp#L1740)
names Type,Flags,EngineOrPipeIndex and QueueIndex. The four-word binary layout is
an HLE inference from the sample and actual calls, not a published SDK offset
contract. This corrects the earlier shared-PC-prefix assumption.

The adapter retains every field. Its bounded scope accepts types0/2/3,Flags0,
engines0..3 and queue indices0..1. Each selection owns a logical queue with normal
priority,flags0 and one-node PC properties over the existing real backend. PC
slot8 keeps its own known-bit validation. At most8 frontends are live. Independent
hardware engines,parallel scheduling and GPU execution are not claimed; future
submission must preserve logical ordering on the leased queue.

Both builds pass31 real queue and1516 strict checks. Four negative controls reject
unknown console flags,engine4,queue2 and unknown PC flags. Both actual games now
create five queues and five shared fences,then stop at device9CreateCommandAllocator.
No game-rendered frame or interactive menu is verified. Profiles remain unchanged.

## CPU command allocators

Device9 creates owned ID3D12CommandAllocator objects using the inherited renderer's
CPU recording model. Lists own command IR; an allocator tracks type and one active
recording owner. The native9-slot ABI implements identity/refcounts/GetDevice/Reset
and retains the real device until its last child lease is released. Empty resets
are supported; active-list integration and its busy-reset coverage remain next.
CPU types0..3 are supported under a64-object bound; video allocation is explicitly
unsupported. This does not create a Vulkan pool or claim GPU execution.

Both builds pass24 live allocator and1516 strict checks. Three safety controls
stop unknown methods,foreign runtime access and released objects before access.
Both actual games create the first allocator and reach Xbox73CreateCommandListX,
with no fault/timeout/watchdog failure. No game frame or menu is verified. Profiles
remain unchanged; no game bytes or raw diagnostics belong in publication artifacts.

## Xbox command-list contract

Xbox73CreateCommandListX shadows PC73CreateShaderCacheSession. A typed diagnostic
verifies the owned device/allocator and captures only a bounded prefix and identity
in private trace mode,then stops without returning a list. Both compilers verify
the six-argument ABI with synthetic inputs and capture the actual call: direct
list,idle matching allocator,null initial pipeline state,present output.

The requested identity matches a published console GraphicsCommandList6 alias
outside the public PC SDK. Prior art documents inline writes into the console
object,so a console-shaped wrapper must precede the existing portable command IR.
The next implementation must validate its buffer bounds and recording lifetime.
This is contract evidence,not a command list,GPU work or an interactive menu.
Raw descriptor/identity data remains private. Existing profiles are unchanged.

## Owned console CPU command lists

Xbox slot 73 now creates a console-shaped guest object around the existing
portable CommandList IR. Guest caches live in a separate owned mapping. Equal
write/end bounds force inline writers into virtual-method fallback; Close and
Reset reject changed bounds before altering the IR. Uninterpreted descriptor
words are retained. This is CPU recording support; GPU commands and submission
remain unsupported and stop explicitly.

Identity, references, GetDevice, GetType, Close and Reset are implemented. Each
open recording retains one matching allocator and marks it busy. Close or final
release drops that lease; Reset of a closed list acquires an idle allocator.
Lists independently retain the real device. Unknown table entries stop, including
the extended console range. Public identity/layout references are in REUSE.md.

Both GCC and Clang pass 1,523 strict checks, 29 real-device list lifecycle checks
and 24 allocator checks. Five negative controls stop unsupported commands,
foreign/released objects and changed bounds at the intended boundary. The
standalone offline CMake targets build and pass source/header/guard/ELF hash
verification; those standalone executables were not run outside isolation.

Both actual read-only game launches create their first direct CPU command list
and stop at console list slot 132, without fault, timeout or watchdog failure.
No GPU command, game-rendered frame or interactive menu is verified. Existing
guest/host profiles are unchanged; compiler source access adds only the new unit.

## Console slot 132 contract investigation

A bounded nonreturning diagnostic now captures slot 132's four scalar arguments
after checking list ownership, open recording, allocator lease and buffer bounds.
Both compiler fixtures verify the register and stack argument positions with
synthetic values. Actual GCC and Clang launches produce identical observations
and stop without acknowledging the operation. Raw arguments remain private.

The older FiveStar notes call slots 131/132 register writes. Archived TuxBox
revision 8661492d74886bc99ab7816745999bc02e2f393d instead treats its extended-list
slots as cache barriers: two 32-bit masks, then a 64-bit address and byte count,
lowered to a full global barrier. Static read-only decoding of the actual caller
confirms the argument widths and following slot 131/Close calls. These references
disagree on semantics; a plausible name alone does not establish an SDK contract.
The diagnostic's offset/value/control/mask labels follow the older notes and are
provisional. No register or cache operation is implemented by this diagnostic.

Next, review conservative whole-memory barrier recording through the canonical
IR, retain original masks/range metadata, and reject overflow or closed recordings.
GPU execution must remain explicit until a real submission backend exists.

## Conservative console cache-barrier recording

Extended console list slots 131 and 132 now append a canonical GlobalBarrier with
ALL synchronization and COMMON access before/after. This adapts TuxBox's public
Apache-2.0 whole-memory policy; LICENSE.TuxBox and REUSE.md retain its provenance.
Both original masks, address, byte count and canonical command index are retained
outside guest-writable storage. Range overflow, closed recordings, changed inline
bounds and more than 4,096 cache requests fail before an excess command is added.
Failed IR allocation rolls back its metadata; successful Reset clears both.

This broad CPU recording policy preserves ordering for future replay. It does
not implement individual hardware cache controls, narrow range synchronization,
GPU submission or prove console SDK conformance. Existing portable Vulkan replay
maps these IR scopes to ALL_COMMANDS and MEMORY_READ/WRITE, but that replay path
has not been executed by this native implementation.

Both compilers pass 1,531 strict checks and 30 live list checks. Targeted closed-list
and overflowing-range probes stop at the intended boundary. Both actual read-only
game launches create and close 581 lists, record 10 barriers and reach device slot
78 without fault, timeout or watchdog failure. Xbox slot 78 is the inherited HLE's
SetFrameIntervalX override, distinct from its public PC SDK slot. Raw game evidence
remains private. No guest/host/compiler capability or dependency changed.

## Native frame timer

Xbox device slots 78/79/80 implement interval setup, event scheduling and timer
waits. These shadow unrelated PC methods; slot 80 exceeds the public Device11
table. The published console signatures include reserved arguments, retained in
their correct positions. Nonnull display/reserved integration remains unsupported.

The portable FrameTiming component adapts FiveStar's timer-grid policy. It plans
origin polling, the next origin, token-in event offsets and finite timeouts using
explicit monotonic microsecond inputs. Checked arithmetic rejects wrapping
deadlines. Configuration supports periods 1..1,000,000 us, latency 1..16, four
event types and offsets up to 1,000,000 us. Repeating the same configuration
preserves its epoch. Unsupported flags fail explicitly; current-origin polling
uses flag 2. Timeout returns S_FALSE and preserves the input token.

Each wait captures a value snapshot and releases the graphics ownership mutex
before using existing host clock/sleep adapters. Sleep granularity can add about
a millisecond; precision, cancellation and a display-vblank source are not claimed.
These are host timer tokens. GPU completion, frames in flight, presented-token
consumption and render throttling still need actual submission/presentation work.

Both compilers pass 1,545 strict checks and 14 real-device timer ABI checks,
including host deadline arrival. Foreign/released-device controls stop before
access. Offline CMake and all source/header/guard/ELF gates pass. Both actual games
configure one interval and two events, then reach device slot 15 without fault,
timeout or watchdog failure. Actual game wait calls are not reached yet. No frame
or interactive menu is verified. Profiles and dependencies are unchanged.

## Guest descriptor record increments

Device slot 15 reports guest record strides independently of Vulkan descriptors:
32 bytes for views, 16 for samplers, and the inherited 32-byte RTV/DSV record policy.
Console kind 4 follows the extra-view alias published in XCL's
[real.rs](https://github.com/ZigZagBoi1/ScarlettPort/blob/48ba23a91a83121f8d0625a633e88fa463d5a0ca/runtime/xcl/crates/xcl/src/hle/d3d12x/real.rs),
with view/sampler widths from its console_heap.rs at the same revision. These are
API/layout facts; no GPL2 implementation is copied. Kind 4 shadows the public PC
NUM_TYPES marker; older FiveStar startup notes return zero for it. This adapter
accepts the published console alias for query metadata. Heap creation and descriptor
encoding remain unimplemented and must use a coherent record layout when added.
Unknown kinds stop explicitly. Ownership and native/guest error state are preserved.

Both compilers pass 1,545 strict checks and five live query checks. Foreign/released
device and unknown-kind controls stop at the intended boundaries. Both actual game
launches pass their first query, then a new unsupported XMem request returns error
50. No further trace progress occurs before the 10-second timeout kills the probe.
Observed RSS stays near 487 MB; no RSS/monitor failure or fault trace is observed.
Other descriptor kinds have synthetic ABI coverage, not actual-game coverage yet.
Investigate that memory profile next; extending the timeout alone does not resolve
the measured error. No GPU work, frame or interactive menu is verified.

## Owned XMem placeholders

The measured MEM_RESERVE | MEM_RESERVE_PLACEHOLDER request now owns one aligned,
rounded, no-access Mapping. Checked reserve-only replacements can attach bounded,
nonoverlapping child-window metadata without owning or charging the same span
twice. Children retain opaque attributes/protection and have no physical backing.
Ordinary commit/decommit and populated-parent/partial release remain explicitly
unsupported. A single owner mutex serializes reservations, metadata and accounting.
Small synthetic checks also cover two workers racing to replace the same range.

The internal sparse reservation allowance is 49 GiB: the existing two 24 GiB
windows otherwise leave no room for the additional 185 MiB placeholder. The
reviewed sandbox still enforces 64 GiB virtual space and 8 GiB data with its
sampled RSS watchdog. No hard RAM cap or new sandbox capability is claimed.

Both compilers pass 1,563 strict checks and the two-check replacement race. The
actual placeholder reservation succeeds, then VirtualFree rejects a 10 MiB
MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER split with error 50. Each 10-second startup
probe is killed at its deadline. A bounded private failure trace identifies the
flags without exposing pointers; static read-only inspection agrees with the
published split contract. Splitting is the next implementation step. Actual child
replacement, physical-page mapping, GPU work and the interactive menu are unverified.

## Explicit placeholder split boundaries

MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER now partitions an owned no-access reservation
into two aligned placeholder segments. Only bounded prefix/suffix splits of an
unconverted segment are supported; middle three-way, empty, whole, overlapping or
unaligned requests fail before mutation. A transactional metadata copy keeps the
single underlying Mapping and its reservation charge unchanged. At most 128
segments exist per parent. Replacement windows must exactly match a segment.
Coalescing, partial release and conversion back from populated windows remain
explicitly unsupported.

The five attribute-0x27 reserve-only replacement policies already documented in
xmem_exe_calls.md are accepted with their original protection words. Their GPU
permission bits grant no CPU execution; all windows remain inaccessible until
actual physical mapping is implemented. Both compilers pass 1,573 strict checks
and a three-check duplicate-replacement race. Both actual games now complete the
split sequence and five replacements, allocate 2,960 page tokens and stop at
xmem.dll!XMemMapPhysicalPages without timeout, fault or watchdog failure. Tokens
are owned allocations, not yet mapped game memory or GPU-visible backing.

## Leased physical-window aliases

XMemMapPhysicalPages now maps prevalidated owned page leases into aligned window
ranges through the existing SharedBacking adapter. Null arrays and zero tokens
unmap pages back to inaccessible anonymous reservation. Counts, destinations and
all nonzero token IDs validate before any mapping changes. Ordinary private
allocations and gaps cannot be used as physical windows. At most 16,384 mapped
pages are retained per owner, under the existing sealed 1 GiB backing profile.

Mapped leases outlive token release and are destroyed after their Mapping views.
Reused token IDs therefore cannot silently recycle an older alias's backing.
Remaps prepare metadata before host operations; failure restores the original
mapped prefix and failed-page view while retaining old/new leases. If rollback
itself fails, execution terminates instead of continuing with unknown aliases.
The inherited CPU-renderer policy projects CPU-noaccess GPU windows to RW staging;
raw console protection is retained, read-only windows stay read-only, and no
execute permission or GPU-coherence claim is added.

Both compilers pass 1,573 strict regressions and 20 physical alias/lifetime checks
under the existing physical sandbox. A denial-only rule forces the second shared
mapping to fail; five rollback checks per compiler prove original aliases remain
coherent and all backing is reclaimed afterward. No extra sandbox access is added.
Both actual games complete five physical-window mapping calls, then stop at device
slot 133 without fault, timeout or watchdog failure. Actual unmaps are not reached;
their evidence is synthetic. No GPU submission, game frame or interactive menu is
verified. Scatter mapping and further device extensions remain separate work.

## Actual placed-resource contract

Console device slot 133 is now a typed, nonreturning diagnostic with the published
seven-argument placed-resource ABI. It reads the fixed public 56-byte resource
description, compares the public ID3D12Resource identity, and classifies the start
address against owned window metadata. It does not create or return a resource.
Raw numeric fields remain in private diagnostic evidence.

Both compiler fixtures verify all scalar/descriptor fields and stack argument
positions with synthetic values. Both actual requests match: a row-major buffer,
unit height/depth/mips, unknown format, one sample, no clear value, resource IID
and initial layout 0xffffffff. The address starts in an owned window, but the
requested width exceeds that child window. Task 165 now proves complete mapped
coverage across adjacent windows in both actual builds. The next resource
implementation must make those retained bytes visible to a real GPU buffer. A
disconnected committed buffer would not satisfy this contract.

Both builds pass 1,573 strict regressions; actual diagnostics stop at the intended
integration boundary without fault, timeout or watchdog failure. No GPU resource,
game frame or interactive menu is verified by this diagnostic.

## Retained physical spans

MemoryServices can atomically snapshot every mapped 64 KiB page in a bounded
range, including ranges crossing adjacent child windows. Each entry retains its
physical lease and original attributes/protection. Null, empty, overflowing,
private, unmapped and out-of-owner ranges fail as a whole. A span is limited to
the existing 16,384-page physical pool, including its first-page prefix.

An optional read-only CPU view maps those same leases through the existing host
adapter. Original unmaps, remaps, token retirement, ID reuse and memory-owner
destruction do not invalidate retained bytes. View mappings are destroyed before
leases. No GPU resource or GPU visibility is implied by this CPU view.

Both compilers pass 15 span checks, 20 existing mapping checks, five forced
rollback checks and 1,573 strict regressions. CMake wiring builds both standalone
ELFs. Both actual buffer requests have complete mapped coverage across 2,784
pages, despite exceeding the first child window. Both still stop explicitly at
the GPU-backed resource boundary. No menu, GPU resource or frame is verified.
The compiler unit allowlist adds physical_span.cpp; sandbox capabilities and
resource limits are unchanged.
