"""Synthetic startup traces only: no native images executed, game, network or GPU."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import boot_readiness as br


def summary(**changes):
    event = {"kind": "binding_summary", "modules": 1, "shim": 2, "host": 3,
             "module": 0, "unimplemented": 0, "strict": True, "mode": "dry-run"}
    event.update(changes)
    return event


def encoded(*events):
    return ("\n".join(json.dumps(e) for e in events) + "\n").encode()


def assessed(event=None, *extra, code=0):
    return br.assess(encoded(summary() if event is None else event,
                             *extra, {"kind": "exit", "code": code}))


class ReadinessTests(unittest.TestCase):
    def test_resolved_graph_is_not_labeled_boot_or_game_verified(self):
        report = assessed()
        self.assertEqual(report["status"], "imports-resolved")
        self.assertEqual(report["initial_bindings"]["host"], 3)
        self.assertIn("not certified", report["limitations"])

    def test_legacy_success_exit_does_not_hide_unimplemented_imports(self):
        report = assessed(summary(strict=False, unimplemented=7))
        self.assertEqual(report["status"], "blocked")
        self.assertIn("unimplemented-initial-imports", report["issues"])

    def test_strict_rejection_is_reported_as_blocked(self):
        report = assessed(summary(unimplemented=1),
                          {"kind": "startup_blocked", "reason": "unimplemented_imports"}, code=3)
        self.assertEqual(report["status"], "blocked")
        self.assertIn("startup-blocked", report["issues"])

    def test_old_trace_cannot_claim_resolved_imports(self):
        report = br.assess(encoded({"kind": "exit", "code": 0}))
        self.assertEqual(report["status"], "incomplete")

    def test_missing_exit_is_incomplete(self):
        self.assertEqual(br.assess(encoded(summary()))["status"], "incomplete")

    def test_crash_truncated_json_cannot_pass(self):
        raw = encoded(summary()) + b'{"kind":"exit","code":'
        self.assertEqual(br.assess(raw)["status"], "invalid")

    def test_missing_boolean_negative_and_oversized_counts_are_invalid(self):
        for field in br.COUNTS:
            for value in (None, True, -1, 2**31, 1.5, "1"):
                with self.subTest(field=field, value=value):
                    self.assertEqual(assessed(summary(**{field: value}))["status"], "invalid")

    def test_zero_modules_and_bad_mode_or_strict_are_invalid(self):
        for changes in ({"modules": 0}, {"mode": "ready"}, {"strict": 1}):
            self.assertEqual(assessed(summary(**changes))["status"], "invalid")

    def test_second_summary_cannot_overwrite_missing_imports(self):
        self.assertEqual(assessed(summary(strict=False, unimplemented=2), summary())["status"], "invalid")

    def test_exit_must_be_an_integer_and_must_not_repeat(self):
        for value in (False, None, "0", -1, 2**32):
            self.assertEqual(assessed(code=value)["status"], "invalid")
        self.assertEqual(assessed(None, {"kind": "exit", "code": 0})["status"], "invalid")

    def test_nonzero_exit_with_resolved_imports_is_still_blocked(self):
        self.assertEqual(assessed(code=37)["status"], "blocked")

    def test_inconsistent_strict_block_is_invalid(self):
        block = {"kind": "startup_blocked", "reason": "unimplemented_imports"}
        for event, code in ((summary(), 3), (summary(strict=False, unimplemented=1), 3),
                            (summary(unimplemented=1), 0)):
            self.assertEqual(assessed(event, block, code=code)["status"], "invalid")

    def test_missing_strict_block_event_is_invalid(self):
        self.assertEqual(assessed(summary(unimplemented=1), code=3)["status"], "invalid")

    def test_duplicate_and_unknown_block_events_are_invalid(self):
        block = {"kind": "startup_blocked", "reason": "unimplemented_imports"}
        self.assertEqual(assessed(summary(unimplemented=1), block, block, code=3)["status"], "invalid")
        self.assertEqual(assessed(None, {"kind": "startup_blocked", "reason": "other"})["status"], "invalid")

    def test_later_unimplemented_call_prevents_pass(self):
        event = {"kind": "unimplemented_call", "import": "synthetic.dll!Missing"}
        report = assessed(None, event)
        self.assertEqual(report["status"], "blocked")
        self.assertEqual(report["unimplemented_calls"], 1)

    def test_detach_callbacks_after_exit_are_valid(self):
        raw = encoded(summary(mode="init"), {"kind": "exit", "code": 0},
                      {"kind": "tls_callback", "reason": 0})
        self.assertEqual(br.assess(raw)["status"], "imports-resolved")

    def test_summary_or_block_after_exit_is_invalid(self):
        for event in (summary(), {"kind": "startup_blocked", "reason": "unimplemented_imports"}):
            raw = encoded({"kind": "exit", "code": 0}, event)
            self.assertEqual(br.assess(raw)["status"], "invalid")

    def test_unknown_events_and_fields_are_never_echoed(self):
        secret = "synthetic-private-marker"
        raw = encoded({"kind": "debug_string", "text": secret},
                      {"kind": "future_kind", "path": secret},
                      summary(path=secret, credentials={"value": secret}),
                      {"kind": "exit", "code": 0})
        report = br.assess(raw)
        self.assertEqual(report["status"], "imports-resolved")
        self.assertNotIn(secret, json.dumps(report))

    def test_file_and_line_limits_fail_closed(self):
        self.assertEqual(br.assess(b" " * (br.MAX_BYTES + 1))["status"], "invalid")
        raw = encoded({"kind": "debug_string", "text": "x" * br.MAX_LINE}, summary())
        self.assertEqual(br.assess(raw)["status"], "invalid")

    def test_invalid_encoding_objects_duplicate_keys_and_constants(self):
        for raw in (b"\xff", b"[]", b"null", b'{"kind":"exit","kind":"exit","code":0}',
                    b'{"kind":"unknown","value":NaN}', b'[' * 1200 + b']' * 1200):
            with self.subTest(raw=raw[:30]):
                self.assertEqual(br.assess(raw)["status"], "invalid")

    def test_bom_and_blank_lines_are_accepted(self):
        self.assertEqual(br.assess(b"\xef\xbb\xbf\n" + encoded(summary(), {"kind": "exit", "code": 0}))
                         ["status"], "imports-resolved")

    def test_cli_codes_and_output_do_not_expose_input_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "synthetic-private-marker.jsonl"
            for raw, expected in ((encoded(summary(), {"kind": "exit", "code": 0}), 0),
                                  (encoded(summary(strict=False, unimplemented=1), {"kind": "exit", "code": 0}), 1),
                                  (encoded(summary()), 2), (b"bad", 2)):
                path.write_bytes(raw)
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(br.main([str(path)]), expected)
                self.assertNotIn(str(path), output.getvalue())
                json.loads(output.getvalue())
            path.unlink()
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(br.main([str(path)]), 2)
            self.assertNotIn("synthetic-private-marker", output.getvalue())


if __name__ == "__main__":
    unittest.main()
