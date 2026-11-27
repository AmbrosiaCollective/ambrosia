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
