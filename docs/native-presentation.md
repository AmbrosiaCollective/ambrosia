# Native presentation boundary

The native host adapter can use an explicitly supplied directory of three fixed
file endpoints to reach a separate SDL presentation broker. It opens no display
socket and receives no host display credentials. A single client owns the request
channel, with serialized requests and a two-second response deadline.

The broker creates only its own bounded software windows. Its versioned protocol
supports create, destroy, title updates, visibility, event polling and copied RGBA frames. It
accepts no executable code, guest callbacks, game files, arbitrary paths or X11
packets. Window IDs are service-owned integers, never host pointers. Titles,
dimensions, frames, event counts and live windows have explicit limits.

The reviewed launcher gives the guest writable request/frame files and a read-only
response file inside an immutable endpoint directory. The guest retains network
and execution denial, hidden home/credentials/GPU, read-only inputs and existing
memory/thread bounds. Its separate presentation profile allows 32 MiB files for
the measured 3840×2160 request. Older profiles keep their original limits.

Only the trusted broker connects to the host display. It restricts new sockets,
processes and execution after SDL starts, and runs for a bounded lifetime. The
initial file protocol is a portable host seam; a later direct backend can replace
the transport while preserving guest window/message ownership.

The host adapter and live broker are tested separately from game startup. Their
original fixtures exercise software windows, copied synthetic pixels, visibility,
events, 4K geometry and stale IDs. They do not establish actual game pixels or
menu interaction.

`CreateWindowExA` now uses an explicitly bound presentation root. It owns a guest
handle and real hidden host window before delivering `WM_NCCREATE`,
`WM_NCCALCSIZE` and `WM_CREATE` on the caller's existing guest thread/context.
The class retains its mapped callback catalog. Rejected creation rolls back
ownership; visibility follows successful creation. Parent/menu/extended styles
and unmeasured creation styles remain unsupported. Callback-computed client
rectangles are retained without inventing native border metrics.

Default `WM_NCCREATE` processing accepts only the owned active creation structure
on the owner thread. It initializes the actual host title, requires host title
readback, and then accepts creation. Unsupported default messages stop explicitly.
The temporary structure lease is cleared when creation returns or rolls back.

No visible game window, game frame or interactive menu has been verified.
