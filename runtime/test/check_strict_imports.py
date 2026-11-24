"""Native Windows regression: generated tiny PE images only, no game or GPU.

The DLL's entry is an intentional breakpoint: a strict rejection must never
initialise it. Child output stays local; the test prints only case labels.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def image(*, missing=False, delay=False, ordinal=False, dll=False):
    """Construct an original PE32+ image; no external compiler or binary fixture."""
    data = bytearray(0x600)

    def put(offset, fmt, *values):
        struct.pack_into("<" + fmt, data, offset, *values)

    data[:2] = b"MZ"
    put(0x3c, "I", 0x80)
    data[0x80:0x84] = b"PE\0\0"
    put(0x84, "HHIIIHH", 0x8664, 1, 0, 0, 0, 0xf0, 0x2022 if dll else 0x22)
    opt = 0x98
    put(opt, "H", 0x20b)
    put(opt + 16, "II", 0x1000, 0x1000)
    put(opt + 24, "Q", 0x180000000 if dll else 0x190000000)
    put(opt + 32, "II", 0x1000, 0x200)
    put(opt + 40, "HHHHHH", 6, 0, 0, 0, 6, 0)
    put(opt + 56, "II", 0x2000, 0x200)
    put(opt + 68, "HH", 3, 0)
    put(opt + 72, "QQQQ", 0x100000, 0x1000, 0x100000, 0x1000)
    put(opt + 108, "I", 16)
    sec = opt + 0xf0
    data[sec:sec + 8] = b".text\0\0\0"
    put(sec + 8, "IIII", 0x400, 0x1000, 0x400, 0x200)
    put(sec + 36, "I", 0xe0000060)
    code = b"\xcc\xb8\x01\0\0\0\xc3" if dll else b"\xb8\x25\0\0\0\xc3"
    data[0x200:0x200 + len(code)] = code
    if missing:
        # No export in fixture.dll, so the import is an unresolved thunk. The
        # module still maps, giving --init a real DllMain to avoid on rejection.
        if delay:
            put(opt + 112 + 13 * 8, "II", 0x1080, 64)
            put(0x280, "IIIIIIII", 1, 0x10c0, 0, 0x1120, 0x1100, 0, 0, 0)
        else:
            put(opt + 112 + 8, "II", 0x1080, 40)
            put(0x280, "IIIII", 0x1100, 0, 0, 0x10c0, 0x1120)
        data[0x2c0:0x2cc] = b"fixture.dll\0"
        thunk = (1 << 63) | 42 if ordinal else 0x1140
        put(0x300, "QQ", thunk, 0)
        put(0x320, "QQ", thunk, 0)
        data[0x342:0x34a] = b"Missing\0"
    return bytes(data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--loader", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("native Windows is required")
    # Do not show crash dialogs if a regression accidentally enters the DLL.
    ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    loader = args.loader.resolve(strict=True)
    environment = {k: v for k, v in os.environ.items() if not k.upper().startswith("AMBROSIA_")}
    environment["AMBROSIA_UNIMPLEMENTED"] = "stop"
    failures = []
    cases = [
        ("legacy-dry-run", True, False, False, "--dry-run", False, 0),
        ("strict-dry-run", True, False, False, "--dry-run", True, 3),
        ("strict-before-dllmain", True, False, False, "--init", True, 3),
        ("strict-before-entry", True, False, False, "--run", True, 3),
        ("strict-delay-import", True, True, False, "--init", True, 3),
        ("strict-ordinal-import", True, False, True, "--init", True, 3),
        ("resolved-dry-run", False, False, False, "--dry-run", True, 0),
        ("resolved-run", False, False, False, "--run", True, 37),
    ]
    with tempfile.TemporaryDirectory(prefix="strict_imports_", dir=args.work_dir.resolve()) as tmp:
        root = Path(tmp)
        (root / "fixture.dll").write_bytes(image(dll=True))
        for label, missing, delay, ordinal, mode, strict, expected in cases:
            guest, trace = root / "synthetic.exe", root / (label + ".jsonl")
            guest.write_bytes(image(missing=missing, delay=delay, ordinal=ordinal))
            command = [str(loader), mode, str(guest), "--game-dir", str(root),
                       "--mods", str(root / "no-mods"), "--trace", str(trace)]
            if strict:
                command.append("--strict-imports")
            try:
                result = subprocess.run(command, cwd=root, env=environment, capture_output=True, timeout=10)
                events = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
                summaries = [e for e in events if e.get("kind") == "binding_summary"]
                stopped = strict and missing
                assert result.returncode == expected
                assert len(summaries) == 1
                summary = summaries[0]
                assert summary["unimplemented"] == int(missing)
                assert summary["strict"] is strict and summary["mode"] == mode[2:]
                assert summary["modules"] == (2 if missing else 1)
                assert any(e.get("kind") == "startup_blocked" for e in events) == stopped
                assert any(e.get("kind") == "exit" and e.get("code") == expected for e in events)
                if stopped:
                    assert not any(e.get("kind") in ("tls_callback", "dllmain", "unimplemented_call") for e in events)
                print("PASS " + label)
            except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired):
                failures.append(label)
                print("FAIL " + label)
    print(f"strict import regression: {len(cases) - len(failures)}/{len(cases)} passed")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
