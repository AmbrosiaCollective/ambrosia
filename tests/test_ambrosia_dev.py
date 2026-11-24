"""Offline developer commands: real synthetic test execution and failure controls.

    python -I tests/test_ambrosia_dev.py
"""
import contextlib
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import ambrosia_dev as dev  # noqa: E402


def catalog(methods=None):
    return {"fixture": {"description": "Synthetic control", "module": "tests/fixture.py",
                        "requires": [], "methods": methods or ["Checks.test_one"]}}


class CatalogTests(unittest.TestCase):
    def read(self, data):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "catalog.json"
            path.write_text(json.dumps(data), encoding="utf-8", newline="\n")
            return dev.load_catalog(path)

    def test_real_catalog_has_nonempty_explicit_methods(self):
        data = dev.load_catalog()
        self.assertTrue(data)
        for entry in data.values():
            self.assertTrue(entry["methods"])
            self.assertTrue(all(".test_" in method for method in entry["methods"]))

    def test_invalid_roots_and_schema_fail_cleanly(self):
        for data in ([], None, "bad", {}, {"schema": "other", "suites": catalog()},
                     {"schema": "ambrosia.suites/1", "suites": {}}):
            with self.subTest(data=data), self.assertRaises(dev.CatalogError):
                self.read(data)

    def test_invalid_paths_and_methods_are_rejected(self):
        for field, value in [("module", "../outside.py"), ("module", "/private.py"),
                             ("module", "tests\\outside.py"), ("module", "C:/private.py"),
                             ("module", "tools/tool.py"), ("methods", []),
                             ("requires", ["tools/bad\nname.py"]),
                             ("methods", [["not", "hashable"]]), ("methods", ["setUp"]),
                             ("methods", ["Checks.test_one", "Checks.test_one"]),
                             ("requires", ["../private.py"])]:
            data = catalog()
            data["fixture"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(dev.CatalogError):
                self.read({"schema": "ambrosia.suites/1", "suites": data})

    def test_missing_invalid_encoding_and_oversized_catalog(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "catalog.json"
            with self.assertRaises(dev.CatalogError):
                dev.load_catalog(path)
            for content in (b"\xff", b"{bad", b" " * 65537):
                path.write_bytes(content)
                with self.assertRaises(dev.CatalogError):
                    dev.load_catalog(path)


class DoctorTests(unittest.TestCase):
    def fixture(self, root):
        path = root / "tests/fixture.py"
        path.parent.mkdir()
        path.write_text("raise AssertionError('doctor must not import this')\n", encoding="utf-8", newline="\n")

    def test_discovery_does_not_execute_tools_or_import_tests(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            with mock.patch.object(subprocess, "run", side_effect=AssertionError("executed")), \
                    mock.patch.object(socket, "socket", side_effect=AssertionError("network")), \
                    mock.patch.object(dev.shutil, "which", return_value="/private/account/compiler"), \
                    mock.patch.dict(os.environ, {"AMBROSIA_GITHUB_REPO": ""}):
                report = dev.doctor("runtime", root, catalog=catalog())
            self.assertEqual(report["status"], "discovered")
            self.assertIn("versions", report["limitations"])
            self.assertNotIn("/private", json.dumps(report))

    def test_missing_tools_have_actionable_fixes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            with mock.patch.object(dev.shutil, "which", return_value=None):
                report = dev.doctor("translator", root, catalog=catalog())
            missing = [item for item in report["checks"] if item["id"].startswith("tool:")]
            self.assertEqual(len(missing), 3)
            self.assertTrue(all(item["status"] == "missing" and item["fix"] for item in missing))
            self.assertEqual(report["status"], "missing")

    def test_missing_source_is_not_reported_as_ready(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = dev.doctor("contributor", Path(tmp), catalog=catalog())
        self.assertEqual(report["status"], "missing")
        self.assertTrue(any(c["id"] == "source:tests/fixture.py" and c["status"] == "missing"
                            for c in report["checks"]))

    def test_old_python_is_a_missing_requirement(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(dev.sys, "version_info", (3, 11, 9)):
            report = dev.doctor("contributor", Path(tmp), catalog=catalog())
        self.assertEqual(report["checks"][0]["status"], "missing")
        self.assertIn("3.11.9", report["checks"][0]["detail"])

    def test_environment_values_and_credentials_are_never_reported(self):
        sentinel = "PRIVATE_ACCOUNT_AND_PATH_SENTINEL"
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, {
                "AMBROSIA_GITHUB_REPO": "https://host/" + sentinel,
                "GH_TOKEN": sentinel, "HOME": "/home/" + sentinel}):
            report = dev.doctor("contributor", Path(tmp), catalog=catalog())
        self.assertNotIn(sentinel, json.dumps(report))
        self.assertEqual(report["checks"][-1]["status"], "missing")


class RunnerTests(unittest.TestCase):
    def execute(self, body, methods=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "tests").mkdir()
            (root / "tests/fixture.py").write_text("import unittest\n" + body, encoding="utf-8", newline="\n")
            return dev.run_tests("fixture", root, catalog=catalog(methods))

    def test_real_selected_assertion_runs_and_unselected_method_does_not(self):
        report = self.execute("class Checks(unittest.TestCase):\n"
                              " def test_one(self): self.assertEqual(2 + 2, 4)\n"
                              " def test_unselected(self): raise RuntimeError('must not run')\n"
                              "def load_tests(*args): raise RuntimeError('no discovery hooks')\n")
        self.assertEqual((report["status"], report["run"], report["expected"]), ("passed", 1, 1))

    def test_behavioral_failure_is_redacted_and_not_green(self):
        report = self.execute("print('PRIVATE_IMPORT_OUTPUT')\n"
                              "class Checks(unittest.TestCase):\n"
                              " def test_one(self):\n"
                              "  print('PRIVATE_TEST_OUTPUT')\n"
                              "  self.fail('/home/PRIVATE_TRACEBACK')\n")
        self.assertEqual((report["status"], report["failures"]), ("failed", 1))
        self.assertNotIn("PRIVATE", json.dumps(report))
        self.assertEqual(report["issues"][0]["test"], "fixture:Checks.test_one")

    def test_error_is_not_green(self):
        report = self.execute("class Checks(unittest.TestCase):\n def test_one(self): raise RuntimeError('private')\n")
        self.assertEqual((report["status"], report["errors"]), ("failed", 1))

    def test_skip_is_incomplete(self):
        report = self.execute("class Checks(unittest.TestCase):\n def test_one(self): self.skipTest('private reason')\n")
        self.assertEqual((report["status"], report["skipped"]), ("incomplete", 1))
        self.assertNotIn("private reason", json.dumps(report))

    def test_expected_failure_is_incomplete(self):
        report = self.execute("class Checks(unittest.TestCase):\n @unittest.expectedFailure\n"
                              " def test_one(self): self.fail('known')\n")
        self.assertEqual((report["status"], report["expected_failures"]), ("incomplete", 1))

    def test_unexpected_success_is_failed(self):
        report = self.execute("class Checks(unittest.TestCase):\n @unittest.expectedFailure\n"
                              " def test_one(self): pass\n")
        self.assertEqual((report["status"], report["unexpected_successes"]), ("failed", 1))

    def test_subtest_values_do_not_leak(self):
        report = self.execute("class Checks(unittest.TestCase):\n def test_one(self):\n"
                              "  with self.subTest(path='/home/PRIVATE_SUBTEST'): self.fail('PRIVATE')\n")
        self.assertEqual(report["status"], "failed")
        self.assertNotIn("PRIVATE", json.dumps(report))
        self.assertEqual(report["issues"][0]["test"], "fixture:Checks.test_one")

    def test_missing_method_cannot_silently_shrink_the_run(self):
        report = self.execute("class Checks(unittest.TestCase):\n def test_other(self): pass\n")
        self.assertEqual((report["status"], report["run"], report["expected"]), ("incomplete", 0, 1))
        self.assertEqual(report["issues"][0]["kind"], "load-or-run-error")

    def test_missing_source_and_import_error_are_incomplete(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = dev.run_tests("fixture", Path(tmp), catalog=catalog())
        self.assertEqual(report["issues"][0]["kind"], "missing-source")
        report = self.execute("raise ImportError('/home/PRIVATE_IMPORT')\n")
        self.assertEqual(report["status"], "incomplete")
        self.assertNotIn("PRIVATE", json.dumps(report))

    def test_import_system_exit_is_incomplete_and_does_not_escape(self):
        report = self.execute("raise SystemExit('PRIVATE_EXIT')\n")
        self.assertEqual(report["status"], "incomplete")
        self.assertNotIn("PRIVATE", json.dumps(report))

    def test_sys_path_is_restored_after_failed_import(self):
        before = list(sys.path)
        self.execute("raise RuntimeError('private')\n")
        self.assertEqual(sys.path, before)


class CommandTests(unittest.TestCase):
    def command(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = dev.main(args)
        return code, json.loads(out.getvalue())

    def test_list_is_json_without_importing_test_modules(self):
        with mock.patch.object(dev.importlib.util, "spec_from_file_location", side_effect=AssertionError("imported")):
            code, report = self.command("test", "--list", "--json")
        self.assertEqual(code, 0)
        self.assertEqual(report["expected"], sum(len(e["methods"]) for e in report["suites"]))

    def test_unknown_suite_and_invalid_catalog_are_usage_errors(self):
        code, report = self.command("test", "unknown", "--json")
        self.assertEqual((code, report["status"]), (2, "error"))
        with mock.patch.object(dev, "load_catalog", side_effect=dev.CatalogError("Restore catalog.")):
            code, report = self.command("doctor", "--json")
        self.assertEqual((code, report["status"]), (2, "error"))

    def test_incomplete_test_result_has_nonzero_exit(self):
        with mock.patch.object(dev, "run_tests", return_value={"status": "incomplete", "run": 0}):
            code, report = self.command("test", "--json")
        self.assertEqual((code, report["status"]), (1, "incomplete"))


if __name__ == "__main__":
    unittest.main()
