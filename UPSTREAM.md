# Upstream provenance and credit

Ambrosia is an independent derivative of
[ProjectLeonida/Leonida](https://github.com/ProjectLeonida/Leonida). Its original
contributors built the inherited recompilation tools, runtime, format research
and test coverage. A rename does not make that work newly authored by Ambrosia.

The reviewed local baseline for this import is
`a03af048105a14e9f35bf93d78238347d79aebae`: upstream main
`5d74c272363f2703265d44e6aaab1a1df8ddfaf3` with the separately prepared repeated
`--extra` hashdict fix. Ambrosia-specific work after that baseline is described in
[CHANGELOG.md](CHANGELOG.md). Original technical citations remain linked to their
upstream issues, PRs and measured revisions.

The code retains its [GPLv3 license](LICENSE), existing copyright/notice text and
third-party license files. Dependencies and submodules retain their own
attribution and pinned revisions. The Ambrosia visual identity is new artwork
inspired by the upstream visual direction; upstream artwork credit remains in
the README. No game artwork, game files, keys or executable is distributed here.

Launch preparation uses a source snapshot with explicit provenance. The local
checkout is shallow; it does not contain the upstream project's complete Git
history. Do not describe a snapshot import as a full-history mirror or rewrite
upstream credit to imply otherwise. The existing fork and preserved source
archives are not modified by creating a separate private organization repository.
