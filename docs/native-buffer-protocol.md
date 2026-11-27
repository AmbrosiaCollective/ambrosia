# Native buffer transport

The file transport extends the existing 64-byte AMBGFX01 device/fence protocol.
Only the fixed host service loads Vulkan. The isolated native runtime retains
read-only game inputs and has no direct GPU/display socket, networking, credential
or unrestricted process access.

## Endpoints and ordering

All four graphics endpoints are pinned single-link regular files in an immutable
private directory. Request and response remain 64 bytes. Upload and download
payload files start at exactly 1 MiB. The host reads upload and writes download;
the client has the opposite permissions. Both sides reject changed sizes. The
client's existing 32 MiB file limit bounds hostile upload growth before detection;
there is no separate hard 1 MiB inode limit.

One client owns the request lock. Its mutex covers payload access, request
publication, response validation and download consumption. Upload bytes precede
the request. Download bytes precede the response. Sequence numbers increase by
one; a repeated sequence is not executed again. A fresh session starts with zero
request/response files. Shutdown and service lifetime bounds retain ownership of
cleanup after a dead client.

## Buffer commands

Every integer is little endian. Header fields retain their original positions:
magic at 0, sequence at 8, operation at 12, owner at 16, payload byte count at 20,
and value at 24. Buffer copy adds destination ID at 32 and destination offset at
48. Other request fields must be zero. Existing fence sharing flags remain valid
only for the original fence-create operation.

| Operation | Owner | Value | Payload count | Destination fields |
| --- | --- | --- | --- | --- |
| 10 create | Device ID | Requested bytes | 0 | 0 |
| 11 destroy | Buffer ID | 0 | 0 | 0 |
| 12 upload | Buffer ID | Buffer offset | Bytes in upload file | 0 |
| 13 download | Buffer ID | Buffer offset | Bytes requested | 0 |
| 14 copy | Source buffer ID | Source offset | Bytes copied | ID and offset |

Creation returns a nonreused buffer ID in owner and its numeric GPU virtual
address in value. That value is not a CPU pointer or transferable driver handle.
Other buffer responses echo the requested owner, with zero value and API fields.
Status is zero only after the operation completes. Invalid ownership/ranges are
rejected; driver failures terminate the service instead of acknowledging success.

## Bounds and coherence

The host backend retains at most 32 live buffers and 128 lifetime IDs, 256 MiB
per allocation and 1 GiB total allocated bytes. IDs remain monotonic across device
recreation. Each byte transfer is at most 1 MiB and must fit the requested extent.
CPU transfers can be byte aligned. GPU copies require four-byte aligned offsets
and sizes and cannot overlap within one buffer. No caller-supplied CPU pointer,
shader, executable command stream or arbitrary Vulkan operation is accepted.

The initial copy backend waits synchronously for a bounded fence, with explicit
CPU/device-write and CPU/device-read memory dependencies. Destruction cannot
strand submitted work. Existing timeline ownership and device commands retain
their separate ID namespaces and behavior.

This transport is a foundation for physical-page coherence. A resource frontend
must retain the game's exact physical leases, upload the applicable bytes before
GPU execution and propagate GPU writes back before exposing completion. Aliased
resources must observe the same physical identity. Creating an unrelated buffer
and returning success would not satisfy that requirement. The transport alone
proves neither game resource behavior nor an interactive menu.
