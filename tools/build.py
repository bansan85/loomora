#!/usr/bin/env python
"""Build one Loomora deliverable and record it.

Usage:
  python tools/build.py <platform> <variant> [--wsl-distro NAME]

  platform  windows | macos | ios | android | linux | web
  variant   legacy | modern

A target builds the UI of that platform together with the C++11 backend, copies the artifact to
out/<platform>-<variant>/ and writes docs/implementation/validation/deliverables/<platform-id>-<variant>.json (the
Deliverable of data-model.md). A target that is not written yet fails with
"not implemented yet (Txxx)".
The records live in the docs submodule: when docs/implementation is not checked out, the build
still runs, a warning is printed and no record is written (the product build never needs it).

Implemented targets:
  linux modern   meson + ninja on ui/modern/linux-gtk3 (T058). On a Windows host the build runs
                 inside WSL (distribution --wsl-distro, or $LOOMORA_WSL_DISTRO, default
                 Ubuntu-24.04): the tool re-runs itself there. Needs meson, ninja, a C and a C++
                 compiler and the gtk+-3.0 development files.

Platform id of a Linux deliverable: "linux-<os-release ID>-<VERSION_ID>-<machine>" such as
"linux-ubuntu-24.04-x86_64", the same rule as ui/modern/linux-gtk3/probe.c uses for the probe
record's platform_id (keep both in step).
"""
import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

import common

PLATFORMS = ("windows", "macos", "ios", "android", "linux", "web")
VARIANTS = ("legacy", "modern")
DEFAULT_WSL_DISTRO = "Ubuntu-24.04"

# Targets that do not exist yet and the task that will add each of them.
PENDING = {
    ("web", "modern"): "T040",
    ("windows", "modern"): "T046",
    ("macos", "modern"): "T049",
    ("ios", "modern"): "T052",
    ("android", "modern"): "T055",
    ("windows", "legacy"): "T075",
    ("macos", "legacy"): "T077",
    ("ios", "legacy"): "T079",
    ("android", "legacy"): "T082",
    ("linux", "legacy"): "T084",
    ("web", "legacy"): "T086",
}


def capture(cmd, cwd=None):
    """Run a command and return its standard output (stripped); fails with a clear message."""
    res = subprocess.run(
        [str(c) for c in cmd],
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        universal_newlines=True,
    )
    if res.returncode != 0:
        sys.exit("command failed (%d): %s\n%s" % (res.returncode, " ".join(map(str, cmd)), res.stderr))
    return res.stdout.strip()


def require_tools(names):
    missing = [n for n in names if shutil.which(n) is None]
    if missing:
        sys.exit("missing tool(s) on this machine: %s" % ", ".join(missing))


def id_part(text):
    """One platform-id component: lowercase, anything outside [a-z0-9._-] becomes '-'."""
    return re.sub(r"[^a-z0-9._-]", "-", text.lower())


def linux_platform_id():
    values = {}
    for path in ("/etc/os-release", "/usr/lib/os-release"):
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                for line in f:
                    key, sep, value = line.strip().partition("=")
                    if sep:
                        values[key] = value.strip("\"' ")
            break
    parts = ["linux"]
    for key in ("ID", "VERSION_ID"):
        if values.get(key):
            parts.append(id_part(values[key]))
    parts.append(id_part(platform.machine()))
    return "-".join(parts)


# ---- linux modern (T058) -------------------------------------------------------------------

