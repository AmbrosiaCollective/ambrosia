# Ambrosia identity

Ambrosia is an independent fork. First-party project names, CMake options,
`AMBROSIA_*` environment variables, local work/cache paths, C++ namespaces, Rust
crate/FFI names, Python module/header filenames and associated tests/docs use the
Ambrosia identity. Use fresh Ambrosia build/cache directories after switching;
the old project's configuration prefixes are not aliases.

The runtime/game-facing formats, DLL names and executable names that identify
external software are unchanged. Licenses, third-party attribution, pinned
dependencies and historical source references are preserved. Existing technical
measurements are inherited evidence, not new test results from this fork.

The GitHub repository itself has not been renamed or published by this local
change. The planned destination is `AmbrosiaCollective/ambrosia`; clone examples
will work only after that private repository exists and access is granted. For optional dashboard GitHub metadata, supply `--repo OWNER/REPO`
or `AMBROSIA_GITHUB_REPO` explicitly.

Discord: https://discord.gg/Bdguf59NvB
Server description: Ambrosia — GTA VI recompilation, tools and research.
