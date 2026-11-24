# CI scope and opt-in builds

## Automatic checks

- **Contributor**: Windows and Linux, Python 3.12, offline doctor, exact suite
  listing, all 78 selected game-free methods and Python syntax. No submodule or
  package installation. Hosted checkout/setup actions still require GitHub's
  normal network and runner environment; the commands themselves are offline.
- **No Game Data**: tracked-name and content checks.
- **jactionlint**: workflow review using the existing pinned action.

All workflow actions are commit-pinned. Contributor checkouts do not persist
credentials and do not initialize submodules. The workflows use hosted runners,
read-only source access and no repository secrets. Running tests in a hosted
runner does not turn the contributor command into an OS sandbox.

## Personal fork synchronization

`Sync personal fork` is a separate, credential-bearing maintenance workflow.
It runs only in the official org repository on main pushes or an explicit
main-branch dispatch. It never runs on PRs or in forks, checks out no source,
and executes no repository scripts. Contributor jobs have no access to its key.

Repository variable `FORK_SYNC_REPOSITORY` names the destination personal fork.
Secret `FORK_SYNC_SSH_KEY` holds a write-enabled SSH deploy key attached only
to that fork, not an account-wide token or a key granting org write access.
The workflow pins GitHub's Ed25519 host key, writes its credential to private
temporary storage, removes it on exit and unsets it before running Git.
Deploy keys do not expire automatically; revoke the fork key and delete the
org secret when retiring the automation. Replace both when rotating credentials.

The destination's main branch must be an ancestor of org main. Divergence stops
the job without a merge, reset or force push. Keep personal experiments on other
branches. A normal non-force push also protects against concurrent changes.
Only main is synchronized; other branches, tags, issues and repository settings
are not mirrored. Runs are serialized and bounded to five minutes.

The profile fork can have Actions disabled to avoid duplicating the org's CI.
The official Contributor workflow remains the source of its README CI badge.
The sync workflow becomes active only after its reviewed change lands on main.

## Extended validation

The inherited **CI**, **Runtime** and **XMA2 decoder** jobs are retained, with their
existing tests and commands. They run only when either:

1. a maintainer manually dispatches that workflow; or
2. repository variable `AMBROSIA_EXTENDED_CI` is exactly `true`, enabling the
   existing push/PR triggers and path conditions.

The default is off. Skipped extended jobs are **unrun coverage**, not passing
build evidence. Do not configure their skipped results as proof of release
readiness. Contributor checks are intended as the initial PR gate.

Extended jobs may download submodules, packages, toolchains, Vulkan/SPIR-V tools
or other dependencies and can consume substantial runner time. Inspect the
workflow and its source changes, choose the exact revision and approve this work
before dispatching or enabling the variable. No such jobs were dispatched during
local launch preparation. Inherited Windows, Linux/Wine and macOS jobs retain
their original scope and platform limitations.

## Review and merge configuration

Create `AmbrosiaCollective/maintainers` with write-or-higher repository access
before relying on `.github/CODEOWNERS`. A file alone does not enforce approvals.
On a GitHub plan that supports private-repository rulesets/protection, require a
PR, one maintainer/code-owner approval, resolved review discussions and successful
Contributor OS checks plus No Game Data. Prevent direct/force pushes and branch
deletion, and dismiss stale approvals when code changes.

Configure check names from the first actual completed run rather than guessing
contexts before CI exists. If the selected plan cannot enforce these rules for
private repositories, record that limitation explicitly; a written policy is
not equivalent to a protected branch.

Contributor skips intentionally return a nonzero incomplete result. For example,
a runner that lacks symbolic-link privileges must have its environment corrected
or have that coverage explicitly reassessed; do not suppress a real failure just
to obtain a green check.
