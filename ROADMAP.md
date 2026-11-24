# Ambrosia roadmap

Ambrosia focuses on a reproducible development experience, careful validation
and small reviewed contributions. This roadmap describes this fork's work;
inherited technical measurements remain attributed to their original sources.

## 1. Reliable offline setup

Delivered locally: an offline doctor checks Python, contributor sources, optional
repository configuration and tool discovery. Reports exclude host paths and
credentials. It does not execute tools, install anything or certify builds.

Acceptance: a clean checkout can explain a missing contributor prerequisite with
an actionable message; JSON contains no host/account values; a failing check
returns nonzero. Record the exact revision and test environment.

Next: review a separate opt-in design for external tool version/target checks.
No automatic downloads or game-folder scans belong in this milestone.

## 2. Reproducible contributions

Delivered locally: 78 explicitly selected game-free tests, component suites,
non-importing listing and redacted results. A separate old-behavior control
produces four real failures. The final local Linux run passed 78/78 methods.

Prepared: automatic Windows/Linux contributor CI and a small-task review policy.
Remote CI has not run yet. These checks do not replace the full component suites.

Acceptance: both hosted OS jobs run the complete advertised selections; missing,
skipped or failed coverage cannot produce a full pass; every PR names one task,
an owner, scope and reproducible validation. Invite an initial two or three
collaborators only after access and review rules are configured.

## 3. Native build confidence

Prepared: inherited runtime, full CI and decoder jobs remain available behind
explicit opt-in. They have not been run for the Ambrosia rename locally.

Acceptance: record Windows C++/Rust decoder results and the portable Linux build
at an exact revision; account for dependencies, skips and platform differences;
run relevant complete component tests; retain synthetic reproducible failures.
Only then describe the renamed native components as build-verified.

No playable release, retail boot, GPU correctness or public delivery date is
promised by these milestones. Runtime/game validation needs its own evidence.
