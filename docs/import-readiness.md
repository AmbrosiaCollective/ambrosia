# Initial import baseline

Use `gta6rt --dry-run synthetic.exe --strict-imports --trace boot.jsonl` to
reject unresolved initial imports without executing mapped image code. The
same flag works with `--init` and `--run`: rejection happens before mapped TLS
callbacks, DLL process attach, hooks or the executable entry point. Mapping
still allocates memory, installs TLS data and loads host DLLs to resolve imports;
this option is a diagnostic gate, not a sandbox for arbitrary executables.

Without the flag, the existing stub policy and exit behavior are unchanged.
Strict rejection returns **3**, regardless of `AMBROSIA_UNIMPLEMENTED`; the
per-call stub policy only applies when unresolved imports are actually called.
The gate covers normal and eagerly bound delay imports. Future dynamic module
loads and `GetProcAddress` requests are outside this initial snapshot.

When tracing is enabled, `binding_summary` records module count, binding counts
(`shim`, `host`, `module`, `unimplemented`), mode and strictness before mapped
initialisation. A strict rejection adds `startup_blocked` with reason
`unimplemented_imports`, followed by `exit` code 3. These events contain counts
and fixed labels; other startup events may contain private or title information.
Keep raw traces local.

Assess a trace without executing anything:

```text
python -I tools/boot_readiness.py boot.jsonl
```

The JSON report emits fixed labels and counts only. It never echoes event text,
symbols, module names, paths or OS exception details. Input is bounded to 4 MiB
and 64 KiB per nonblank line. Malformed/truncated JSON, duplicate keys, invalid
counts, contradictory strict-block evidence and repeated summary/exit events
cannot produce a pass. Unknown events are tolerated; detach callbacks may
legitimately follow the runtime's exit event.

| Status | Exit | Meaning |
|---|---|---|
| `imports-resolved` | 0 | Initial summary has zero unimplemented imports, no observed unimplemented calls and a recorded zero exit. |
| `blocked` | 1 | A complete trace records missing imports, a strict block, an unimplemented call or nonzero exit. |
| `incomplete` | 2 | No binding summary or no final exit was recorded, including older traces. |
| `invalid` | 2 | Unreadable, malformed, oversized or contradictory evidence. |

`imports-resolved` does not prove that shims or host bindings behave correctly.
It does not certify later imports, successful boot, audio, graphics or gameplay.
A clean dry run is only the first diagnostic milestone.

## Game-free checks

`python -I tests/test_boot_readiness.py` exercises original synthetic JSONL only.
Native Windows CTest `strict_imports` generates tiny PE files in a temporary
folder. It checks legacy dry-run behavior, strict rejection in all three modes,
normal/delay/ordinal imports and a resolved executable returning a sentinel.
The synthetic dependency's DllMain deliberately traps if entered: strict
rejection must avoid it. No game, GPU or external fixture download is needed.

The native regression requires a built Windows runtime and Python. Python-only
tests on another OS do not establish that the native gate or fixture runs.
