"""Shared terminal entry for native bootstrap builds and game readiness."""
from datetime import datetime
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parent.parent
GAME_UNAVAILABLE = (
    "Game launch is not available in this revision. Native module initialization "
    "is still being brought up; main game entry, graphics and menus are unverified."
)


class Console:
    def __init__(self, stream=None, *, plain=False):
        self.stream = sys.stdout if stream is None else stream
        self.color = (not plain and self.stream.isatty()
                      and "NO_COLOR" not in os.environ
                      and os.environ.get("TERM") != "dumb"
                      and (os.name != "nt" or "WT_SESSION" in os.environ
                           or "ANSICON" in os.environ))

    def write(self, message, tone="36"):
        # Child output and paths cannot inject terminal control sequences.
        clean = "".join(char if char.isprintable() else " " for char in message)
        if self.color:
            clean = "\033[" + tone + "m" + clean + "\033[0m"
        print(clean, file=self.stream, flush=True)

    def event(self, message, tone="36"):
        stamp = datetime.now().astimezone().strftime("%H:%M:%S")
        self.write("  [" + stamp + "] " + message, tone)

    def banner(self):
        self.write("  +----------------------------------------------------------+", "35")
        self.write("  |                         AMBROSIA                         |", "35")
        self.write("  |                 Build. Explore. Play.                    |", "35")
        self.write("  +----------------------------------------------------------+", "35")
        self.write("  Native runtime preview | game launch in development", "33")
        self.write("")


def prerequisites(root=ROOT):
    checks = []
    host = platform.system()
    machine = platform.machine().lower()
    checks.append((host in ("Linux", "Windows", "Darwin"),
                   "Host system", "Use Linux, Windows or macOS."))
    checks.append((machine in ("x86_64", "amd64"),
                   "x86-64 CPU", "The current native bootstrap requires x86-64; ARM is unsupported."))
    checks.append((shutil.which("cmake") is not None,
                   "CMake", "Provide CMake 3.24 or newer on PATH."))
    compilers = ("cl", "clang-cl") if host == "Windows" else ("c++", "g++", "clang++")
    checks.append((any(shutil.which(name) is not None for name in compilers),
                   "C++20 compiler", "Provide a C++20 compiler on PATH. On Windows use an x64 developer terminal."))
    checks.append(((root / "runtime/native/CMakeLists.txt").is_file(),
                   "Native source", "Restore the native source from the same Ambrosia revision."))
    return checks


def show_requirements(console, root=ROOT):
    checks = prerequisites(root)
    for ready, name, fix in checks:
        console.event(("FOUND  " if ready else "NEEDED ") + name, "32" if ready else "33")
        if not ready:
            console.write("             " + fix, "33")
    console.event("Discovery only; tool versions and builds have not been verified.")
    if platform.system() != "Linux":
        console.event("Windows/macOS bootstrap builds have not been validated here.", "33")
    return all(ready for ready, _, _ in checks)


def build_directory(root, requested):
    source = (root / "runtime/native").resolve()
    destination = (requested if requested is not None else root.parent / "Ambrosia-build").resolve()
    if destination.is_relative_to(root.resolve()) or root.resolve().is_relative_to(destination):
        raise ValueError("Choose a build directory outside the checkout and its parent directories.")
    if destination.exists():
        if not destination.is_dir():
            raise ValueError("The build destination must be a directory.")
        if any(destination.iterdir()):
            cache = destination / "CMakeCache.txt"
            marker = destination / ".ambrosia-native-source"
            if marker.is_symlink():
                raise ValueError("The build ownership marker must not be a symbolic link.")
            if cache.is_file() and cache.stat().st_size <= 1024 * 1024:
                entries = cache.read_text(encoding="utf-8", errors="replace").splitlines()
                prefix = "CMAKE_HOME_DIRECTORY:INTERNAL="
                homes = [line[len(prefix):] for line in entries if line.startswith(prefix)]
            elif not cache.exists() and marker.is_file() and marker.stat().st_size <= 8192:
                homes = [marker.read_text(encoding="utf-8").strip()]
            else:
                raise ValueError("Use an empty directory or this native bootstrap's existing CMake build.")
            if len(homes) != 1 or Path(homes[0]).resolve() != source:
                raise ValueError("This build directory belongs to different sources; choose another directory.")
    return destination


