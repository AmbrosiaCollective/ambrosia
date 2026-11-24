# Ambrosia changelog

## Public organization launch

- Published `AmbrosiaCollective/ambrosia` with public source and forking,
  collaborator-only pull requests, and access through approved org teams.
- Imported attributed source with the focused Ambrosia commits preserved.
  Removed workstation paths from every outgoing historical tree; retained
  original credits and licenses. The original local history remains intact.
- Linked the README to live Contributor CI. Native Windows compilation and
  game/GPU validation remain unverified.

## Unreleased — local launch preparation

- Independent Ambrosia branding, transparent wordmark and A emblem, updated
  first-party identifiers/configuration and community links. Historical upstream
  references, licenses and third-party attribution are preserved.
- Offline `python -I ambrosia.py doctor` with discovery profiles and redacted JSON.
- `python -I ambrosia.py test` with 78 explicit synthetic methods, component
  selections and nonzero failure/incomplete outcomes.
- Developer documentation, focused contribution rules, conduct/security guidance,
  roadmap and a prepared maintainer-team CODEOWNERS rule.
- Prepared automatic Windows/Linux contributor CI; inherited toolchain-heavy
  validation requires explicit opt-in. Remote CI/builds have not run yet.

Local evidence: Linux x86-64, CPython 3.12.3, 78 selected methods pass with no
failures/errors/skips under network/process-blocking isolation. A separate
negative control yields four behavioral failures. This is not a full suite,
native build, GPU test or game result. No release has been published.
