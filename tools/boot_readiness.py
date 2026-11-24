"""Assess initial import resolution from a startup JSONL trace, without running images.

Output contains fixed labels and counts only. Raw traces can contain private paths
and title data: keep them local. A resolved initial graph does not establish shim
correctness, later dynamic imports, rendering or gameplay. Standard library only.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

MAX_BYTES = 4 * 1024 * 1024
MAX_LINE = 64 * 1024
COUNTS = ("modules", "shim", "host", "module", "unimplemented")
LIMITATION = ("Initial binding snapshot only; host/shim bindings do not prove compatible behavior. "
              "Dynamic imports, boot, graphics and gameplay are not certified.")


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate key")
        result[key] = value
    return result


def _constant(_):
    raise ValueError("non-finite constant")


def assess(raw: bytes) -> dict:
    """Fail closed on malformed, incomplete or contradictory diagnostic evidence."""
    report = {"schema": "ambrosia.boot-readiness/1", "status": "incomplete",
              "initial_bindings": None, "mode": None, "strict": None,
              "exit_code": None, "unimplemented_calls": 0, "issues": [],
              "limitations": LIMITATION}
    issues = set()
    summary = None
    exited = False
    blocked = False
    invalid = False
    if len(raw) > MAX_BYTES:
        issues.add("input-too-large")
        invalid = True
    else:
        try:
            text = raw.decode("utf-8-sig")
            for line in text.splitlines():
                if not line.strip():
                    continue
                if len(line.encode("utf-8")) > MAX_LINE:
                    raise ValueError("line too large")
                event = json.loads(line, object_pairs_hook=_object, parse_constant=_constant)
                if not isinstance(event, dict) or not isinstance(event.get("kind"), str):
                    raise ValueError("invalid event")
                kind = event["kind"]
                if kind == "binding_summary":
                    if exited or summary is not None or any(type(event.get(k)) is not int
                                                  or not 0 <= event[k] <= 2**31 - 1 for k in COUNTS):
                        raise ValueError("invalid summary")
                    if (event["modules"] == 0 or type(event.get("strict")) is not bool
                            or event.get("mode") not in ("dry-run", "init", "run")):
                        raise ValueError("invalid summary fields")
                    summary = {k: event[k] for k in COUNTS}
                    report.update(initial_bindings=summary, mode=event["mode"], strict=event["strict"])
                elif kind == "startup_blocked":
                    if exited or blocked or summary is None or event.get("reason") != "unimplemented_imports":
                        raise ValueError("invalid block event")
                    blocked = True
                elif kind == "unimplemented_call":
                    report["unimplemented_calls"] += 1
                elif kind == "exit":
                    if exited or type(event.get("code")) is not int or not 0 <= event["code"] <= 2**32 - 1:
                        raise ValueError("invalid exit")
                    report["exit_code"] = event["code"]
                    exited = True
                # Loader teardown can legitimately emit detach callbacks after the exit event.
        except (UnicodeError, ValueError, RecursionError):
            issues.add("malformed-trace")
            invalid = True
    if summary is None:
        issues.add("missing-binding-summary")
    elif summary["unimplemented"]:
        issues.add("unimplemented-initial-imports")
    if not exited:
        issues.add("missing-exit")
    elif report["exit_code"] != 0:
        issues.add("nonzero-exit")
    if report["unimplemented_calls"]:
        issues.add("unimplemented-call")
    if blocked:
        issues.add("startup-blocked")
        if (not report["strict"] or not summary["unimplemented"]
                or report["exit_code"] not in (None, 3)):
            issues.add("contradictory-block")
            invalid = True
    if summary and report["strict"] and summary["unimplemented"] and exited and not blocked:
        issues.add("missing-strict-block")
        invalid = True
    report["issues"] = sorted(issues)
    if invalid:
        report["status"] = "invalid"
    elif not summary or not exited:
        report["status"] = "incomplete"
    elif issues:
        report["status"] = "blocked"
    else:
        report["status"] = "imports-resolved"
    return report


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="Local UTF-8 startup JSONL; never uploaded")
    args = parser.parse_args(argv)
    try:
        with args.trace.open("rb") as stream:
            report = assess(stream.read(MAX_BYTES + 1))
    except OSError:
        # Never serialize a path, OS exception text, environment value or raw event.
        report = {"schema": "ambrosia.boot-readiness/1", "status": "invalid",
                  "issues": ["input-unreadable"], "limitations": LIMITATION}
    print(json.dumps(report, indent=2, sort_keys=True))
    return {"imports-resolved": 0, "blocked": 1, "incomplete": 2, "invalid": 2}[report["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
