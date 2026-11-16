#!/usr/bin/env python3
"""Run the macOS virtual machines in VMware Workstation Pro with the DrDonk Unlocker (ADR 0019 fallback).

Run it on the Windows host (python tools/macvmw.py ...). Nothing here downloads: the owner downloads
VMware Workstation Pro (Broadcom portal, login) and the Unlocker release zip, and puts them in the folder below.

    macvmw.py check                read-only: VMware, the Unlocker zip (sha256), the hypervisor, the patch status
    macvmw.py plan                 what is done and in which order (no change)
    macvmw.py unpack               verify the Unlocker zip against its pinned sha256, then extract it to <dir>/unlocker
    macvmw.py unlock | relock      apply or remove the Unlocker patches (elevated PowerShell, VMware closed)
    macvmw.py vmx NAME [--install IMG] [--disk-gb N] [--ram-mb N] [--cpus N] [--guest-os ID]
                                   create <dir>/vms/NAME with its disk, the install image as a disk, and NAME.vmx
    macvmw.py --selftest

The folder is $LOOMORA_VMW_DIR (default: %USERPROFILE%\\loomora-macosvm). Exit status: 0 success, 1 a failed
check, 2 a usage or setup error. Python 3.8+, standard library only (ADR 0015). None of the VMware steps was
run on this machine by an agent: see docs/implementation/validation/procedures/macos.md, section M.9.
"""
import argparse
import hashlib
import os
import re
import shlex
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import setup_utf8_output  # noqa: E402

# DrDonk/unlocker v4.2.8, published 2026-05-14; the sha256 is the digest GitHub shows for the asset
# (API read 2026-11-16) and matched the file the owner downloaded the same day.
UNLOCKER_ZIP = "unlocker428.zip"
UNLOCKER_SHA256 = "7ec212696981a95f7bbf9b04b7d33e644a8a808c88a39db897b16487be456af4"
UNLOCKER_URL = "https://github.com/DrDonk/unlocker/releases/download/v4.2.8/unlocker428.zip"
VMWARE_DIR = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "VMware" / "VMware Workstation"
VMWARE_PROCESSES = ("vmware.exe", "vmware-vmx.exe", "vmplayer.exe")  # the tray icon may stay


class SetupError(Exception):
    """Something to install or supply first; the message says what."""


def root():
    return Path(os.environ.get("LOOMORA_VMW_DIR") or (Path.home() / "loomora-macosvm"))


