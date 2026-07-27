#!/usr/bin/env python3
"""Locate the KiCad toolchain so the rest of the KiCad tooling never hardcodes paths.

KiCad 10.0.5 installs to C:\\Program Files\\KiCad\\10.0\\bin\\ on this workstation
and does NOT amend PATH, so `kicad-cli` is not callable by bare name. Every other
tools/kicad_*.py script resolves it through here instead of repeating the path.

The WSL trap this module exists to prevent
------------------------------------------
Under WSL, /mnt/c/Program\\ Files/KiCad/10.0/bin/kicad-cli.exe *is* executable via
Windows interop -- it launches and prints a version, which makes it look like a
working fallback. It is not. That binary resolves file arguments as Windows paths,
so a WSL-side path like /home/kevin/board3.kicad_pcb is either not found or written
somewhere unexpected. A Windows kicad-cli reached from WSL fails silently and late.

So: on WSL we search Linux install locations ONLY, and report a clear absence if
KiCad is not installed inside the distro. Never hand back a /mnt/c path.

Run standalone to see what resolved:

  python tools/kicad_env.py
  wsl -- python3 tools/kicad_env.py
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

# Where KiCad lands per host, newest-first once globbed. Version directories are
# globbed rather than pinned so a 10.1 or 11.0 upgrade doesn't silently break
# resolution -- the conversion tooling cares that a kicad-cli exists, and the
# caller decides whether the version it reports is acceptable.
WINDOWS_ROOTS = (
    Path("C:/Program Files/KiCad"),
    Path("C:/Program Files (x86)/KiCad"),
)
LINUX_CLI_CANDIDATES = (
    Path("/usr/bin/kicad-cli"),
    Path("/usr/local/bin/kicad-cli"),
    Path("/var/lib/flatpak/exports/bin/org.kicad.KiCad"),
)
DARWIN_CLI_CANDIDATES = (
    Path("/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"),
)


class KiCadNotFound(RuntimeError):
    """KiCad could not be located. Carries an actionable message, not a traceback."""


def detect_host() -> str:
    """Return 'windows', 'wsl', 'linux', or 'darwin'.

    WSL is distinguished from plain Linux because it changes which install we may
    use (see the module docstring) -- everything else treats them the same.
    """
    system = platform.system()
    if system == "Windows":
        return "windows"
    if system == "Darwin":
        return "darwin"
    if system == "Linux":
        if os.environ.get("WSL_DISTRO_NAME"):
            return "wsl"
        try:
            if "microsoft" in Path("/proc/version").read_text().lower():
                return "wsl"
        except OSError:
            pass
        return "linux"
    return system.lower()


def _windows_candidates() -> list[Path]:
    """Every kicad-cli.exe under a versioned KiCad install, newest version first."""
    found: list[Path] = []
    for root in WINDOWS_ROOTS:
        if not root.is_dir():
            continue
        for version_dir in sorted(root.iterdir(), reverse=True):
            candidate = version_dir / "bin" / "kicad-cli.exe"
            if candidate.is_file():
                found.append(candidate)
    return found


def _install_hint(host: str) -> str:
    if host == "windows":
        return (
            "Expected kicad-cli.exe under C:\\Program Files\\KiCad\\<version>\\bin\\. "
            "Install KiCad 10 from https://www.kicad.org/download/ ."
        )
    if host == "wsl":
        return (
            "KiCad is not installed inside this WSL distro. A Windows KiCad reached "
            "through /mnt/c is deliberately NOT used -- it mis-resolves WSL file "
            "paths. Install inside the distro: sudo apt install kicad ."
        )
    if host == "darwin":
        return "Expected kicad-cli at /Applications/KiCad/KiCad.app/Contents/MacOS/."
    return "Install KiCad 10 and ensure kicad-cli is on PATH."


def find_kicad_cli() -> Path:
    """Absolute path to a usable kicad-cli, or raise KiCadNotFound with a fix.

    KICAD_CLI overrides everything -- an explicit path from the operator wins over
    discovery, including on WSL where discovery is deliberately narrow.
    """
    override = os.environ.get("KICAD_CLI")
    if override:
        path = Path(override)
        if not path.is_file():
            raise KiCadNotFound(f"KICAD_CLI is set to {override!r}, which is not a file.")
        return path

    host = detect_host()
    if host == "windows":
        candidates = _windows_candidates()
    elif host == "darwin":
        candidates = [p for p in DARWIN_CLI_CANDIDATES if p.is_file()]
    else:
        # Covers 'wsl' and 'linux'. WSL intentionally shares the Linux list and
        # never falls through to /mnt/c -- see the module docstring.
        candidates = [p for p in LINUX_CLI_CANDIDATES if p.is_file()]
        on_path = shutil.which("kicad-cli")
        if on_path:
            candidates.insert(0, Path(on_path))

    if not candidates:
        raise KiCadNotFound(f"kicad-cli not found on host {host!r}. {_install_hint(host)}")
    return candidates[0]


def find_kicad_python() -> Path:
    """Interpreter that can `import pcbnew` -- what epro2kicad drives conversion through.

    On Windows that is KiCad's own bundled python.exe beside kicad-cli. On Linux and
    WSL, KiCad installs pcbnew into the system site-packages instead of shipping a
    private interpreter, so the current interpreter is the right answer.
    """
    if detect_host() == "windows":
        cli = find_kicad_cli()
        bundled = cli.parent / "python.exe"
        if not bundled.is_file():
            raise KiCadNotFound(f"KiCad's bundled python.exe not found beside {cli}.")
        return bundled
    return Path(sys.executable)


def run_kicad_cli(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    """Invoke kicad-cli with the resolved absolute path."""
    return subprocess.run(
        [str(find_kicad_cli()), *args],
        capture_output=True,
        text=True,
        check=check,
    )


def kicad_version() -> str:
    return run_kicad_cli("--version").stdout.strip()


def main() -> int:
    print(f"host:          {detect_host()}")
    try:
        cli = find_kicad_cli()
    except KiCadNotFound as exc:
        # Flush stdout first: it is block-buffered when piped while stderr is not,
        # so without this the host line lands after the error and the failure
        # output reads out of order in exactly the case you need it legible.
        sys.stdout.flush()
        print(f"kicad-cli:     NOT FOUND\n\n{exc}", file=sys.stderr)
        return 1
    print(f"kicad-cli:     {cli}")
    print(f"version:       {kicad_version()}")
    try:
        print(f"pcbnew python: {find_kicad_python()}")
    except KiCadNotFound as exc:
        print(f"pcbnew python: NOT FOUND ({exc})")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
