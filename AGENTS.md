# Guide for coding agents

Read this before changing anything here. [CONTRIBUTING.md](CONTRIBUTING.md) has the rules for every contributor and
[docs/design.md](docs/design.md) the formats and the translator's runtime contract; this file adds what an agent
tends to get wrong.

## What this is

A port template for the Xbox Series build of GTA VI, built on the player's PC from their own copy:
`make_game.py` carves and translates the shaders (RDNA2 → SPIR-V), exports the assets and builds `runtime/`, a PE
loader that maps the game's executable and resolves its imports to shims. Nothing of the game is in the repository.

## Where things are

- The notes under `docs/` record measured formats, indexed by [docs/README.md](docs/README.md); `docs/design.md` is the one quoted most.
- `scripts/` holds the GPU/Wine test runners (`run_gpu_tests.py`, `run_wine_tests.py`), the translator suite runner
  `run_semantic_tests.py`, `check_no_game_data.py` (see the first hard rule below) and PowerShell build helpers.
- `viewer/` is the map viewer: `index.html` plus `serve.py`, a local-only server for the exported world.
- `third_party/` holds the `rpf9-explorer` submodule (`tools/rpf9dump` needs `git submodule update --init`), the vendored `xtaskqueue` the `xgameruntime` shim links and the vendored `bcpack-rs` rANS decoder of the BCPack calibration tools.
- `game/xbox-1.0.2.1.json` is the per-file size and SHA-256 manifest of the Xbox package — hashes only, no game content.
- `tests/semantic/` is the translator regression suite; a new translator rule comes with a small hand-written `.asm` fixture there.

## Hard rules

- **No game data, ever**: no archives, executables, carved files, shaders in any form (DXBC, FSSM, disassembly,
  SPIR-V), textures, audio, or logs and reports that quote them, in commits, issues, pull requests or test
  fixtures. A test that needs a shader builds its input by hand or reads it from the user's work folder.
  `python -I scripts/check_no_game_data.py <path> ...` reads the first bytes of the files you name, so it catches a
  renamed file the extension check in CI misses.
- **No keys, decryption or protection workarounds.** If a format turns out to be encrypted, write that down in
  `docs/design.md` and stop there.
- **Nothing online**: no Xbox Live or Rockstar services or domains.
- **Fix, don't disable**: a shader the translator rejects gets a rule, not a skip list. A new rule goes into the
  contract table in `docs/design.md` with how it was confirmed (a GPU test, a Mesa/LLVM reference, or a count over
  the corpus).

## Coordinate before you start

Follow `CONTRIBUTING.md`, `ROADMAP.md` and `docs/ci-policy.md`: one focused task
per new contributor, exact acceptance checks and maintainer review before merge.
The GitHub home is `AmbrosiaCollective/ambrosia`. Inspect its live access and
branch settings; these repository files alone do not enforce permissions.