def sha256_of(path):
    h = hashlib.sha256()
    with open(str(path), "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, **kw)


def powershell(text):
    return run(["powershell", "-NoProfile", "-Command", text]).stdout.strip()


def is_admin():
    try:
        import ctypes
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except (ImportError, AttributeError, OSError):
        return False


def vmware_processes():
    out = run(["tasklist", "/fo", "csv", "/nh"]).stdout.lower()
    return sorted({p for p in VMWARE_PROCESSES if '"%s"' % p in out})


def safe_extract(zip_path, dest):
    """Extract a zip, refusing any member that would land outside dest."""
    dest = Path(dest).resolve()
    with zipfile.ZipFile(str(zip_path)) as z:
        for name in z.namelist():
            target = (dest / name).resolve()
            if dest != target and dest not in target.parents:
                raise SetupError("refusing %r: it would be extracted outside %s" % (name, dest))
        z.extractall(str(dest))


def build_vmx(name, guest_os, ram_mb, cpus, disks):
    """The .vmx of a macOS guest, as text. Pure. `disks` is a list of file names on the SATA controller.

    Lines are the minimum known for a macOS guest on VMware (EFI, the SMC of the Unlocker, SATA disks, e1000e);
    this file was not booted by an agent."""
    lines = [
        '.encoding = "UTF-8"', 'config.version = "8"', 'virtualHW.version = "21"',
        'displayName = "%s"' % name, 'guestOS = "%s"' % guest_os, 'firmware = "efi"',
        'memsize = "%d"' % ram_mb, 'numvcpus = "%d"' % cpus,
        'smc.present = "TRUE"', 'smc.version = "0"',
        'sata0.present = "TRUE"',
    ]
    for i, f in enumerate(disks):
        lines += ['sata0:%d.present = "TRUE"' % i, 'sata0:%d.fileName = "%s"' % (i, f)]
    lines += [
        'ethernet0.present = "TRUE"', 'ethernet0.connectionType = "nat"', 'ethernet0.virtualDev = "e1000e"',
        'usb.present = "TRUE"', 'ehci.present = "TRUE"', 'usb_xhci.present = "TRUE"',
        'svga.present = "TRUE"', 'svga.vramSize = "134217728"', 'mks.enable3d = "FALSE"',
    ]
    return "\n".join(lines) + "\n"


def flat_descriptor(image, size_bytes):
    """A VMDK descriptor that exposes a raw image as a disk (a flat extent), so no conversion is needed."""
    sectors = size_bytes // 512
    cyl = max(1, sectors // (255 * 63))
    return ('# Disk DescriptorFile\nversion=1\nencoding="UTF-8"\nCID=fffffffe\nparentCID=ffffffff\n'
            'createType="monolithicFlat"\n\n# Extent description\nRW %d FLAT "%s" 0\n\n# The Disk Data Base\n#DDB\n'
            'ddb.virtualHWVersion = "21"\nddb.geometry.cylinders = "%d"\nddb.geometry.heads = "255"\n'
            'ddb.geometry.sectors = "63"\nddb.adapterType = "lsilogic"\n' % (sectors, image, cyl))


def unlocker_exe(name):
    exe = root() / "unlocker" / "windows" / name
    if not exe.exists():
        raise SetupError("%s not found: run `macvmw.py unpack` first" % exe)
    return exe


def cmd_check(_args):
    results = []

    def line(ok, name, detail):
        results.append(ok)
        print("%-4s %-18s %s" % ("PASS" if ok else "FAIL", name, detail))

    line(os.name == "nt", "windows host", sys.platform)
    exe = VMWARE_DIR / "vmware.exe"
    ver = powershell("(Get-Item '%s').VersionInfo.ProductVersion" % exe) if exe.exists() else ""
    line(exe.exists(), "vmware", "%s %s" % (exe, ver) if exe.exists() else "not installed in %s" % VMWARE_DIR)
    vd = VMWARE_DIR / "vmware-vdiskmanager.exe"
    line(vd.exists(), "vdiskmanager", str(vd) if vd.exists() else "missing")
    zp = root() / UNLOCKER_ZIP
    if zp.exists():
        got = sha256_of(zp)
        line(got == UNLOCKER_SHA256, "unlocker zip", "sha256 %s the pinned value" % ("matches" if got == UNLOCKER_SHA256 else "DIFFERS from (%s)" % got))
    else:
        line(False, "unlocker zip", "%s not found (download %s and keep the file name)" % (zp, UNLOCKER_URL))
    line((root() / "unlocker" / "windows" / "check.exe").exists(), "unlocker unpacked", str(root() / "unlocker"))
    running = vmware_processes()
    line(not running, "vmware closed", ", ".join(running) if running else "no VMware process")
    hv = powershell("(Get-CimInstance Win32_ComputerSystem).HypervisorPresent")
    print("INFO %-18s %s" % ("hypervisor", "present (Hyper-V / WSL2 / VBS): VMware runs on the Windows Hypervisor Platform; "
                                          "a macOS guest in that mode is NOT verified" if hv.lower() == "true" else hv or "unknown"))
    print("INFO %-18s %s" % ("admin", "yes" if is_admin() else "no (needed by unlock and relock)"))
    chk = root() / "unlocker" / "windows" / "check.exe"
    if chk.exists() and not is_admin():
        print("INFO patch status: not read (check.exe needs an elevated PowerShell)")
    elif chk.exists():
        print("INFO patch status (check.exe):")
        print("\n".join("     " + l for l in run([str(chk)]).stdout.splitlines()))
    return 0 if all(results) else 1


def cmd_plan(_args):
    print("""Folder: %s
 1. (owner) VMware Workstation Pro installed, the Unlocker zip %s in the folder           -> `check`
 2. `unpack`            verify the zip's sha256, extract to <dir>/unlocker
 3. close VMware, then in an elevated PowerShell: `unlock`   (patches vmware-vmx and vmwarebase.dll;
                        re-run after every VMware update; `relock` undoes it)
 4. get the recovery image: in WSL, LOOMORA_VM_DIR=/mnt/c/.../dl python3 tools/macvm.py fetch tahoe --yes-download
                        (about 1 GB; writes install.img)
 5. `vmx NAME --install <install.img>`   creates the VM folder, an 80 GB growable disk, the .vmx
 6. open the .vmx in VMware, install macOS (docs/implementation/validation/procedures/macos.md, M.9)
Not verified by an agent: every step from 3 on, and macOS booting while Hyper-V / WSL2 is on.""" % (root(), UNLOCKER_ZIP))
    return 0


def cmd_unpack(_args):
    zp, dest = root() / UNLOCKER_ZIP, root() / "unlocker"
    if not zp.exists():
        raise SetupError("%s not found" % zp)
    got = sha256_of(zp)
    if got != UNLOCKER_SHA256:
        raise SetupError("sha256 of %s is %s, expected %s: not extracting" % (zp, got, UNLOCKER_SHA256))
    if dest.exists() and any(dest.iterdir()):
        raise SetupError("%s is not empty; refusing to extract over it" % dest)
    dest.mkdir(parents=True, exist_ok=True)
    safe_extract(zp, dest)
    print("extracted to %s (nothing was run)" % dest)
    return 0


def patch(tool):
    if not is_admin():
        raise SetupError("%s patches files under %s: run this in an elevated PowerShell" % (tool, VMWARE_DIR))
    running = vmware_processes()
    if running:
        raise SetupError("close VMware first (running: %s)" % ", ".join(running))
    exe = unlocker_exe(tool + ".exe")
    print("+ " + str(exe), flush=True)
    return subprocess.call([str(exe)], cwd=str(exe.parent))


def cmd_vmx(args):
    if not re.match(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$", args.name):
        raise SetupError("VM name %r: letters, digits, '_', '-', '.' only" % args.name)
    folder = root() / "vms" / args.name
    if folder.exists():
        raise SetupError("%s already exists; refusing to overwrite" % folder)
    vd = VMWARE_DIR / "vmware-vdiskmanager.exe"
    if not vd.exists():
        raise SetupError("%s not found: VMware Workstation is not installed" % vd)
    install = None
    if args.install:
        install = Path(args.install).resolve()
        if not install.is_file():
            raise SetupError("install image %s not found" % install)
    folder.mkdir(parents=True)
    disks = ["%s.vmdk" % args.name]
    r = run([str(vd), "-c", "-s", "%dGB" % args.disk_gb, "-a", "lsilogic", "-t", "0", str(folder / disks[0])])
    print(r.stdout.strip())
    if r.returncode != 0 or not (folder / disks[0]).exists():
        raise SetupError("vmware-vdiskmanager failed (status %d)" % r.returncode)
    if install:
        # VMware refuses an absolute extent path ("file name syntax is incorrect", 2026-11-16): the extent
        # must sit in the VM folder under a relative name. A hard link costs no space on the same volume.
        local = folder / "install-flat.img"
        try:
            os.link(str(install), str(local))
        except OSError:
            import shutil
            print("hard link impossible (another volume?): copying %.1f GB" % (install.stat().st_size / 2 ** 30))
            shutil.copyfile(str(install), str(local))
        disks.append("install.vmdk")
        (folder / "install.vmdk").write_text(flat_descriptor(local.name, local.stat().st_size))
    vmx = folder / ("%s.vmx" % args.name)
    vmx.write_text(build_vmx(args.name, args.guest_os, args.ram_mb, args.cpus, disks))
    print("created %s (guestOS %s, %d MB, %d vCPU, disk %d GB growable%s)\nOpen it with VMware; the Unlocker must be applied first."
          % (vmx, args.guest_os, args.ram_mb, args.cpus, args.disk_gb, ", install image attached as a disk" if install else ""))
    return 0


def selftest():
    fails = []

    def ok(cond, what):
        print("%-4s %s" % ("PASS" if cond else "FAIL", what))
        if not cond:
            fails.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        os.environ["LOOMORA_VMW_DIR"] = tmp
        vmx = build_vmx("t", "darwin25-64", 8192, 4, ["t.vmdk", "install.vmdk"])
        ok('guestOS = "darwin25-64"' in vmx and 'smc.version = "0"' in vmx and 'firmware = "efi"' in vmx, "vmx: guest, SMC, EFI")
        ok('sata0:1.fileName = "install.vmdk"' in vmx, "vmx: install disk on sata0:1")
        d = flat_descriptor("install-flat.img", 512 * 1000)
        ok('RW 1000 FLAT "install-flat.img" 0' in d, "descriptor: sectors and a relative extent name")
        z = Path(tmp) / UNLOCKER_ZIP
        z.write_bytes(b"not the unlocker")
        try:
            cmd_unpack(None)
            ok(False, "unpack refuses a zip with another sha256")
        except SetupError:
            ok(not (Path(tmp) / "unlocker").exists(), "unpack refuses a zip with another sha256, extracts nothing")
        evil = Path(tmp) / "evil.zip"
        with zipfile.ZipFile(str(evil), "w") as zf:
            zf.writestr("../escape.txt", "x")
        try:
            safe_extract(evil, Path(tmp) / "out")
            ok(False, "zip member outside the destination refused")
        except (SetupError, OSError):
            ok(not (Path(tmp) / "escape.txt").exists(), "zip member outside the destination refused")
        try:
            cmd_vmx(argparse.Namespace(name="../bad", install=None, disk_gb=1, ram_mb=1, cpus=1, guest_os="x"))
            ok(False, "VM name with a path is refused")
        except SetupError:
            ok(True, "VM name with a path is refused")
        ok(sha256_of(z) == hashlib.sha256(b"not the unlocker").hexdigest(), "sha256_of")
    print("selftest: %s" % ("FAILED (%d)" % len(fails) if fails else "all passed"))
    return 1 if fails else 0


def main(argv=None):
    setup_utf8_output()
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--selftest" in argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="action")
    sub.required = True
    for name, fn in (("check", cmd_check), ("plan", cmd_plan), ("unpack", cmd_unpack),
                     ("unlock", lambda a: patch("unlock")), ("relock", lambda a: patch("relock"))):
        sub.add_parser(name).set_defaults(fn=fn)
    s = sub.add_parser("vmx")
    s.add_argument("name")
    s.add_argument("--install", help="raw install image (install.img made by macvm.py fetch)")
    s.add_argument("--disk-gb", type=int, default=80)
    s.add_argument("--ram-mb", type=int, default=8192)
    s.add_argument("--cpus", type=int, default=4)
    s.add_argument("--guest-os", default="darwin25-64", help="VMware guest id; try darwin24-64 if VMware shows 'other'")
    s.set_defaults(fn=cmd_vmx)
    args = ap.parse_args(argv)
    if os.name != "nt":
        print("error: run this on the Windows host", file=sys.stderr)
        return 2
    try:
        return args.fn(args)
    except SetupError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
