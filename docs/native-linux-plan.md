# Native Linux path to the menu

Decision: x86-64 Linux is the target host. The runtime must be a native Linux
executable with Vulkan, without Wine or Proton. Target stable 30 fps with minimal
stutter after correct boot/menu and world rendering. A CPU bootstrap exists;
there is no available native game build.

## Local bootstrap progress

The [native CPU bootstrap](../runtime/native/README.md) shares PE parsing,
relocation/import binding and path policy across hosts, with OS memory operations
behind one adapter. Linux builds pass 435 synthetic checks under GCC and Clang.
The CLI engine is tested in-process with synthetic package files; the standalone
executable builds through offline CMake but has not been launched. This does
not implement the retail entry contract, exceptions or graphics. Single-module
static TLS and dynamic DLL lifecycle now work in fixtures, including an in-memory
module graph, forwarded exports and independent shared TLS slots. Worker threads,
cyclic imports and a complete guest TEB remain unfinished. A bounded provider now
discovers requested DLLs in an explicit package root without scanning asset contents.
Windows/macOS guest thread adapters fail explicitly until implemented.
The first explicit native service slice supplies bounded reserve/commit/decommit/
release and per-context last error. Fixed-address reservations, protection changes
and many other guest services remain unsupported. Unknown imports stay fatal. Inspection now maps the supplied executable and seven
bundled DLLs read-only/NX, retaining unwind data and reporting unresolved imports
and unsupported metadata. There are 861 unresolved import entries in the measured
graph; that count does not establish unique APIs or their execution order.
Inspection mappings cannot initialize or execute. Actual imports guide priorities.
An explicit fail-stop diagnostic mode now executes supplied PE initialization
until the first unsupported call/fault. Native clocks, recursive critical sections
and FLS, real TIB self/stack bounds and CRT callback-table initialization now
advance module initialization to malloc. The earlier direct TEB read
no longer faults. It neither
returns fake service results nor implements full exception handling. The main
game entry, graphics and menu have not been reached.

## What the comparison establishes

Several projects have reported 30 fps menus using a Windows runtime or Proton.
The lit-world VIctory fork reports characters, textures and direct sunlight but
still black shadowed areas and only a few frames per second. Those are useful
implementation references, not proof that native Linux is already solved.

