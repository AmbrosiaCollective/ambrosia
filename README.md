<h1 align="center">
  <img src="assets/wordmark.svg" alt="Ambrosia" width="560">
</h1>

<p align="center"><strong>Preservation, open research, and a native PC runtime for GTA VI.</strong></p>

<p align="center">
  <a href="CHANGELOG.md"><img alt="Version: unreleased" src="https://img.shields.io/badge/version-unreleased-8b5cf6?style=flat-square"></a>
  <a href="ROADMAP.md"><img alt="Status: experimental" src="https://img.shields.io/badge/status-experimental-f97372?style=flat-square"></a>
  <a href="LICENSE"><img alt="License: GPL v3 or later" src="https://img.shields.io/badge/license-GPL--3.0%2B-0f766e?style=flat-square"></a>
</p>
<p align="center">
  <a href="runtime/native/README.md"><img alt="CPU bootstrap: Linux tested" src="https://img.shields.io/badge/CPU_bootstrap-Linux_tested-0f766e?style=flat-square"></a>
  <a href="https://github.com/AmbrosiaCollective/ambrosia/actions/workflows/contributor.yml"><img alt="Contributor CI" src="https://github.com/AmbrosiaCollective/ambrosia/actions/workflows/contributor.yml/badge.svg?branch=main"></a>
  <a href="docs/ci-policy.md"><img alt="CD: not configured" src="https://img.shields.io/badge/CD-not_configured-64748b?style=flat-square"></a>
  <a href="#project-status"><img alt="Tests: 106 local checks passed" src="https://img.shields.io/badge/tests-106_local_checks_passed-0f766e?style=flat-square"></a>
</p>
<p align="center">
  <a href="tools/"><img alt="Python 3.12 or newer" src="https://img.shields.io/badge/Python-3.12%2B-3776ab?style=flat-square&amp;logo=python&amp;logoColor=white"></a>
  <a href="runtime/"><img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599c?style=flat-square&amp;logo=cplusplus&amp;logoColor=white"></a>
  <a href="runtime/xma_decoder/"><img alt="Rust decoder" src="https://img.shields.io/badge/Rust-decoder-ce6f44?style=flat-square&amp;logo=rust&amp;logoColor=white"></a>
  <a href="https://discord.gg/Bdguf59NvB"><img alt="Join Discord" src="https://img.shields.io/badge/Discord-join_us-5865f2?style=flat-square&amp;logo=discord&amp;logoColor=white"></a>
</p>

<p align="center">
  <a href="#project-status">Status</a> ·
  <a href="docs/README.md">Docs</a> ·
  <a href="CONTRIBUTING.md">Contribute</a> ·
  <a href="CHANGELOG.md">Changes</a> ·
  <a href="docs/i18n/README.md">Other languages</a>
</p>

Ambrosia is an independent open-source project working toward running the Xbox
Series version of GTA VI natively on **x86-64 Linux with Vulkan**. The goal is
interactive menus followed by stable 30 fps gameplay. Wine and Proton are outside
the target runtime. **We are not there yet.**

The CPU code is already x86-64. The runtime loads it and supplies the console
services it expects, while the shader tools translate RDNA2 GPU programs to
SPIR-V for Vulkan. Game files and generated outputs stay on the player's machine.

> [!IMPORTANT]
> Ambrosia exists to preserve knowledge about this game and the technology behind
> it. We document what we learn and build tools that people can study, test, and
> improve. That work belongs in the open, where it can benefit more than one
> project or generation of hardware.
>
> **What we share here is code, documentation, and synthetic tests.** GTA VI's
> executables, original assets, decryption keys, and ready-to-play game packages
> are neither included nor offered. Ambrosia provides no package-decryption
> facility or directions for sourcing game files.
>
> Experiments that depend on the game begin with your own locally readable files.
> Keep those files, exported content, game-derived shaders and caches, and logs
> that reproduce game content outside the repository. They must also stay out of
> pull requests, issue attachments, and community uploads. Share reproducible
> synthetic examples, counts, and redacted error summaries instead.
>
> Preservation, scientific inquiry, and better technology are reasons to support
> this effort. They are also why we stand against attempts to intimidate its
> contributors or suppress the research.

## Project status

**Experimental. No playable Ambrosia release or verified retail boot is claimed.**

| Area | What we can actually say |
|---|---|
| Local checks | 106 synthetic test methods passed on Linux with Python 3.12.3: 78 contributor checks, 22 import-readiness checks, and six launch-policy checks. |
| Runtime | A separate native CPU bootstrap maps and executes tiny synthetic PEs through a shared core and host-memory adapters. The inherited full loader/shims still depend on Windows; native retail services and graphics integration remain unfinished. |
| Shaders and formats | Translation, archive, model, texture, and audio tools are inherited from earlier work. Their recorded measurements are not new Ambrosia validation. |
| Native builds | The CPU bootstrap builds with GCC, Clang and offline CMake on Linux; 472 native checks pass under each compiler, including package discovery, CLI modes and bounded memory services. CLI logic is tested in-process; the standalone executable was built, not launched. No verified menu result exists. |
| CI and delivery | Contributor CI runs on GitHub for Windows and Linux. Extended builds are opt-in. No continuous deployment or release pipeline is configured. |
| Game and GPU | No Ambrosia menu, in-game rendering, performance, or stability result has been verified. |

