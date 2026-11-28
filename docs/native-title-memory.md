# Title memory for native descriptor heaps

The title exports `__XMemAllocationHooks__`, a two-word allocator/free table.
Published console implementations use that allocator for descriptor storage with
attributes `0x00800004`. The game can install an allocator that returns a range
it already chose, then compare the heap's CPU handle with that range. An
unrelated host allocation would not preserve this contract.

## Validated callback access

The loader records each validated PE section's raw extent and intended access.
The module graph can copy at most 64 bytes from a readable, nonexecuting loaded
data section. Headers, gaps, padding, code sections, wrapping extents and reads
crossing a section boundary are rejected. Inspection-only mappings never become
callable code. Hook targets must lie in executable sections of active images.

The adapter resolves the named data export from the sealed loaded module graph,
rejects distinct ambiguous tables, reads the pair afresh for each allocation and
checks both callbacks before invoking either. Calls require the current runtime,
module graph and an active guest context. The original module execution
generation is paired with the allocation. A closed or restarted generation, or
a different runtime scope, cannot consume its release ownership.

Allocation is synchronous in the existing guest context. A graphics ownership
mutex must not be held across the call because the title allocator may reenter
native services. Release uses the paired callback explicitly in the original
active context. Destruction never invokes guest code implicitly after detach.
Process teardown remains responsible for allocations whose context has closed.

## Current integration

Device slot 14 now returns an owned heap over the title-selected storage. The
complete extent must be tracked writable memory with the console descriptor
alignment before either handle is published. Reserved, read-only, unmapped,
incomplete and wrapping ranges are rejected without changing permissions.

The frontend preserves the original address for console CPU handles and for
shader-visible GPU record identities. Creation and explicit release run the
paired title hooks outside the graphics lock, while retaining the parent and
reserving live, pending and retiring quotas. See
[native-descriptor-heaps.md](native-descriptor-heaps.md) for interfaces, ABI and
bounds. Descriptor encoding, GPU binding, game rendering and the interactive
menu remain separate implementation and verification gates.
