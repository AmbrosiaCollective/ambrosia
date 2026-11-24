"""Security contracts for the credential-bearing workflow; no Git, network or packages."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github/workflows/sync-personal-fork.yml"
GUARD = "if: github.repository == 'AmbrosiaCollective/ambrosia' && github.ref == 'refs/heads/main'"


def violations(text):
    """Conservative checks for this one workflow, not a general YAML validator."""
    found = []
    required = {
        "trusted-main-only": GUARD,
        "push-main": "  push:\n    branches: [main]\n",
        "manual-recovery": "  workflow_dispatch:",
        "read-only-token": "permissions:\n  contents: read\n",
        "no-cancelled-push": "  cancel-in-progress: false",
        "bounded-run": "    timeout-minutes: 5",
        "private-temp-files": "          umask 077",
        "cleanup": "          trap 'rm -rf -- \"$work\"' EXIT",
        "key-not-in-child-env": "          unset FORK_SYNC_SSH_KEY",
        "strict-host-verification": "StrictHostKeyChecking=yes",
        "identity-scoped": "IdentitiesOnly=yes",
        "fast-forward-only": "git merge-base --is-ancestor refs/remotes/mirror/main refs/remotes/upstream/main",
        "fixed-destination-branch": "refs/remotes/upstream/main:refs/heads/main",
        "no-hook-execution": "core.hooksPath=/dev/null",
    }
    for name, value in required.items():
        if value not in text:
            found.append(name)
    if re.findall(r"\$\{\{\s*secrets\.([A-Z_]+)\s*\}\}", text) != ["FORK_SYNC_SSH_KEY"]:
        found.append("unexpected-secret")
    if re.findall(r"\$\{\{\s*vars\.([A-Z_]+)\s*\}\}", text) != ["FORK_SYNC_REPOSITORY"]:
        found.append("unexpected-variable")
    expressions = [value.strip() for value in re.findall(r"\$\{\{(.*?)\}\}", text, re.S)]
    if expressions != ["vars.FORK_SYNC_REPOSITORY", "secrets.FORK_SYNC_SSH_KEY"]:
        found.append("unexpected-expression")
    for pattern in (r"\bpull_request(?:_target)?\s*:", r"\bworkflow_run\s*:",
                    r"\buses\s*:", r"--force(?:-with-lease)?\b", r"StrictHostKeyChecking=no",
                    r"\bcontents:\s*write", r"\$\{\{\s*(?:github\.event|inputs\.)"):
        if re.search(pattern, text):
            found.append("unsafe-surface")
    if not re.search(r'^\s*if \[\[ ! "\$FORK_SYNC_REPOSITORY" =~ ', text, re.M):
        found.append("unvalidated-target")
    return found


class ForkSyncContracts(unittest.TestCase):
    def setUp(self):
        self.text = WORKFLOW.read_text(encoding="utf-8")

    def test_actual_workflow_has_required_boundaries(self):
        self.assertEqual(violations(self.text), [])

    def test_removed_repository_guard_is_rejected(self):
        self.assertIn("trusted-main-only", violations(self.text.replace(GUARD, "if: true")))

    def test_removed_ancestry_check_is_rejected(self):
        value = "git merge-base --is-ancestor refs/remotes/mirror/main refs/remotes/upstream/main"
        self.assertIn("fast-forward-only", violations(self.text.replace(value, "true")))

    def test_force_push_and_unsafe_trigger_are_rejected(self):
        for extra in ("\n  pull_request:\n", "\n  pull_request_target:\n", "\n  workflow_run:\n",
                      "\n          git push --force destination main\n"):
            with self.subTest(extra=extra):
                self.assertIn("unsafe-surface", violations(self.text + extra))

    def test_unrelated_secret_and_variable_are_rejected(self):
        for extra in ("${{ secrets.OTHER_KEY }}", "${{ vars.OTHER_REPOSITORY }}",
                      "${{ toJSON(secrets) }}", "${{ secrets['OTHER_KEY'] }}"):
            self.assertTrue(violations(self.text + extra))

    def test_missing_key_cleanup_or_host_check_is_rejected(self):
        for value in ("unset FORK_SYNC_SSH_KEY", "StrictHostKeyChecking=yes", "umask 077",
                      "trap 'rm -rf -- \"$work\"' EXIT"):
            self.assertTrue(violations(self.text.replace(value, "REMOVED")))

    def test_checkout_and_write_token_are_rejected(self):
        self.assertIn("unsafe-surface", violations(self.text + "\n      - uses: actions/checkout@HEAD\n"))
        self.assertIn("unsafe-surface", violations(self.text.replace("contents: read", "contents: write")))

    def test_contributor_ci_runs_contract_checks(self):
        text = (ROOT / ".github/workflows/contributor.yml").read_text(encoding="utf-8")
        self.assertIn("run: python -I tests/test_fork_sync.py", text)


if __name__ == "__main__":
    unittest.main()
