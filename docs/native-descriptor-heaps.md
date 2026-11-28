# Native title-backed descriptor heaps

Device slot 14 creates an owned frontend heap over the exact storage returned by
the title allocator. Every byte must lie in tracked writable memory. Reserved,
read-only, unmapped, overflowing and incomplete ranges are rejected before a
handle is published. No unrelated allocation fallback is used.

The heap preserves its original 16-byte description and console descriptor
stride. CPU handles are scalar addresses of title records. Shader-visible GPU
handles use the same console identity; CPU-only heaps return a zero GPU handle.
These are frontend record identities, not Vulkan descriptor handles. Descriptor
encoding and GPU binding remain separate implementation boundaries.

The object supports its public heap and base interface identities, reference
counting, parent lookup, both result conventions for GetDesc and the console's
scalar CPU/GPU start methods. Other slots fail explicitly. Capability checks
validate the description and return S_FALSE without allocating storage.

Creation reserves a parent reference and pending quota before unlocking for the
reentrant title allocator. The full returned range is validated before insertion.
Live descriptor ranges cannot overlap. Release removes the public object before
calling the paired free hook outside the graphics lock. Parent and quota remain
reserved until the callback finishes. This prevents reentry from stranding a
parent or bypassing limits. Process teardown never calls guest code after detach.

Bounds are 32 pending/live/retiring heaps, 64 MiB per heap and 128 MiB total
reserved storage. The byte budget includes allocations and frees inside callbacks.
Actual memory permissions remain those of the title's tracked allocation; heap
creation does not grant new write or execute permissions.

Future asynchronous binding/submission must retain heap ownership until GPU use
completes. The current object and CPU storage do not establish descriptor replay,
a rendered frame or an interactive game menu.