- This is the independent Ambrosia fork. Check this repository's issues and
  [community](https://discord.gg/Bdguf59NvB) for existing work before proposing a fix.
- Keep scope, dependencies and exact tested revisions in the task/PR description.
  Existing upstream issue links are historical technical references.
- Work locally. Do not push, open PRs or change GitHub until the user explicitly
  instructs publication for the concrete change. Do not ask to publish at task
  boundaries. Keep commits small, reviewed and accompanied by local evidence.
- Continuing, finishing or committing work does not authorize publication.
  Historical task-boundary publication prompts remain superseded by the
  standing local-only instruction.
- Reviewed local integration is authorized. Keep remote-tracking/publication
  branches and historical source snapshots intact; use a separate local baseline.
- Use synthetic inputs without game files where possible. Inspect executable
  paths before running them; preserve isolation and do not install dependencies
  or weaken controls without approval.
- Standing permission for local native boot work includes reading and executing
  the supplied game files inside reviewed isolation with read-only inputs. This
  supersedes historical repository-only/no-game-file restrictions. Keep original
  bytes, disassembly, generated assets and
  private diagnostics outside Git. Game execution is authorized; unsupported
  startup requirements must still fail explicitly. GPU access and any weaker
  execution profile require their concrete isolation review before use.

## Before you guess, look

- The format or behaviour may already be measured in `docs/design.md`. Quote the file and offset a claim comes from,
  or call it a hypothesis.
- Count over the whole corpus, not a sample: a rule that holds for 400 shaders can fail on the other 26 000.

## Test without the game first

Ambrosia's small contributor entry point is `python -I ambrosia.py doctor`, then
`python -I ambrosia.py test --list` and `python -I ambrosia.py test`. See
[developer-workflow.md](docs/developer-workflow.md). The explicit selections are
in `tools/ambrosia_suites.json`; inspect test modules/imports before adding methods.
These commands run in-process and do not provide an OS sandbox. Keep applicable
isolation controls. Doctor results are discovery only; compiler/SDK versions and
builds are not verified. Failed or incomplete test runs exit nonzero.

None of these need game files. Most run in seconds - the translator suite takes a few minutes (433 files and 5 821
tests in 224 s on 16 cores) and the two builds longer - so run one translator test file while you iterate and the
whole suite once before the pull request:

```powershell
python -m compileall -q make_game.py tools tests
python -I make_game.py --help
python -I scripts/run_semantic_tests.py tests/semantic   # translator suite, one file per core; no LLVM, SPIR-V tools or GPU needed
python -m unittest discover -s tests/semantic -p test_<name>.py   # one translator test file
python -I tests/test_<name>.py                     # standalone standard-library unit test
node --test tests/test_viewer_streaming.cjs        # viewer streaming; needs Node 22+
dotnet build tools/rpf9dump/rpf9dump.csproj -c Release
cmake -S runtime -B runtime/build; cmake --build runtime/build --config RelWithDebInfo
ctest --test-dir runtime/build -C RelWithDebInfo --output-on-failure
```

`gta6rt` and the shims are Windows-only, so Linux and macOS configure the portable subset of the runtime tests. The
runtime tests that need the retail package read `AMBROSIA_GAME_DIR` (and `AMBROSIA_MOVIE`) and print `SKIP` when
unset; `loader_test` writes small PE images to `%TEMP%\ambrosia_loader_test`, which antivirus can block.

For pytest-based tests (the test files with `import pytest`, or no `__main__` block: they fail to import under
`python -I`), use a virtual environment and install the pinned dependencies in
[`tests/requirements.txt`](tests/requirements.txt). From the repository root, run `python -m pytest` with the
test file you want to check.

```powershell
python -m pip install -r tests/requirements.txt
python -m pytest tests/test_user_sgpr_census.py
```

With a work folder already built, a translator change is checked on a slice before the full run, and the result is
compared with the run before it:

```powershell
python -I tools/rdna2spirv.py <work>/shaders/dxbc/disasm --out <scratch> --limit 200
python -I tools/rdna2spirv.py <work>/shaders/dxbc/disasm --out <scratch> --shard 0/8   # one of 8 parallel slices
```

Shard runs write `index.part*.json`; merge them into `index.json` with `--merge`. Report translated and
`spirv-val`-valid counts before and after; a change that lowers either is a regression.

## Conventions

- Keep code human-readable, clean and non-compact. Use descriptive names,
  explicit control flow and small focused functions. Let ordinary behavior speak
  for itself; document ABI, ownership, ordering and other non-obvious constraints.
  Avoid compressed one-line logic, redundant comments and unrelated formatting
  churn. Keep commits focused and manageable; related implementation and tests may share
  a slightly larger commit to reduce overhead. Native runtime formatting is defined in
  `runtime/native/.clang-format`; follow it when editing those files.
- Python tools run with `python -I` and use only the standard library unless a file says otherwise; the exceptions
  are the texture and model tools that import numpy or Pillow: `bc_feature.py`, `amd_swizzle.py`, `tile_probe.py`,
  `swizzle_search.py`, `tex_export.py`, `tex_mode_probe.py`, `calibrated_texture.py`, `world_grid_atlas.py`,
  `tex_headers.py` and `skinned_glb.py` (which fall back to the standard library when numpy is absent).
- A text file a tool writes names its line ending - `open(path, "w", encoding="utf-8", newline="\n")` - so the same
  input gives the same bytes on every platform; `tests/test_text_write_line_endings.py` catches a writer that forgets.
- Native Linux is the current runtime priority. Keep portable algorithms and
  guest ABI code separate from host memory, threading, files and presentation.
  Prefer standard C++/Python facilities and the existing Vulkan execution core.
  Keep OS calls behind narrow host adapters; preserve Windows/macOS extension
  points without claiming those backends are tested. Resolve paths dynamically
  with `std::filesystem`/`pathlib` or `os.path`, never developer drive/home paths.
- In the pull request, say what you tested, the exact revision, platform and any GPU/driver used. Omit optional personal and authorship labels; do not make false authorship claims.
