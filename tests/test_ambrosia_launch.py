"""Launch policy checks without packages, Git, processes, network or game files.

    python -I tests/test_ambrosia_launch.py

These checks assert the repository's specific workflow contracts. They are not a
general YAML parser or substitute for workflow syntax/expression linting.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent.parent
WORKFLOWS = ROOT / ".github/workflows"


def jobs(text):
    body = text.split("\njobs:\n", 1)[1]
    matches = list(re.finditer(r"^  ([A-Za-z][\w-]*):\s*$", body, re.M))
    return {m[1]: body[m.end():matches[n + 1].start() if n + 1 < len(matches) else len(body)]
            for n, m in enumerate(matches)}


def ungated_jobs(text):
    return [name for name, body in jobs(text).items()
            if not re.search(r"^    if: .*github\.event_name == 'workflow_dispatch'.*"
                             r"vars\.AMBROSIA_EXTENDED_CI == 'true'", body, re.M)]


class LaunchConfiguration(unittest.TestCase):
    def test_contributor_checks_both_platforms_without_dependencies(self):
        text = (WORKFLOWS / "contributor.yml").read_text(encoding="utf-8")
        self.assertRegex(text, r"os: \[ubuntu-latest, windows-latest\]")
        for command in ("python -I ambrosia.py doctor --json",
                        "python -I ambrosia.py test --list --json",
                        "python -I ambrosia.py test --json",
                        "python -I tests/test_ambrosia_launch.py"):
            self.assertIn("run: " + command, text)
        self.assertIn("submodules: false", text)
        self.assertNotRegex(text, r"pip install|apt-get|brew install|cargo build|cmake --build")
        self.assertIn("timeout-minutes: 10", text)

    def test_extended_jobs_all_require_explicit_opt_in(self):
        for name in ("ci.yml", "runtime.yml", "xma2-decoder.yml"):
            with self.subTest(workflow=name):
                text = (WORKFLOWS / name).read_text(encoding="utf-8")
                self.assertTrue(jobs(text))
                self.assertEqual(ungated_jobs(text), [])
                self.assertRegex(text, re.compile(r"^  workflow_dispatch:", re.M))

    def test_removing_one_gate_is_detected(self):
        text = (WORKFLOWS / "xma2-decoder.yml").read_text(encoding="utf-8")
        broken, changed = re.subn(r"^    if: .*\n", "", text, count=1, flags=re.M)
        self.assertEqual(changed, 1)
        self.assertEqual(ungated_jobs(broken), ["windows-ffi"])

    def test_workflow_actions_are_pinned_and_privileged_pr_events_absent(self):
        for path in sorted(WORKFLOWS.glob("*.yml")):
            with self.subTest(workflow=path.name):
                text = path.read_text(encoding="utf-8")
                self.assertNotRegex(text, re.compile(r"^  pull_request_target:", re.M))
                self.assertNotIn("${{ secrets.", text)
                for use in re.findall(r"\buses:\s+([^\s#]+)", text):
                    self.assertRegex(use, r"^[\w.-]+/[\w./-]+@[0-9a-f]{40}$")

    def test_codeowners_and_review_policy_name_the_planned_team(self):
        owners = (ROOT / ".github/CODEOWNERS").read_text(encoding="utf-8")
        self.assertIn("* @AmbrosiaCollective/maintainers", owners)
        policy = (ROOT / "docs/ci-policy.md").read_text(encoding="utf-8")
        self.assertIn("not equivalent to a protected branch", policy)
        self.assertIn("unrun coverage", policy)

    def test_roadmap_changelog_and_reporting_route_exist(self):
        for name in ("ROADMAP.md", "CHANGELOG.md", "CODE_OF_CONDUCT.md", "SECURITY.md"):
            text = (ROOT / name).read_text(encoding="utf-8")
            self.assertTrue(text.strip())
        security = (ROOT / "SECURITY.md").read_text(encoding="utf-8")
        self.assertIn("privately", security)
        self.assertIn("https://discord.gg/xEaVQtW5jH", security)
        template = (ROOT / ".github/pull_request_template.md").read_text(encoding="utf-8")
        self.assertNotIn("AI-assisted:", template)
        self.assertIn("Exact revision", template)


if __name__ == "__main__":
    unittest.main()
