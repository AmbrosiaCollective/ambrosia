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

Linux syscalls stay inside the host adapter. Guest page IDs, allocation budgets,
window validation and lease lifetimes belong above it. Backing bytes are not host
physical frame numbers, locked RAM, GPU addresses or proof of device support.
The guest allocation and mapping imports are still pending integration.

The reviewed physical-pool sandbox prepares one sealed 1 GiB backing file on a
dedicated descriptor, then permits its initial shared nonexecuting mappings.
It retains read-only game inputs, privacy controls and existing resource limits.
Shared mappings bypass the data limit; backing ownership needs its own budget.
The RSS watchdog is sampled and provides no hard resident-memory guarantee.

Original fixtures cover alias coherence, remap persistence, range checks, zeroing
retired storage and descriptor cleanup. A private-mapping control must fail the
coherence and persistence checks. These are CPU backing tests, separate from
actual game startup and interactive menu verification.
