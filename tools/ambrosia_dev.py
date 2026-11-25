"""Ambrosia developer commands, native builds and launch readiness.

Doctor/listing do not execute external tools. Tests execute in-process; builds
invoke CMake. These commands do not provide an operating-system sandbox.
"""
import argparse
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
CATALOG = Path(__file__).with_name("ambrosia_suites.json")
PROFILES = ("contributor", "translator", "runtime")
NOTE = "Discovery only: compiler/SDK versions, builds, GPU and game behavior are not verified."


class CatalogError(ValueError):
    """Invalid or incomplete contributor suite configuration."""


def _relative_file(value):
    return (isinstance(value, str) and bool(value)
            and re.fullmatch(r"[A-Za-z0-9_./-]+", value) is not None
            and not value.startswith("/") and "\\" not in value and ":" not in value
            and all(part not in ("", ".", "..") for part in value.split("/")))


def load_catalog(path=CATALOG):
    """Read declarative selections; listing/doctor never imports a test module."""
    try:
        if path.stat().st_size > 65536:
            raise CatalogError("Contributor catalog exceeds the 64 KiB limit.")
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, UnicodeError) as error:
        raise CatalogError("Contributor catalog is missing or invalid; restore tools/ambrosia_suites.json.") from error
    suites = data.get("suites") if isinstance(data, dict) else None
    if not isinstance(data, dict) or data.get("schema") != "ambrosia.suites/1" or not isinstance(suites, dict) or not suites:
        raise CatalogError("Contributor catalog has an unsupported schema or no suites.")
    for name, entry in suites.items():
        if not re.fullmatch(r"[a-z][a-z0-9_-]{0,39}", name) or name == "core" or not isinstance(entry, dict):
            raise CatalogError("Contributor catalog contains an invalid suite.")
        methods, required = entry.get("methods"), entry.get("requires")
        module = entry.get("module")
        if (not _relative_file(module) or not module.startswith("tests/") or not module.endswith(".py")
                or not isinstance(entry.get("description"), str)
                or not isinstance(methods, list) or not methods
                or not all(isinstance(item, str) and re.fullmatch(r"[A-Za-z_]\w*\.test_\w+", item) for item in methods)
                or len(methods) != len(set(methods))
                or not isinstance(required, list) or not all(_relative_file(item) for item in required)):
            raise CatalogError("Contributor catalog contains invalid paths or test selections.")
    return suites


def selection(catalog, name):
    if name == "core":
        return list(catalog.items())
    if name not in catalog:
        raise CatalogError("Unknown suite; use test --list to see available names.")
    return [(name, catalog[name])]


def _source_exists(root, relative):
    try:
        path = root / relative
        return path.resolve().is_relative_to(root.resolve()) and path.is_file()
    except (OSError, RuntimeError):
        return False


def source_files(selected):
    return sorted({path for _, entry in selected for path in [entry["module"], *entry["requires"]]})


