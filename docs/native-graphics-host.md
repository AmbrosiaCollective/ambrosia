# Native graphics host

The Linux guest runtime does not inherit host GPU nodes or display credentials.
A separate trusted service owns host integration. Game code and ordinary tests
stay in their reviewed sandbox under the default outer tool sandbox.

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
