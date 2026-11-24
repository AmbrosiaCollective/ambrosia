# Ambrosia developer workflow

Start contributing with Python 3.12 or newer and this repository. These commands
use the standard library and tiny synthetic fixtures. They need no game files,
GPU, dependency installation, initialized submodules or internet connection.

```sh
python -I ambrosia.py doctor
python -I ambrosia.py test --list
python -I ambrosia.py test
```

Run from the repository root. On Windows, `py -3.12 -I ambrosia.py ...` also works
when the Python launcher and that version are installed.

## Offline doctor

The doctor checks the running Python version, required contributor source paths
and the syntax of the optional `AMBROSIA_GITHUB_REPO` setting. Additional profiles
look for tools on `PATH`:

| Command | Additional discovery |
| --- | --- |
| `doctor` | No external tools required for the contributor tests |
| `doctor --profile translator` | `llvm-mc`, `spirv-as`, `spirv-val` |
| `doctor --profile runtime` | CMake, Cargo and a platform-appropriate C++ compiler |

It never runs an executable it finds, installs packages, accesses a game folder
or queries a service. Missing requirements include a next step. A `discovered`
result means the selected checks found their prerequisites; it does **not**
certify tool versions, LLVM AMDGPU support, Vulkan SDK availability, compiler
compatibility, successful builds or GPU/game behavior. Tool overrides such as
`LLVM_MC` are not resolved by these `PATH` checks. Consult [testing.md](testing.md)
for the full platform setup and supported versions.

`--json` writes a report to stdout for inspection or sharing:

```sh
python -I ambrosia.py doctor --profile translator --json
```

The report contains relative repository paths and check results, not absolute
host paths, discovered executable locations, usernames, account names, environment
values or credentials. Redirect it to a file outside the checkout if needed. The
command itself does not create files.

## Small contributor suites

`core` combines the explicit selections below. A suite name selects one component:

| Suite | Selected methods | Coverage |
| --- | ---: | --- |
| `hashdict` | 15 | Hashing, harvesting and actual `main()` repeated-input regressions |
| `paths` | 19 | Report aliases, hard/symbolic links and volume-name behavior |
| `manifests` | 12 | Hand-built JSON manifests, invalid basenames and UTF-8/BOM handling |
| `console` | 9 | Eligibility, keyboard parsing, temporary log panes and capture settings |
| `developer` | 23 | Doctor/catalog behavior, runner failure controls and output redaction |
| `core` | 78 | All of the above |

```sh
python -I ambrosia.py test hashdict
python -I ambrosia.py test paths --json
python -I ambrosia.py test developer
python -I ambrosia.py test --list --json
```

The expected outcome is `PASSED`, with the full selected method count and zero
failures, errors or skips. For example, a complete `hashdict` run reports 15/15.
Some systems disallow symbolic or hard links; that coverage is then reported as
`incomplete`, with the curated test ID, rather than silently treated as a full
pass. Expected failures also count as incomplete. Unexpected successes are failed
runs. Import errors, missing source/methods and zero/short runs cannot pass.

| Exit code | Meaning |
| --- | --- |
| `0` | Doctor discovery complete; listing successful; or every selected test passed |
| `1` | Missing doctor prerequisite; or failed/incomplete test run |
| `2` | Invalid command, suite or catalog |

Only the exact methods in [ambrosia_suites.json](../tools/ambrosia_suites.json)
execute. Listing reads this catalog without importing tests. There is no broad
discovery, `load_tests` hook, subprocess runner or package download. Existing test
modules are imported normally, so inspect their imports and setup as well as
methods when changing the catalog. Review any new selection for game files,
services, child processes, thread creation, installs and host side effects.

Tests run **in-process** and use scratch files. The command is **not an OS
sandbox** and does not replace your execution/isolation policy. It does not grant
permissions or weaken existing controls. It restores its added Python import
paths, but is intended to run as a standalone CLI rather than in an application
that requires all imported modules and global state to be preserved.

Reports omit captured stdout/stderr, exception messages, tracebacks, skip reasons
and subtest values, which can contain local paths. They retain counts, issue
kinds and the predefined parent test IDs. For detailed local diagnosis, run the
named original test class/method directly under your existing isolation policy;
inspect output before sharing it.

These suites are a quick contribution check, **not the entire upstream suite or
a release gate**. A passing result establishes only the selected synthetic
behavior. Runtime/translator changes still need their component-specific checks.

## Contribution handoff

Publication is on hold until the owner explicitly instructs otherwise. Keep
development, reviews and integration local; do not prompt to publish after tasks.
Record scope and overlap checks locally. When publication resumes, coordinate a
small task through the repository or [community](https://discord.gg/Bdguf59NvB).
Include the exact revision, command, selected suite, outcome and platform in a
review. State additional coverage and limitations separately. Keep one focused
change per PR and distinguish inherited capabilities from newly verified results.
