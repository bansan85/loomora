"""Shared helpers for the Loomora host tools (Python 3.8+)."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / "core"
DOCS = ROOT / "docs" / "implementation"  # git submodule ../loomora-docs (implementation documentation)
VALIDATION = DOCS / "validation"
ADR = DOCS / "adr"
CONTRACTS = ROOT / "specs" / "001-loomora-cross-platform" / "contracts"
VECTORS = CORE / "tests" / "vectors.json"


def setup_utf8_output():
    """Print UTF-8 on every console (the Windows cp1252 console would raise or garble)."""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)  # Python 3.7+
        if reconfigure is not None:
            try:
                reconfigure(encoding="utf-8", errors="replace")
            except (ValueError, OSError):
                pass


def docs_present():
    """True when the docs submodule is checked out (docs/implementation/validation exists)."""
    return VALIDATION.is_dir()


def docs_missing_message(what):
    """Message printed when the docs submodule is not checked out: what is skipped and how to fetch it."""
    return ("docs/implementation (git submodule ../loomora-docs) is not checked out: %s. "
            "Fetch it with: git -c protocol.file.allow=always submodule update --init "
            "docs/implementation" % what)


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, cwd=None, check=True, capture=False, timeout=None):
    """Run a command; returns CompletedProcess. Exits with its message on failure if check."""
    res = subprocess.run(
        [str(c) for c in cmd],
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
        universal_newlines=True,
        timeout=timeout,
    )
    if check and res.returncode != 0:
        if capture and res.stdout:
            sys.stderr.write(res.stdout)
        sys.exit("command failed (%d): %s" % (res.returncode, " ".join(str(c) for c in cmd)))
    return res