- [Lit-world fork at the reviewed revision](https://github.com/fxceless/victory-gta6/blob/7b0f736d0b29c2c512a9506e38df92f1f8d50986/README.md): culling/view fixes, descriptor lifetime, packed-lighting texture visibility and more shader semantics. The author reports missing ray-traced indirect light/shadows. Its status is self-reported; source-document dates are not independently authenticated.
- [TuxBox's title setup](https://github.com/brcly/TuxBox/blob/3b1197301d6c68872520bdb8cfa13cd16986eb00/docs/PROJECTSCARLETT.md): a relevant Linux menu result, explicitly using Proton, so not an execution route under our constraint.
- [VIlkan's native backend](https://github.com/SebSilent/vilkan/tree/45a954a4706e1c69e9322c07a852472e28bb1259/crates/vilkan-vk): native compute, raster and presentation prototype code exists. Its title loader is still Windows-gated and its reported game boot uses Proton. Native graphics tests are not native retail-menu proof.
- Asset viewers can show models without executing the title. Treat their images as format/rendering research rather than game-boot milestones.

These projects are moving quickly. Use pinned revisions and tested workload
details, not screenshots, project names or shader-conversion percentages alone.
Do not copy an existing author's unmerged implementation as a competing patch.
Any source reuse needs a separate license, attribution and code review; no source
was incorporated by preparing this plan.

## What is missing here

The inherited executable target is Windows-only. Its loader depends on Windows
virtual memory, DLL loading, TLS and unwind/exception facilities. The Vulkan
execution core is already separated from many COM-facing wrappers, which is
useful material for the port. A Linux CMake build now produces a separate CPU
bootstrap, not a complete native title loader.

Native Linux still needs to supply the guest's Windows-shaped ABI. Map PE images
in a native process, bridge the Microsoft x64 calling convention, maintain guest
TLS/thread state, and provide precise import/exception behavior. Host libc and
pthreads are implementation tools, not drop-in replacements for those semantics.
Bundled PE DLLs need the same loader/service boundary; Linux cannot load them
through its normal ELF dynamic loader.

The directly inspected title has 587 static imports across 68 DLLs, including
24 ordinal imports. Seven console-only modules account for 37 imports, but that
does not mean only 37 functions need consideration: the Windows host currently
supplies many other services, and bundled libraries add dependencies. Static
counts neither establish complete coverage nor identify all dynamic calls.

Live DirectStorage queues, console audio services, console texture-layout
semantics, shader completeness and CPU/GPU visibility remain separate gaps.
The current `DStorageGetFactory` return-policy classification is not a factory
implementation. Some texture-layout functions explicitly return `E_NOTIMPL`.

## Keep the work small

1. Add a native Linux target and the minimum host memory/import boundary.
2. Load and execute a tiny original PE fixture; assert its return value, import
   ABI, bounds checks and failure behavior. No game package or GPU yet.
3. Add TLS, threads, DLL initialization and exception regressions before any
   retail entry point is attempted.
4. Add required service slices with synthetic fixtures: file mappings, storage
   ordering/completion, offline profiles/settings, input, audio/video adapters.
5. Reuse Vulkan execution code and prove Linux presentation with original
   shaders before connecting the title's graphics ABI.
6. Run staged package checks, then initialization, then the menu under an
   explicitly reviewed native/GPU sandbox. Record each first blocker.

The first deliverable is the fixture-running Linux executable, not a renamed
Windows executable or a claim that the real game boots. Do not turn off missing
imports, skip asserts, or substitute dropped mandatory draws to claim a pass.

## One layout and one entry point

Keep the core portable: standard filesystem and C++ facilities first, narrow
adapters for OS-specific semantics, and the existing Vulkan execution model for
graphics. Vulkan surface creation will remain a host adapter. Do not invent a
second graphics API or claim that a thin wrapper makes guest services trivial.

Use one eventual build/run entry point through the existing developer command
surface, rather than another launcher GUI. The integrated native game command
does not exist yet; the standalone CPU bootstrap is separate. Keep the package
and separate executable read-only. Store build output,
shader caches, saves and logs under one work root outside the repository, with
distinct subdirectories. No game assets or generated shader data belong in Git.

Do not export all assets for menu work. Stream the original package as needed.
Translate/cache the required shaders with explicit versioning; choose offline
preparation versus first-use translation from measured coverage and stalls.
Avoid introducing both a new Rust runtime and a new C++ runtime: evaluate reuse
at the host/ABI boundary and retain one supported architecture.

On the inspected machine, CMake, GCC and versioned Clang 18 are available.
Native GPU devices are present on the host but hidden in the default execution
sandbox. Shader preparation still needs its applicable compiler/tool version;
no new toolchain has been installed. Recheck exact dependencies at implementation.

The package drive has less than the existing 30 GiB shader-work budget free.
Put work on the separate drive with more space; no package copy is necessary.
Budgets are estimates, not a completed native build's measured footprint.

## Isolation and validation gates

Keep credentials, home directories and session services hidden, the package
read-only and network blocked. Builds require a reviewed process-capable compiler sandbox, distinct
from the existing Python launcher that denies child execution. GPU runs need a
separate, explicitly reviewed device/display profile. Do not silently weaken
the CPU-only sandbox or execute the title through elevated tool permissions.
GPU access shares the host driver and can still cause a driver reset.

Use CPU fixtures first, then a synthetic Vulkan draw/readback and presentation,
then game initialization. Native title execution, dependencies, host/device
integration and large generated outputs need their concrete task approval.
Task 025 research ran no native build or game. Task 026's CPU-only compiler
profile was explicitly approved, and its native shared-object checks still use
the original strict Python launcher. Game execution and GPU initialization have
not occurred. Work stays local without publication prompts until explicitly
instructed otherwise.
