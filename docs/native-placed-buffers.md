# Native retained placed buffers

Console device slot 133 creates supported row-major buffers over complete mapped
physical spans. Creation retains every physical lease and initializes a real
coherent Vulkan buffer with those bytes before returning a resource. It does not
fall back to an unrelated committed allocation when the range is unmapped.

The initial supported shape is a single-mip, single-sample buffer with unknown
format, unit height/depth, row-major layout and zero or 64 KiB alignment. Width
must fit the backend's 256 MiB individual buffer bound. The published console
unordered-access and console-only hint bits are retained in the original
resource description. The console-only hint is not passed as a Vulkan flag or
claimed to have a newly established hardware meaning. Initial layout is retained;
this path supports only common or the console's initial-layout sentinel. Clear
values, textures and other shapes remain explicit unsupported boundaries.

## Ownership and addresses

The resource retains its actual parent device, immutable physical snapshot and
CPU alias. Its GPU allocation closes before the physical aliases and leases.
Resources use the existing 32-live-buffer backend bound. QueryInterface shares
one object identity for the supported resource/base interfaces; unknown methods
remain guarded failures. GetDesc supports both the explicit C result pointer and
the Windows aggregate-return calling convention and preserves the original
56-byte descriptor. GetGPUVirtualAddress returns the console virtual identity.
Backend GPU addresses remain a separate command-lowering concern.

Map returns the original console CPU address while it still maps the exact
retained pages. After original mappings disappear or change physical identity,
Map uses the resource's retained alias. A read-only original policy cannot gain
a writable CPU map through this API. Unmap validates its advisory written range
and uploads that range; it does not retire or replace original CPU mappings.

## Physical byte synchronization

The explicit PhysicalBuffer coordinator uses canonical physical bytes between
synchronous operations. It refreshes source and destination bytes before a GPU
copy, waits for actual completion, then downloads the written range into those
same retained physical pages. Other CPU/resource aliases therefore observe the
write, and their next GPU operation refreshes from the updated physical bytes.
Original unmaps, token retirement and virtual-address reuse cannot silently
retarget a retained resource to different pages.

The coordinator serializes through its caller's graphics queue/state lock. Its
initial copy operation is bounded to one 1 MiB transfer chunk. General asynchronous
submission must retain owners and preserve the same pre-submission refresh and
completion/writeback ordering. CPU writers remain responsible for normal API
synchronization with GPU execution. General command-list replay, shaders,
textures, a game frame and an interactive menu are separate verification gates.