The CI badge follows the published Contributor workflow. The test-count badge
records local validation; native builds and gameplay remain unverified.
See the [roadmap](ROADMAP.md), [CI policy](docs/ci-policy.md), and
[inherited technical notes](docs/README.md) for the detail behind it.

## 🧪 Contribute without game files

You can work on loaders, imports, streaming queues, format handling, diagnostics,
and synthetic tests without owning or downloading the game. Start with Python
3.12 or newer, from the repository root:

```sh
python -I ambrosia.py doctor
python -I ambrosia.py test --list
python -I ambrosia.py test
python -I tests/test_boot_readiness.py
python -I tests/test_ambrosia_launch.py
```

The doctor discovers prerequisites without running external tools or installing
anything. The contributor command runs 78 selected tests; the next two commands
cover the other 28. These are focused checks, not the full suite or a native
build. The commands do not provide an OS sandbox.

Use `test hashdict`, `test paths`, `test manifests`, `test console`, or
`test developer` to narrow the contributor selection. Doctor and contributor
commands support redacted `--json` reports. See the
[developer workflow](docs/developer-workflow.md) for examples.

## 🧩 How it works

| Part | Job | Read more |
|---|---|---|
| Package checks | Identify supported files using names, sizes, and hashes. | [Build entry point](make_game.py) |
| Shader tools | Unpack and translate native RDNA2 programs to SPIR-V. | [Translator contract](docs/design.md) |
| Runtime | Map PE images, resolve imports, supply offline services, and bridge console graphics to Vulkan. | [Runtime guide](runtime/README.md) |
| Format tools | Inspect and export supported archive, model, texture, and audio formats locally. | [Technical index](docs/README.md) |
| Diagnostics | Record startup events and assess initial import resolution without sharing raw traces. | [Import readiness](docs/import-readiness.md) |

A resolved import is not proof that a service behaves correctly. A translated
shader is not proof that a frame renders correctly. Progress means reproducing
results and fixing the next failure, not just increasing counts.

## 🛠️ Build and platform notes

Start with the game-free contributor commands above. The next runtime milestone
is the native host/guest boundary described in the
[CPU bootstrap guide](runtime/native/README.md). Its shared core already runs an
original synthetic PE without Wine or Proton. It is not a game build.

We are keeping one target host, one Vulkan backend, and one eventual build/run
entry point. Game data stays read-only; builds, caches and saves stay outside the
source tree. Full asset export is not part of the menu milestone.

The [native Linux plan](docs/native-linux-plan.md) lists the execution, service
and graphics work required before a real menu is possible. Existing Windows
build scripts and notes remain as inherited technical references, not the
supported direction. Shader contracts and hardware limits are in
[design notes](docs/design.md) and [testing](docs/testing.md).

### Running the loader on Linux

There is no supported native game-launch command yet. A Linux build of the
portable tests does not build the Windows-dependent game loader. See the
[milestones and acceptance checks](docs/native-linux-plan.md) before attempting
game execution. Existing Wine examples in older notes are historical references.

## 🤝 What needs work

The next steps are the native Linux host/loader boundary, a reproducible boot
baseline, live DirectStorage queues, console audio integration, CPU compatibility,
and shader/memory correctness.
Stable gameplay also needs testing by people using their own copies on real
hardware. Synthetic checks alone cannot establish that.

Keep contributions small: one problem, clear acceptance checks, and enough
information to reproduce the result. Check for existing work before starting,
then follow [CONTRIBUTING.md](CONTRIBUTING.md). Join the
[Discord](https://discord.gg/Bdguf59NvB) to coordinate.

## 📝 A note from the author

> I consider takedowns, meddling, or attempts to suppress this work aggression
> toward the open-source community. They reflect extremely poorly on the parties
> behind them. Ambrosia exists for preservation, the pursuit of knowledge,
> science, and making technology better for humanity. There are no original game
> files or game assets in this repository. Open research deserves room to exist.

## 🙏 Credits

Ambrosia is independently maintained. It builds on earlier Leonida work and the
people who created it. Their contributions, original notices, and technical
citations remain part of the project. See [UPSTREAM.md](UPSTREAM.md) for provenance.

- [rpf9-explorer](https://github.com/breadbbbb/rpf9-explorer) by breadbbbb: archive, model, texture, and audio decoding used by `tools/rpf9dump`.
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (MIT): descriptor layouts and shader behavior references.
- [libHttpClient](https://github.com/microsoft/libHttpClient) (MIT), by Microsoft: the vendored XTaskQueue/XAsync implementation.
- [LLVM](https://llvm.org/): AMDGPU disassembly.
- [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools): assembly and validation.
- Historical artwork reference: [SlyDevil's GTA VI icon](https://www.steamgriddb.com/icon/139218).

Grand Theft Auto and related names belong to their respective trademark owners.
Ambrosia is not affiliated with or endorsed by them.

## ⚖️ License

[GPLv3 or later](LICENSE). Third-party components keep their own licenses and notices.
