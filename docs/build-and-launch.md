# Build and launch commands

Ambrosia uses one command implementation with a terminal entry point for each
system. Install Python 3.12 or newer first. A command without an action opens the
interactive menu, with color on supported terminals and timestamped status lines.

| System | Menu | Build | Launch availability |
| --- | --- | --- | --- |
| Linux | `./ambrosia.sh` | `./ambrosia.sh build` | `./ambrosia.sh launch` |
| Windows | `.\ambrosia.cmd` | `.\ambrosia.cmd build` | `.\ambrosia.cmd launch` |
| macOS | `./ambrosia.command` | `./ambrosia.command build` | `./ambrosia.command launch` |

The build command builds the **native CPU bootstrap**, not a complete game.
Game launch currently explains the missing runtime milestone and exits nonzero.
It does not run the executable, prepare assets, start Proton or contact services.
There is no verified menu result. Windows and macOS native bootstrap builds are
unvalidated; the current bootstrap requires x86-64, including on macOS. Apple
Silicon execution is unsupported.

The shared commands also work directly, from any current directory:

```sh
python -I /path/to/Ambrosia/ambrosia.py menu
python -I /path/to/Ambrosia/ambrosia.py build --dry-run
python -I /path/to/Ambrosia/ambrosia.py build --build-dir /path/to/build --jobs 2
python -I /path/to/Ambrosia/ambrosia.py launch
```

Builds require CMake 3.24 or newer and a C++20 compiler. Use an x64 developer
terminal on Windows. Requirement discovery does not execute tools or certify
their versions. The commands install nothing and fetch no dependencies.

The default build directory is `Ambrosia-build` beside the checkout. An existing
nonempty destination must contain a CMake cache for this native source directory,
or its owned marker from a failed configuration so that a retry can proceed.
Outputs cannot go inside the checkout or into an ancestor of it. Configuration
and compilation output streams to the terminal and a separate log for each build.
Logs are local diagnostics and may contain host paths; inspect before sharing.
A failed configuration stops compilation; build failures return nonzero.

Use `--plain` or set `NO_COLOR` to disable colors. Redirected output is plain;
the interactive menu requires a terminal. Automation should use named commands.
Ctrl+C returns 130. These tools do not provide an OS sandbox; keep the applicable
execution controls when building or testing untrusted source.

See the [developer workflow](developer-workflow.md) for offline doctor/tests and
the [native bootstrap](../runtime/native/README.md) for implemented runtime scope.
