"""Ambrosia's offline developer commands: python -I ambrosia.py --help."""
import sys
from pathlib import Path

if sys.version_info < (3, 12):
    raise SystemExit("Ambrosia developer commands require Python 3.12 or newer.")

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent / "tools"))
from ambrosia_dev import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