def build_commands(root, destination, jobs):
    return [
        ["cmake", "-S", str(root / "runtime/native"), "-B", str(destination),
         "-DBUILD_TESTING=OFF", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"],
        ["cmake", "--build", str(destination), "--config", "RelWithDebInfo",
         "--parallel", str(jobs), "--target", "ambrosia-host"],
    ]


def run_stage(command, root, console, log):
    with subprocess.Popen(command, cwd=root, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, encoding="utf-8", errors="replace") as child:
        try:
            for line in child.stdout:
                log.write(line)
                log.flush()
                console.event(line.rstrip("\r\n"), "90")
            return child.wait()
        except BaseException:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
            raise


def build(args, console, root=ROOT):
    try:
        destination = build_directory(root, args.build_dir)
        commands = build_commands(root, destination, args.jobs)
        if args.dry_run:
            console.event("Build preview: no programs executed or directories created.")
            for command in commands:
                console.write("  " + subprocess.list2cmdline(command))
            console.event("Build scope: native CPU bootstrap only; game preparation is unfinished.", "33")
            return 0
        if not show_requirements(console, root):
            return 1
        destination.mkdir(parents=True, exist_ok=True)
        marker = destination / ".ambrosia-native-source"
        if not marker.exists():
            with marker.open("x", encoding="utf-8", newline="\n") as stream:
                stream.write(str((root / "runtime/native").resolve()) + "\n")
        name = "ambrosia-build-" + datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8] + ".log"
        log_path = destination / name
        console.event("Building the native CPU bootstrap. No game files are needed.")
        console.event("Local build log: " + str(log_path))
        with log_path.open("x", encoding="utf-8", newline="\n") as log:
            for label, command in zip(("Configure", "Compile"), commands):
                console.event(label + " started")
                code = run_stage(command, root, console, log)
                if code:
                    console.event(label + " failed. Review the local build log.", "31")
                    return code if code > 0 else 1
        console.event("Native bootstrap build completed. Game launch remains unavailable.", "32")
        return 0
    except ValueError as error:
        console.event(str(error), "31")
        return 1
    except (OSError, RuntimeError):
        console.event("Build could not proceed. Check tools, permissions and the build directory.", "31")
        console.write("  Use an empty external --build-dir, or reuse this source's CMake build.")
        return 1


def launch(console):
    console.event(GAME_UNAVAILABLE, "33")
    console.write("  Next step: use the requirements check or build the native CPU bootstrap.")
    return 1


def menu(args, console):
    if not sys.stdin.isatty() or not console.stream.isatty():
        console.event("The menu needs an interactive terminal. Use build, launch or doctor directly.", "33")
        return 2
    while True:
        console.banner()
        console.write("  1  Check native build requirements")
        console.write("  2  Build native CPU bootstrap")
        console.write("  3  Launch game [unavailable in this revision]", "33")
        console.write("  0  Exit")
        console.write("")
        try:
            choice = input("  Choose an action > ").strip()
        except EOFError:
            return 0
        if choice == "0":
            return 0
        if choice == "1":
            show_requirements(console)
        elif choice == "2":
            code = build(args, console)
            console.event("Build action finished with exit code " + str(code))
        elif choice == "3":
            launch(console)
        else:
            console.event("Choose 0, 1, 2 or 3.", "33")
        console.write("")


def add_commands(commands):
    for name, help_text in (
            ("menu", "Open the interactive native build and readiness menu"),
            ("build", "Build the native CPU bootstrap; no game preparation or launch"),
            ("launch", "Check game launch availability; currently returns unavailable")):
        parser = commands.add_parser(name, help=help_text)
        parser.add_argument("--plain", action="store_true", help="Disable terminal colors")
        if name != "launch":
            parser.add_argument("--build-dir", type=Path, help="External CMake build directory")
            parser.add_argument("--jobs", type=int, choices=range(1, 65), default=2, metavar="1..64")
            if name == "build":
                parser.add_argument("--dry-run", action="store_true", help="Preview build commands without executing tools")
            else:
                parser.set_defaults(dry_run=False)


def dispatch(args):
    console = Console(plain=args.plain)
    try:
        if args.command == "build":
            return build(args, console)
        if args.command == "launch":
            return launch(console)
        return menu(args, console)
    except KeyboardInterrupt:
        console.event("Cancelled.", "33")
        return 130