def doctor(profile, root=ROOT, *, catalog=None):
    """Return a redacted discovery report, without executing discovered programs."""
    catalog = load_catalog() if catalog is None else catalog
    checks = []

    def add(name, ok, detail, fix=""):
        checks.append({"id": name, "status": "pass" if ok else "missing", "detail": detail,
                       "fix": fix if not ok else ""})

    add("python", sys.version_info >= (3, 12),
        "Running Python " + ".".join(map(str, sys.version_info[:3])) + "; minimum 3.12.",
        "Use Python 3.12 or newer to run ambrosia.py.")
    for path in source_files(selection(catalog, "core")):
        add("source:" + path, _source_exists(root, path), "Required contributor source: " + path,
            "Restore this file from the same Ambrosia revision; do not download game files.")
    tools = []
    if profile == "translator":
        tools = [("llvm-mc", ("llvm-mc",)), ("spirv-as", ("spirv-as",)), ("spirv-val", ("spirv-val",))]
    elif profile == "runtime":
        tools = [("cmake", ("cmake",)), ("cargo", ("cargo",)),
                 ("C++ compiler", ("cl", "clang-cl") if os.name == "nt" else ("c++", "g++", "clang++"))]
    for label, candidates in tools:
        found = any(shutil.which(name) is not None for name in candidates)
        add("tool:" + label, found, label + " executable discovery only; version not probed.",
            "Install " + label + " separately and expose it on PATH; consult docs/testing.md for supported versions.")
    target = os.environ.get("AMBROSIA_GITHUB_REPO", "")
    add("github-target", not target or bool(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", target)),
        "Optional AMBROSIA_GITHUB_REPO syntax; its value and account are omitted.",
        "Unset AMBROSIA_GITHUB_REPO or set it to OWNER/REPOSITORY (not a URL).")
    return {"schema": "ambrosia.doctor/1", "profile": profile,
            "status": "discovered" if all(c["status"] == "pass" for c in checks) else "missing",
            "checks": checks, "limitations": NOTE,
            "privacy": "No host paths, account names, environment values or credentials are included."}


class QuietOutput(io.TextIOBase):
    """Discard test/traceback text, which may contain local paths or fixture data."""
    def write(self, text):
        return len(text)

    def flush(self):
        pass


def run_tests(name, root=ROOT, *, catalog=None):
    catalog = load_catalog() if catalog is None else catalog
    selected = selection(catalog, name)
    expected = sum(len(entry["methods"]) for _, entry in selected)
    report = {"schema": "ambrosia.tests/1", "suite": name, "expected": expected,
              "run": 0, "failures": 0, "errors": 0, "skipped": 0, "unexpected_successes": 0,
              "expected_failures": 0, "status": "incomplete", "issues": [],
              "scope": "Explicit in-process synthetic tests only; not a full suite, build or sandbox."}
    missing = [p for p in source_files(selected) if not _source_exists(root, p)]
    if missing:
        report["issues"] = [{"kind": "missing-source", "source": p} for p in missing]
        return report
    old_path = list(sys.path)
    quiet = QuietOutput()
    ids = {}
    try:
        sys.path[:0] = [str(root), str(root / "tools"), str(root / "tests")]
        with contextlib.redirect_stdout(quiet), contextlib.redirect_stderr(quiet):
            suite = unittest.TestSuite()
            for suite_name, entry in selected:
                module_name = "_ambrosia_suite_" + suite_name
                spec = importlib.util.spec_from_file_location(module_name, root / entry["module"])
                module = importlib.util.module_from_spec(spec)
                sys.modules[module_name] = module
                spec.loader.exec_module(module)
                for method in entry["methods"]:
                    class_name, method_name = method.split(".")
                    cls = getattr(module, class_name)
                    if not isinstance(cls, type) or not issubclass(cls, unittest.TestCase):
                        raise CatalogError("Selected test class is unavailable.")
                    if not callable(getattr(cls, method_name, None)):
                        raise CatalogError("Selected test method is unavailable.")
                    case = cls(method_name)
                    ids[case.id()] = suite_name + ":" + method
                    suite.addTest(case)
            result = unittest.TextTestRunner(stream=quiet, verbosity=0).run(suite)
    except (Exception, SystemExit):
        report["issues"] = [{"kind": "load-or-run-error", "detail": "Could not execute the selected tests; inspect the suite locally."}]
        return report
    finally:
        sys.path[:] = old_path
    report.update(run=result.testsRun, failures=len(result.failures), errors=len(result.errors),
                  skipped=len(result.skipped), unexpected_successes=len(result.unexpectedSuccesses),
                  expected_failures=len(result.expectedFailures))
    for kind, entries in [("failure", result.failures), ("error", result.errors), ("skip", result.skipped),
                          ("expected-failure", result.expectedFailures)]:
        for test, _ in entries:
            # Subtest IDs can contain arbitrary input values: report only the curated parent method.
            parent = getattr(test, "test_case", test)
            report["issues"].append({"kind": kind, "test": ids.get(parent.id(), "selected-test")})
    for test in result.unexpectedSuccesses:
        report["issues"].append({"kind": "unexpected-success", "test": ids.get(test.id(), "selected-test")})
    if result.testsRun == expected and not report["issues"] and result.wasSuccessful():
        report["status"] = "passed"
    elif result.failures or result.errors or result.unexpectedSuccesses:
        report["status"] = "failed"
    return report


def main(argv=None):
    from ambrosia_start import add_commands, dispatch

    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("doctor", help="Discover local requirements; never execute tools or install anything")
    check.add_argument("--profile", choices=PROFILES, default="contributor")
    check.add_argument("--json", action="store_true", help="Emit a redacted JSON report to stdout")
    test = commands.add_parser("test", help="Run reviewed game-free tests in-process (not the full test suite)")
    test.add_argument("suite", nargs="?", default="core", help="core, hashdict, paths, manifests, console or developer")
    test.add_argument("--list", action="store_true", help="List exact selections without importing or running tests")
    test.add_argument("--json", action="store_true", help="Emit counts and curated test IDs; omit tracebacks and captured output")
    add_commands(commands)
    args = parser.parse_args(argv)
    if args.command in ("menu", "build", "launch"):
        return dispatch(args)
    try:
        catalog = load_catalog()
        if args.command == "doctor":
            report = doctor(args.profile, catalog=catalog)
            code = 0 if report["status"] == "discovered" else 1
        elif args.list:
            selected = selection(catalog, args.suite)
            report = {"schema": "ambrosia.suite-list/1", "suite": args.suite,
                      "suites": [{"name": n, **entry} for n, entry in selected],
                      "expected": sum(len(e["methods"]) for _, e in selected)}
            code = 0
        else:
            report = run_tests(args.suite, catalog=catalog)
            code = 0 if report["status"] == "passed" else 1
    except CatalogError as error:
        report = {"schema": "ambrosia.error/1", "status": "error", "message": str(error)}
        code = 2
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    elif "checks" in report:
        print("Ambrosia doctor / " + report["profile"])
        for item in report["checks"]:
            print("  " + item["status"].upper() + "  " + item["id"] + ": " + item["detail"])
            if item["fix"]:
                print("        " + item["fix"])
        print("Result: " + report["status"] + ". " + report["limitations"])
    elif "suites" in report:
        for entry in report["suites"]:
            print(entry["name"] + ": " + entry["description"])
            for method in entry["methods"]:
                print("  " + method)
        print("Expected: " + str(report["expected"]) + " selected methods; nothing executed.")
    elif "run" in report:
        print("Ambrosia tests / " + report["suite"] + ": " + report["status"].upper())
        print("  {run}/{expected} run; {failures} failures, {errors} errors, {skipped} skips".format(**report))
        for issue in report["issues"]:
            print("  " + issue["kind"] + ": " + issue.get("test", issue.get("source", issue.get("detail", ""))))
        print(report["scope"])
    else:
        print("ERROR: " + report["message"])
    return code
