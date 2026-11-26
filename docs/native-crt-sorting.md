# Native CRT sorting

`qsort` resolves through `ucrtbase.dll` and the CRT utility contract. The entry
and comparison function both use Microsoft x64 calling conventions. A host libc
callback has a different ABI on Linux.

The portable implementation sorts fixed-width records in place with iterative
heapsort and standard byte-range swaps. It needs no allocation or recursion.
Comparisons receive pointers to actual array elements; equal elements can change
order. Only the sign of the comparison result matters. Records retain all their
bytes regardless of alignment or width.

Comparisons run synchronously on the caller's existing guest thread and module
scope. They retain its TLS and can call other implemented guest APIs. The adapter
creates no worker, module attach/detach or fresh guest TLS. A nontrivial sort
requires active guest context and a comparison pointer in a mapped module.
This membership check covers image containment; callers still supply a valid
function pointer. Guest exception propagation remains unsupported.

Arrays requiring rearrangement are bounded to 1,048,576 records and 64 MiB.
Overflowing address ranges stop before element access. Zero or one record needs
no comparison or rearrangement; zero count permits a null base. A zero width,
null comparator or null base with nonzero count dispatches invalid-parameter
handling with guest EINVAL. Host errno is preserved; changes a guest comparator
makes to guest error state remain visible.

Original PE callback fixtures verify the Windows ABI, caller TLS and scope,
record/payload preservation, arbitrary-width guards and parameter behavior.
Dedicated fail-stop fixtures check resource/address and missing-scope guards.
Game startup is measured separately; passing these fixtures does not prove a
rendered frame or interactive menu.
