# Ambrosia roadmap

Primary target: **native x86-64 Linux, Vulkan, stable 30 fps**. No Wine or Proton in the
target runtime. The first game milestone is an interactive menu; world rendering
and stable gameplay with minimal stutter follow it. Frame-rate unlocking and
further optimization come later. No playable release or completion date is claimed.
Keep guest/runtime logic portable and OS calls behind narrow adapters so future
platforms can reuse the core. This does not imply their runtime builds work today.

## What exists

The repository includes inherited shader/format tools, a Windows-dependent PE
loader and compatibility shims, portable C++ tests and substantial Vulkan
execution code. Contributor CI runs on GitHub, including the synthetic
import-readiness checks. It does not verify a native Linux game runtime.

Ambrosia-specific local validation includes 78 curated contributor methods,
22 readiness methods, six launch contracts and eight fork-sync contracts.
These are synthetic/tooling checks, not retail boot or GPU evidence.

## 1. One native Linux bootstrap

Delivered locally: portable PE mapping/relocation/import binding, Windows/POSIX
memory adapters, dynamic host paths and an offline CMake target. The same 638 CPU
fixture checks pass through native shared objects built with GCC and Clang on
Linux. CLI logic is tested in-process; the standalone ELF is build-verified,
not launch-verified. Windows and
macOS adapters are unverified. See [bootstrap scope](runtime/native/README.md).

Create one Linux executable target and a small host boundary for memory, files,
threads, clocks and diagnostics. Reuse portable code before writing replacements.
Do not start another renderer, launcher GUI or full asset-export pipeline.

Acceptance: a reproducible Linux build loads an original synthetic x64 PE image,
resolves a known import through the Microsoft x64 calling convention and returns
the expected result. Malformed images and missing imports fail explicitly. No
game files, Wine, Proton, Windows SDK or host credentials are involved.

## 2. Correct guest execution and services

Delivered locally: aligned per-context static TLS, scoped Linux guest GS state,
TLS callbacks and dynamic DLL attach/detach. An in-memory module graph now resolves
names, ordinals and forwarded exports, assigns shared TLS slots and initializes
dependencies before their importers. Fixtures execute EXE-to-DLL calls and verify
reverse-order cleanup after failed attach. Worker-thread creation, cyclic imports,
dynamic module insertion and a complete guest TEB remain unfinished. A bounded
package provider now loads requested DLLs lazily from an explicit root. CLI
inspection maps dependencies without calling guest initialization or entry code;
bootstrap execution remains limited to original synthetic fixtures.
An explicit service registry now binds bounded VirtualAlloc/VirtualFree and
per-context last-error adapters. Unsupported flags and imports fail explicitly;
these four APIs do not constitute complete native startup support.

Implement and test guest TLS/thread state, exceptions/unwinding and the required
host-API semantics. Resolve names and ordinals, including dynamically requested
imports. A guest's Windows API imports remain a compatibility requirement even
though the host executable is Linux-native.

Add live storage queues, physical-memory destinations, bounded decompression,
offline user/save services, input and required audio/video paths. Scope each
change to an observed requirement and a synthetic regression.

Acceptance: synthetic DLL initialization, thread/TLS behavior, file reads,
queue completions/errors, input and save/restart cases pass in isolation.
An implemented symbol or a zero-return stub is not proof of correct behavior.

## 3. Native graphics to interactive menu

Connect the guest graphics ABI to the existing Vulkan execution components and
a Linux presentation path. Validate shader lookup/translation, texture layout,
resource visibility and descriptor lifetime on the target GPU. Keep required
passes; do not hide missing functionality by silently dropping work.

Acceptance: an isolated, read-only package run presents correctly rendered
first-run/profile/start screens, responds to input and preserves settings across
restart. Record exact source revision, hardware/driver, failures, missing shaders
and memory use. Demonstrate consistent 30 fps menu pacing and a bounded soak.
Synthetic presentation alone does not satisfy this gate.

## 4. Correct world, then stable 30 fps

Resolve mesh/indirect rendering, shader control-flow and wave64 semantics,
packed-format coherence, lighting/composition, audio and streaming failures.
Profile before optimizing; visible characters alone do not establish a correct
scene. Native Linux must meet the same correctness bar as other backends.

Acceptance: a visible controlled scene with working input/audio/saves, followed
by a defined-resolution 30-minute stability run. Track frame times against the
33.3 ms budget, memory growth, missing passes and driver/device resets. Hardware
and resolution must be stated; average fps alone does not establish stability.

The [native Linux plan](docs/native-linux-plan.md) explains the implementation
boundaries, research findings and immediate first task. Continue in focused local
commits. Publication is on hold until the owner explicitly requests it; do not
prompt for publication at task boundaries.