def build_linux_modern():
    """Native Linux build of ui/modern/linux-gtk3; returns the Deliverable record."""
    require_tools(["meson", "ninja", "pkg-config", "gcc", "g++"])
    src = common.ROOT / "ui" / "modern" / "linux-gtk3"
    build_dir = Path(os.environ.get("LOOMORA_BUILD_DIR") or "/tmp") / "loomora-linux-modern"
    if (build_dir / "build.ninja").exists():
        common.run(["meson", "setup", "--reconfigure", build_dir, src])
    else:
        common.run(["meson", "setup", build_dir, src])
    common.run(["ninja", "-C", build_dir])

    exe = build_dir / "loomora-gtk3"
    if not exe.is_file():
        sys.exit("build finished but %s is missing" % exe)
    out_dir = common.ROOT / "out" / "linux-modern"
    out_dir.mkdir(parents=True, exist_ok=True)
    artifact = out_dir / exe.name
    shutil.copyfile(str(exe), str(artifact))
    os.chmod(str(artifact), 0o755)

    compilers = json.loads(capture(["meson", "introspect", build_dir, "--compilers"]))["host"]
    cpp, cc = compilers["cpp"], compilers["c"]
    cpp_id = "%s-%s" % (cpp["id"], cpp["version"])
    cc_id = "%s-%s" % (cc["id"], cc["version"])
    cxx = cpp["exelist"][0]
    gtk = capture(["pkg-config", "--modversion", "gtk+-3.0"])

    record = {
        "platform_id": linux_platform_id(),
        "ui_variant": "modern",
        "cpp_std": "c++11",
        "compiler": cpp_id,
        "target": capture([cxx, "-dumpmachine"]),
        "output": "native",
        "ui_language": "c",
        "ui_toolkit": "gtk+-3.0 %s (3.10 API subset)" % gtk,
        "ui_compiler": cc_id,
        "artifact": {"name": artifact.name, "size": artifact.stat().st_size},
        "disabled_features": [],
        "fallbacks": [],
        "notes": "meson + ninja (ui/modern/linux-gtk3/meson.build); static libloomora.a built "
                 "from core/src with -std=c++11; libstdc++ and libgcc linked statically, GTK 3 "
                 "and the system libraries dynamically; artifact copied to out/linux-modern/.",
    }
    return record, artifact


def in_wsl_or_linux():
    return sys.platform.startswith("linux")


def run_linux_modern(args):
    if in_wsl_or_linux():
        record, artifact = build_linux_modern()
    elif sys.platform == "win32":
        # Re-run this tool inside WSL; the repository is reached through its Windows path.
        distro = args.wsl_distro or os.environ.get("LOOMORA_WSL_DISTRO") or DEFAULT_WSL_DISTRO
        require_tools(["wsl"])
        res = subprocess.run(
            ["wsl", "-d", distro, "--cd", str(common.ROOT), "--",
             "python3", "tools/build.py", "linux", "modern"])
        if res.returncode != 0:
            sys.exit("the build inside WSL (%s) failed with status %d" % (distro, res.returncode))
        return  # the inner run has written the deliverable record
    else:
        sys.exit("linux modern cannot be built on this host (%s): use Linux or Windows with WSL"
                 % sys.platform)
    write_deliverable(record, artifact)


def write_deliverable(record, artifact):
    name = "%s-%s.json" % (record["platform_id"], record["ui_variant"])
    path = common.VALIDATION / "deliverables" / name
    print("artifact: %s (%d bytes)" % (artifact.relative_to(common.ROOT).as_posix(),
                                       record["artifact"]["size"]))
    if not common.docs_present():
        print("WARNING: deliverable record %s NOT written: %s"
              % (name, common.docs_missing_message("the record is skipped, the build itself is complete")))
        return
    common.write_json(path, record)
    print("deliverable record: %s" % path.relative_to(common.ROOT).as_posix())


TARGETS = {("linux", "modern"): run_linux_modern}


def main(argv=None):
    common.setup_utf8_output()
    parser = argparse.ArgumentParser(description="Build one Loomora deliverable.")
    parser.add_argument("platform", choices=PLATFORMS)
    parser.add_argument("variant", choices=VARIANTS)
    parser.add_argument("--wsl-distro", help="WSL distribution used on a Windows host "
                        "(default $LOOMORA_WSL_DISTRO or %s)" % DEFAULT_WSL_DISTRO)
    args = parser.parse_args(argv)

    key = (args.platform, args.variant)
    if key in TARGETS:
        TARGETS[key](args)
        return 0
    task = PENDING.get(key, "no task")
    sys.exit("build target %s-%s: not implemented yet (%s)" % (args.platform, args.variant, task))


if __name__ == "__main__":
    sys.exit(main())
