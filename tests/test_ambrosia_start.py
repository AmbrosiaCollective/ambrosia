"""Native command failure and terminal controls; all child execution is mocked."""
import argparse
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import ambrosia_dev as dev
import ambrosia_start as start


class Terminal(io.StringIO):
    def isatty(self):
        return True


class NativeStartTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        base = Path(self.scratch.name)
        self.root = base / "source with spaces"
        (self.root / "runtime/native").mkdir(parents=True)
        (self.root / "runtime/native/CMakeLists.txt").write_text(
            "# synthetic source marker\n", encoding="utf-8", newline="\n")
        self.destination = base / "build with spaces"
        self.output = io.StringIO()
        self.console = start.Console(self.output)
        self.args = argparse.Namespace(build_dir=self.destination, jobs=2, dry_run=False)

    def test_preview_never_runs_tools_or_creates_output(self):
        self.args.dry_run = True
        with mock.patch.object(start.subprocess, "Popen", side_effect=AssertionError("executed")):
            self.assertEqual(start.build(self.args, self.console, self.root), 0)
        self.assertFalse(self.destination.exists())
        self.assertIn("Build preview", self.output.getvalue())
        commands = start.build_commands(self.root, self.destination, 2)
        self.assertEqual(commands[0][2], str(self.root / "runtime/native"))
        self.assertEqual(commands[0][4], str(self.destination))
        self.assertNotIn("shell", commands[0])

    def test_failed_configuration_stops_before_compile_and_can_retry(self):
        with mock.patch.object(start, "show_requirements", return_value=True), \
                mock.patch.object(start, "run_stage", return_value=7) as stage:
            self.assertEqual(start.build(self.args, self.console, self.root), 7)
            self.assertEqual(stage.call_count, 1)
        self.assertNotIn("build completed", self.output.getvalue())
        with mock.patch.object(start, "show_requirements", return_value=True), \
                mock.patch.object(start, "run_stage", side_effect=[0, 9]) as stage:
            self.assertEqual(start.build(self.args, self.console, self.root), 9)
            self.assertEqual(stage.call_count, 2)
        self.assertEqual(len(list(self.destination.glob("*.log"))), 2)

    def test_success_builds_only_bootstrap_and_does_not_launch(self):
        with mock.patch.object(start, "show_requirements", return_value=True), \
                mock.patch.object(start, "run_stage", return_value=0) as stage:
            self.assertEqual(start.build(self.args, self.console, self.root), 0)
        self.assertEqual(stage.call_count, 2)
        self.assertEqual(stage.call_args_list[1].args[0][-2:], ["--target", "ambrosia-host"])
        self.assertIn("Game launch remains unavailable", self.output.getvalue())

    def test_missing_prerequisite_leaves_filesystem_untouched(self):
        with mock.patch.object(start, "show_requirements", return_value=False), \
                mock.patch.object(start, "run_stage", side_effect=AssertionError("executed")):
            self.assertEqual(start.build(self.args, self.console, self.root), 1)
        self.assertFalse(self.destination.exists())

    def test_checkout_ancestors_and_symlinked_outputs_are_rejected(self):
        alias = self.root.parent / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        for destination in (self.root, self.root / "build", self.root.parent, alias / "build"):
            with self.subTest(destination=destination), self.assertRaises(ValueError):
                start.build_directory(self.root, destination)

    def test_foreign_directory_and_foreign_cmake_cache_are_rejected(self):
        self.destination.mkdir()
        (self.destination / "keep.txt").write_text("keep", encoding="utf-8", newline="\n")
        with self.assertRaises(ValueError):
            start.build_directory(self.root, self.destination)
        (self.destination / "CMakeCache.txt").write_text(
            "CMAKE_HOME_DIRECTORY:INTERNAL=/different/source\n", encoding="utf-8", newline="\n")
        with self.assertRaises(ValueError):
            start.build_directory(self.root, self.destination)
        self.assertEqual((self.destination / "keep.txt").read_text(), "keep")

    def test_matching_existing_cmake_cache_is_accepted(self):
        self.destination.mkdir()
        (self.destination / "CMakeCache.txt").write_text(
            "CMAKE_HOME_DIRECTORY:INTERNAL=" + str(self.root / "runtime/native") + "\n",
            encoding="utf-8", newline="\n")
        self.assertEqual(start.build_directory(self.root, self.destination), self.destination)

    def test_symlinked_ownership_marker_cannot_overwrite_source(self):
        self.destination.mkdir()
        original = self.root / "runtime/native/CMakeLists.txt"
        (self.destination / ".ambrosia-native-source").symlink_to(original)
        with mock.patch.object(start, "show_requirements", return_value=True), \
                mock.patch.object(start, "run_stage", side_effect=AssertionError("executed")):
            self.assertEqual(start.build(self.args, self.console, self.root), 1)
        self.assertEqual(original.read_text(), "# synthetic source marker\n")

    def test_launch_returns_unavailable_without_executing_anything(self):
        with mock.patch.object(start.subprocess, "Popen", side_effect=AssertionError("executed")):
            self.assertEqual(start.launch(self.console), 1)
        self.assertIn("Game launch is not available", self.output.getvalue())

    def test_architecture_gate_rejects_arm_even_when_tools_exist(self):
        with mock.patch.object(start.platform, "system", return_value="Darwin"), \
                mock.patch.object(start.platform, "machine", return_value="arm64"), \
                mock.patch.object(start.shutil, "which", return_value="synthetic-tool"):
            self.assertFalse(start.show_requirements(self.console, self.root))
        self.assertIn("ARM is unsupported", self.output.getvalue())

    def test_no_color_plain_and_redirection_disable_escape_sequences(self):
        for output, plain, environment in (
                (Terminal(), False, {"NO_COLOR": ""}),
                (Terminal(), True, {}), (io.StringIO(), False, {})):
            with mock.patch.dict(os.environ, environment, clear=True):
                console = start.Console(output, plain=plain)
                console.event("status\033[31m\r injected")
            self.assertNotIn("\033", output.getvalue())
            self.assertRegex(output.getvalue(), r"\[\d\d:\d\d:\d\d\]")

    def test_color_is_used_on_supported_terminal(self):
        output = Terminal()
        with mock.patch.dict(os.environ, {"WT_SESSION": "synthetic"}, clear=True):
            start.Console(output).banner()
        self.assertIn("\033[35m", output.getvalue())

    def test_noninteractive_menu_fails_without_input_or_subprocess(self):
        with mock.patch("builtins.input", side_effect=AssertionError("prompted")):
            self.assertEqual(start.menu(self.args, self.console), 2)

    def test_menu_reprompts_and_keeps_launch_unavailable(self):
        output = Terminal()
        with mock.patch.object(start.sys, "stdin", Terminal()), \
                mock.patch("builtins.input", side_effect=["bad", "3", "0"]), \
                mock.patch.object(start.subprocess, "Popen", side_effect=AssertionError("executed")):
            self.assertEqual(start.menu(self.args, start.Console(output, plain=True)), 0)
        self.assertIn("Choose 0, 1, 2 or 3", output.getvalue())
        self.assertIn("Game launch is not available", output.getvalue())

    def test_stage_streams_log_and_preserves_exit_code(self):
        child = mock.MagicMock()
        child.__enter__.return_value = child
        child.stdout = io.StringIO("compiler message\n")
        child.wait.return_value = 4
        log = io.StringIO()
        command = ["cmake", "--build", "path with spaces"]
        with mock.patch.object(start.subprocess, "Popen", return_value=child) as popen:
            self.assertEqual(start.run_stage(command, self.root, self.console, log), 4)
        self.assertEqual(popen.call_args.args[0], command)
        self.assertNotIn("shell", popen.call_args.kwargs)
        self.assertEqual(log.getvalue(), "compiler message\n")

    def test_interrupted_stage_terminates_child(self):
        child = mock.MagicMock()
        child.__enter__.return_value = child
        child.stdout.__iter__.side_effect = KeyboardInterrupt
        with mock.patch.object(start.subprocess, "Popen", return_value=child), \
                self.assertRaises(KeyboardInterrupt):
            start.run_stage(["cmake"], self.root, self.console, io.StringIO())
        child.terminate.assert_called_once()

    def test_real_parser_dispatch_and_invalid_jobs(self):
        with contextlib.redirect_stdout(self.output), mock.patch.object(start, "dispatch", return_value=17) as dispatch:
            self.assertEqual(dev.main(["build", "--dry-run", "--jobs", "3"]), 17)
        self.assertEqual(dispatch.call_args.args[0].jobs, 3)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
            dev.main(["build", "--jobs", "0"])
        self.assertEqual(caught.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
