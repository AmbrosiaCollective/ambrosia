# Native page backing

The host backing adapter owns an explicitly transferred anonymous-file descriptor.
Linux validates its size against the caller's budget, page alignment, read-write
access and seals that prevent size changes. Invalid transfers close the owned
descriptor. Unsupported hosts reject adoption before taking ownership.

Views replace only checked ranges inside an existing `host::Mapping` reservation.
They allow CPU read or read-write access. Shared mappings preserve contents across
aliases, unmapping and remapping. A retained kernel view stays valid after the
descriptor owner closes. Retired ranges can be discarded for zero-filled reuse;
the caller must first retire every page lease and alias for that range.

Linux syscalls stay inside the host adapter. The portable allocator owns 64 KiB
page leases and packed nonzero frame IDs below 2²⁵. IDs can be reused while a
retired alias retains different backing bytes. The last lease clears storage
before reuse. A discard error disables reuse rather than exposing stale data.
Backing bytes are not host physical frames, locked RAM or GPU addresses.

`XMemAllocatePhysicalPages` and `XMemFreePhysicalPages` use that allocator with
the measured attribute word `0x280`. They support packed count/array pointers,
bounded all-or-nothing allocation and valid-prefix freeing. Success preserves
native errno, guest errno and LastError; failures set the corresponding Win32
error. Positive requests without backing, and unmeasured attributes, stop
explicitly. Guest physical-page mapping imports remain unsupported until needed.

Native run modes accept `--physical-backing-fd N` to consume an explicitly owned
sealed descriptor after argument/path validation. Its immutable size bounds the
pool; the packed ID format imposes a separate addressability limit. The reviewed
launcher supplies descriptor 3. Inspection, duplicate options and descriptors
below 3 are rejected before transfer. No host descriptor discovery is performed.

The reviewed physical-pool sandbox prepares one sealed 1 GiB backing file on a
dedicated descriptor, then permits its initial shared nonexecuting mappings.
It retains read-only game inputs, privacy controls and existing resource limits.
Shared mappings bypass the data limit; backing ownership needs its own budget.
The RSS watchdog is sampled and provides no hard resident-memory guarantee.

Original fixtures cover alias coherence, remap persistence, range checks, zeroing
retired storage and descriptor cleanup. A private-mapping control must fail the
coherence and persistence checks. These are CPU backing tests, separate from
actual game startup and interactive menu verification.
