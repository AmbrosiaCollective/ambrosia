# Contributing

Thanks for helping. A few rules keep the project safe to host:

- **No game data, ever.** That covers archives, the executable, carved files, shaders (DXBC, FSSM, disassembly,
  SPIR-V), textures, models, audio, videos, and reports or logs that quote them. `.gitignore` blocks the common
  types; check `git status` before you commit anyway.
- **No keys, decryption or protection workarounds**, and no links to where to get them. The tools only read files
  that are already plain on disk.
- Short excerpts needed to explain a format (a few header bytes, one instruction sequence) are fine in docs and
  comments.

## Before you start

Coordinate Ambrosia work through this repository's issues and the
[Ambrosia community](https://discord.gg/xEaVQtW5jH). Check for an existing claim or
implementation before starting, and describe the scope and any dependencies in
your pull request. Keep changes focused and say whether they need game files or
specific hardware. Report exact revisions, commands and limitations.

Historical upstream issue and PR links in the technical notes are references to
the original measurements; they are not Ambrosia's task queue.

## Small tasks and review

Start with one claimed task and a written acceptance check. New collaborators
should complete one focused PR before taking additional work. Coordinate changes
to shared modules and consolidate duplicate findings rather than opening another
implementation. Use [the roadmap](ROADMAP.md) to choose useful work.

Open a branch and a PR, describe the resulting behavior and list exact tested
revisions/commands. At least one maintainer reviews code and evidence before a
merge. Address or resolve review discussions; a new change after approval needs
another look. Do not push directly to `main`, force-push shared branches or merge
your own unreviewed change. Keep dependencies and unrelated refactors separate.

Contributor CI is the initial check gate. Extended native/full-suite jobs are
opt-in and their absence is unrun coverage. See [CI policy](docs/ci-policy.md).
CODEOWNERS and this written policy require matching GitHub access and branch
rules to be enforced; their presence in the repository alone is not enforcement.

Follow the [conduct policy](CODE_OF_CONDUCT.md). Send vulnerability details through
the [private reporting route](SECURITY.md), not a public issue or Discord channel.

## Working on the code

Start with `python -I ambrosia.py doctor` and `python -I ambrosia.py test` for an
offline, game-free contributor check. Use `test --list` to inspect exact selections
before executing them. [Developer workflow](docs/developer-workflow.md) explains
component suites, redacted JSON reports, exit codes and skipped coverage. These
commands do not replace component-specific tests or your execution isolation.

- Python tools run with `python -I` and use only the standard library unless a file says otherwise.
- A text file a tool writes (JSON, CSV, a manifest, an assembly listing) names its line ending: `open(path, "w",
  encoding="utf-8", newline="\n")` (the same for `Path.open`, `Path.write_text` and a text-mode
  `tempfile.NamedTemporaryFile`), so the output is LF on Windows too and the same input gives the same bytes on every
  platform. `tests/test_text_write_line_endings.py` fails for a writer that forgets it.
- A report writer refuses an output that names one of its inputs or another output, and asks
  `tools/report_paths.same_report_path(a, b)` rather than deriving the answer again: it resolves both paths,
  applies the volume's case rules and falls back to `os.path.samefile`, so a new tool reuses one answer
  instead of a private copy. `tests/test_report_paths.py` covers it.
- `tools/rpf9dump` needs the submodule: `git submodule update --init`.
- A new shader-translator rule goes into the contract table in `docs/design.md`, with how it was confirmed: a GPU
  test, a Mesa/LLVM reference, or a count over the corpus.
- `python -I tests/test_<name>.py` runs one of the unit tests (standard library only; CI runs them all). The test
  files with no `__main__` block or an `import pytest` (they need the packages in `tests/requirements.txt`) run with
  `python -m pytest tests/test_<name>.py` instead, as [AGENTS.md](AGENTS.md) describes; under `python -I` they fail
  to import or run nothing.
- Runtime tests: `cmake -S runtime -B runtime/build`, `cmake --build runtime/build --config RelWithDebInfo`, then
  `ctest --test-dir runtime/build -C RelWithDebInfo --output-on-failure`. The CPU tests that need no game data are configured
  on Windows; Linux and macOS configure the portable subset (`gta6rt` and the shims are Windows-only). Tests that need the retail package read `AMBROSIA_GAME_DIR` (and `AMBROSIA_MOVIE` for the Bink
  decode test) in place and print `SKIP` when the variable is unset; never copy or commit those files.
- Viewer streaming tests: `node --test tests/test_viewer_streaming.cjs` (Node.js 22+, CI tests 24 LTS; no packages,
  browser, network or game data). These execute the viewer streaming code with a synthetic loader and clock.
- `loader_test` writes small PE images to a `ambrosia_loader_test` folder in `%TEMP%`. Antivirus that blocks newly
  written executables makes it stop with that reason; exclude the folder or set `AMBROSIA_TEST_DIR` to one it skips.
- Tool checks: `python -m compileall -q make_game.py tools tests`. The optional
  `vk_pipeline_check <spirv dir> [cache.bin] [--jobs N]` creates a module (and, for compute shaders, a
  pipeline) for every translated shader and reports failures plus the saved cache path.
- GPU tests are small hand-written RDNA2 programs in `runtime/test/shaders`. `build.py` turns them into SPIR-V
  using `llvm-mc` with the AMDGPU target (LLVM 21+). `check.py` runs them on your GPU and compares the results.
  A new test gets its expected values in the comment at its top and a check in `check.py`.
  [docs/testing.md](docs/testing.md) covers setup per OS, including native Windows options, and known problems.
- Say what you tested and on which GPU in the pull request.
- Run the translator tests with `python -I scripts/run_semantic_tests.py tests/semantic` (see `tests/semantic/README.md`). A new
  translator rule should come with a small hand-written `.asm` fixture and its expected output words.
