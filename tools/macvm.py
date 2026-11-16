#!/usr/bin/env python3
"""Manage the macOS virtual machines (QEMU/KVM inside WSL2) used for the Apple trials (ADR 0019).

Run it in WSL (python3 tools/macvm.py ...), or from the Windows host: it then relays itself to WSL.

    macvm.py check                 read-only check of the host (KVM, QEMU, disk space)
    macvm.py plan [VM...]          what each VM is for, its size and the media it needs (no network)
    macvm.py fetch tahoe|mavericks --yes-download
                                   the ONLY command that downloads (OSX-KVM and the Apple recovery image;
                                   without the flag it prints the steps and downloads nothing)
    macvm.py create VM             create the sparse qcow2 disk (and the firmware variables); no download
    macvm.py cmd VM [--install]    print the QEMU command line (review; nothing is started)
    macvm.py start VM [--install] [--headless] [--force]
                                   refuses if ignore_msrs is not Y or the VM folder has < 15 GB free;
                                   logs the command, the host state and QEMU's exit status in qemu.log,
                                   and the guest errors QEMU saw in qemu-debug.log
    macvm.py status VM             read-only: host state, written size of the disk, QEMU's view of the guest
                                   (`info status`), last lines of both logs
    macvm.py screenshot VM         dump the guest screen to screen.ppm and say whether it is all black
    macvm.py stop VM              ACPI power-off through the QEMU monitor
    macvm.py snapshot VM [NAME]    create a snapshot of the stopped VM (default NAME: clean)
    macvm.py restore VM [NAME]     go back to a snapshot of the stopped VM
    macvm.py ssh VM [-- command]   ssh to the guest through the forwarded port
    macvm.py --selftest

The VM folder is $LOOMORA_VM_DIR (default $HOME/loomora-vm). Exit status: 0 success, 1 a failed
check, 2 a usage or setup error. Python 3.8+, standard library only (ADR 0015). The boot recipes
come from OSX-KVM and a Proxmox runbook for 10.6; none of them was booted on this machine yet.
"""
import argparse
import os
import platform
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import setup_utf8_output  # noqa: E402

QEMU = "qemu-system-x86_64"
MIN_QEMU = (8, 2, 2)  # OSX-KVM requirement (README, read 2026-11-15)
# Key the guest checks in the SMC; published by OSX-KVM and by the Proxmox runbook for 10.6.
OSK = "ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc"
OSXKVM_URL = "https://github.com/kholia/OSX-KVM.git"

VMS = {
    "tahoe": {
        "title": "macOS Tahoe 26 (Intel guest) with Xcode 26.x",
        "purpose": "Modern macOS and iOS simulator (T047-T052, T061); CI on a macos-26 runner covers it "
                   "without any local disk, so this VM is optional",
        "boot": "opencore", "ram_mb": 8192, "smp": "4,cores=2,sockets=1",
        "disk_gb": 80, "ssh_port": 2222, "vnc": 1,
        "media": "downloaded by `fetch` (Apple recovery image through OSX-KVM, size not stated by OSX-KVM; "
                 "the macOS install itself then downloads more from Apple), Xcode 26.x from the owner's Apple ID",
        "estimate": "disk 50-70 GB in use (estimate); the 80 GB disk is sparse, so the VHDX must be able to hold what is written (`macvm-vhdx.ps1` size), download 20-30 GB (estimates, not measured)",
    },
    "mavericks": {
        "title": "OS X 10.9 Mavericks with Xcode 5.1.1",
        "purpose": "build the Legacy iOS (armv7, armv7s, arm64) and macOS UIs (T078, T079, T067)",
        "boot": "chameleon", "ram_mb": 4096, "smp": "1",
        "disk_gb": 12, "ssh_port": 2223, "vnc": 2,
        # Apple Internet Recovery image of the board that shipped 10.9, through the OSX-KVM script
        # (board id and MLB: recovery_urls.txt of OpenCorePkg macrecovery, read 2026-11-16).
        "recovery": {"board": "Mac-F60DEB81FF30ACF6", "mlb": "00000000000FNN100", "os_type": "default"},
        "media": "the 10.9 recovery image is downloaded by `fetch` (a recovery image, not the full installer: "
                 "the install itself then downloads from Apple, which may no longer work for 10.9; not verified); "
                 "supplied by the owner: a Chameleon ISO, Xcode 5.1.1 and the 10.6 SDK (Xcode 3.x or 4.x) "
                 "from the developer account",
        "estimate": "disk 15-25 GB in use (estimate); recovery download about 1 GB if it is like the Tahoe one (not measured)",
    },
    "snowleopard": {
        "title": "Mac OS X Server 10.6.8 Snow Leopard",
        "purpose": "run the macOS 10.6 floor of the Legacy UI (T067, T089)",
        "boot": "chameleon", "ram_mb": 2048, "smp": "1",
        "disk_gb": 8, "ssh_port": 2224, "vnc": 3,
        "media": "supplied by the owner: the Snow Leopard Server install disc as a raw image, a Chameleon "
                 "ISO, the 10.6.8 Server combo update",
        "estimate": "disk 8-12 GB in use, download 0 if the media already exist (estimates)",
    },
}


class SetupError(Exception):
    """Something to install, mount or supply first; the message says what."""


def vm_root():
    return Path(os.environ.get("LOOMORA_VM_DIR") or (Path.home() / "loomora-vm"))


def vm_dir(vm):
    return vm_root() / vm


def osxkvm_dir():
    return Path(os.environ.get("LOOMORA_OSXKVM_DIR") or (vm_root() / "OSX-KVM"))


def paths(vm):
    d = vm_dir(vm)
    return {"dir": d, "disk": d / "disk.qcow2", "install": d / "install.img", "chameleon": d / "chameleon.iso",
            "ovmf_vars": d / "OVMF_VARS.fd", "pid": d / "qemu.pid", "mon": d / "monitor.sock",
            "log": d / "qemu.log", "debug": d / "qemu-debug.log", "shot": d / "screen.ppm"}


def known(vm):
    if vm not in VMS:
        raise SetupError("unknown VM %r; known: %s" % (vm, ", ".join(sorted(VMS))))


def build_command(vm, install=False, headless=False, display="gtk"):
    """The QEMU command line of one VM, as a list. Pure: touches nothing."""
    known(vm)
    spec, p = VMS[vm], paths(vm)
    c = [QEMU, "-enable-kvm", "-m", str(spec["ram_mb"]), "-smp", spec["smp"],
         "-device", "isa-applesmc,osk=" + OSK,
         "-monitor", "unix:%s,server,nowait" % p["mon"], "-pidfile", str(p["pid"]),
         # What the guest does that QEMU cannot emulate (unimplemented devices, bad accesses).
         "-D", str(p["debug"]), "-d", "guest_errors,unimp"]
    if headless:
        c += ["-display", "none", "-vnc", "127.0.0.1:%d" % spec["vnc"]]
    else:
        c += ["-display", display]
    net = "user,id=net0,hostfwd=tcp:127.0.0.1:%d-:22" % spec["ssh_port"]
    if spec["boot"] == "opencore":
        kvm = osxkvm_dir()
        cpu = ("Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,vmware-cpuid-freq=on,"
               "+ssse3,+sse4.2,+popcnt,+avx,+aes,+xsave,+xsaveopt,check")
        c += ["-cpu", cpu, "-machine", "q35",
              "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0",
              "-device", "usb-tablet,bus=xhci.0", "-device", "usb-ehci,id=ehci",
              "-drive", "if=pflash,format=raw,readonly=on,file=%s" % (kvm / "OVMF_CODE_4M.fd"),
              "-drive", "if=pflash,format=raw,file=%s" % p["ovmf_vars"],
              "-smbios", "type=2", "-device", "ich9-intel-hda", "-device", "hda-duplex",
              "-device", "ich9-ahci,id=sata",
              "-drive", "id=OpenCoreBoot,if=none,snapshot=on,format=qcow2,file=%s" % (kvm / "OpenCore" / "OpenCore.qcow2"),
              "-device", "ide-hd,bus=sata.2,drive=OpenCoreBoot"]
        if install:
            c += ["-drive", "id=InstallMedia,if=none,file=%s,format=raw" % p["install"],
                  "-device", "ide-hd,bus=sata.3,drive=InstallMedia"]
        c += ["-drive", "id=MacHDD,if=none,file=%s,format=qcow2" % p["disk"],
              "-device", "ide-hd,bus=sata.4,drive=MacHDD",
              "-netdev", net, "-device", "virtio-net-pci,netdev=net0,id=net0,mac=52:54:00:c9:18:27",
              "-device", "vmware-svga"]
    else:
        # Chameleon on SeaBIOS, Penryn, one vCPU, E1000, VMware SVGA (Proxmox runbook for 10.6.8).
        c += ["-cpu", "Penryn", "-machine", "pc-q35-2.5", "-boot", "order=d",
              "-device", "ahci,id=ahci0",
              "-drive", "id=hd0,if=none,file=%s,format=qcow2,discard=unmap" % p["disk"],
              "-device", "ide-hd,bus=ahci0.0,drive=hd0",
              "-drive", "id=chameleon,if=none,file=%s,format=raw,media=cdrom,readonly=on" % p["chameleon"],
              "-device", "ide-cd,bus=ide.2,drive=chameleon"]
        if install:
            c += ["-drive", "id=inst,if=none,file=%s,format=raw" % p["install"],
                  "-device", "ide-hd,bus=ahci0.1,drive=inst"]
        c += ["-netdev", net, "-device", "e1000,netdev=net0",
              "-device", "vmware-svga,vgamem_mb=32",
              "-device", "usb-ehci,id=ehci", "-device", "usb-mouse,bus=ehci.0,port=2",
              "-device", "usb-kbd,bus=ehci.0"]
    return c


def missing_files(vm, install):
    """Files the start of a VM needs and that the owner has to supply or fetch."""
    p, spec = paths(vm), VMS[vm]
    need = [p["disk"]]
    if spec["boot"] == "opencore":
        kvm = osxkvm_dir()
        need += [p["ovmf_vars"], kvm / "OVMF_CODE_4M.fd", kvm / "OpenCore" / "OpenCore.qcow2"]
    else:
        need += [p["chameleon"]]
    if install:
        need += [p["install"]]
    return [str(f) for f in need if not f.exists()]


def free_gb(path):
    probe = Path(path)
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    return shutil.disk_usage(str(probe)).free / 2 ** 30


def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, **kw)


def in_wsl():
    return "microsoft" in platform.release().lower()


def qemu_version():
    exe = shutil.which(QEMU)
    if not exe:
        return None
    m = re.search(r"version (\d+)\.(\d+)\.?(\d*)", run([exe, "--version"]).stdout)
    return tuple(int(x or 0) for x in m.groups()) if m else None


def distro_host_drive():
    """(drive letter, free GB) of the Windows drive that holds this distribution's .vhdx, or None.

    The drive letter comes from $LOOMORA_HOST_DRIVE, which the Windows side sets when it relays the
    command (WSL interop with powershell.exe is not reliable on every machine)."""
    letter = (os.environ.get("LOOMORA_HOST_DRIVE") or "").strip(": \\").lower()
    mount = Path("/mnt/%s" % letter)
    if len(letter) != 1 or not mount.exists():
        return None
    return (letter.upper() + ":", free_gb(mount))


def distro_drive_on_windows(distro):
    """Windows side: the drive letter of the folder that holds the .vhdx of a WSL distribution."""
    try:
        import winreg
        base = r"Software\Microsoft\Windows\CurrentVersion\Lxss"
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, base) as k:
            for i in range(winreg.QueryInfoKey(k)[0]):
                with winreg.OpenKey(k, winreg.EnumKey(k, i)) as sub:
                    if winreg.QueryValueEx(sub, "DistributionName")[0] == distro:
                        m = re.search(r"([A-Za-z]):", winreg.QueryValueEx(sub, "BasePath")[0])
                        return m.group(1) if m else None
    except (ImportError, OSError):
        pass
    return None


def cmd_check(_args):
    """Read-only. One line per check; exit 1 if one failed."""
    results = []

    def line(ok, name, detail):
        results.append(ok)
        print("%-4s %-22s %s" % ("PASS" if ok else "FAIL", name, detail))

    cpuinfo = Path("/proc/cpuinfo").read_text() if Path("/proc/cpuinfo").exists() else ""
    flags = set(re.findall(r"\b[a-z0-9_]+\b", " ".join(l for l in cpuinfo.splitlines() if l.startswith("flags"))))
    line(in_wsl(), "wsl2", platform.release())
    line(Path("/dev/kvm").exists(), "/dev/kvm",
         "present" if Path("/dev/kvm").exists() else
         "missing: nested virtualization is off (.wslconfig `nestedVirtualization=true` and a `wsl --shutdown`)")
    if Path("/dev/kvm").exists():
        line(os.access("/dev/kvm", os.R_OK | os.W_OK), "/dev/kvm access",
             "read/write" if os.access("/dev/kvm", os.R_OK | os.W_OK) else "no access: sudo usermod -aG kvm $USER, then restart WSL")
    line(bool(flags & {"vmx", "svm"}), "cpu virtualization", "vmx/svm exposed to WSL" if flags & {"vmx", "svm"} else "not exposed")
    line("sse4_1" in flags, "cpu sse4.1", "needed for macOS 10.12+")
    line("avx2" in flags, "cpu avx2", "needed for macOS 13+ (Tahoe)")
    msrs = Path("/sys/module/kvm/parameters/ignore_msrs")
    if msrs.exists():
        v = msrs.read_text().strip()
        line(v in ("Y", "1"), "kvm ignore_msrs", "%s (macOS guests reboot-loop without it: echo 1 | sudo tee %s)" % (v, msrs))
    ver = qemu_version()
    line(ver is not None and ver >= MIN_QEMU, "qemu >= 8.2.2",
         ".".join(map(str, ver)) if ver else "not installed: sudo apt install qemu-system-x86 qemu-utils")
    for tool in ("qemu-img", "dmg2img", "git"):
        line(shutil.which(tool) is not None, tool, shutil.which(tool) or "not installed")
    root = vm_root()
    where = str(root.resolve()) if root.exists() else str(root)
    inside = not re.match(r"^/mnt/[a-z]/", where) and not where.startswith("/mnt/wsl/")
    host = distro_host_drive() if inside else None
    if host:
        # Inside the distribution disk `df` shows the size the virtual disk may grow to, not the room
        # left on the Windows drive that holds the .vhdx, which is the real limit.
        gb = host[1]
        line(gb >= 40, "free disk", "%.1f GB free on %s, the Windows drive holding the distribution disk "
             "(`df` inside WSL would show %.0f GB, the virtual size): use tools/macvm-vhdx.ps1 to put the VMs "
             "on another drive" % (gb, host[0], free_gb(root)))
    elif inside:
        line(False, "free disk", "unknown: `df` shows %.0f GB, the virtual size of the distribution disk, not the room left "
             "on the Windows drive that holds it; run this from Windows or set LOOMORA_HOST_DRIVE=<letter>" % free_gb(root))
    else:
        line(free_gb(root) >= 40, "free disk", "%.1f GB free where %s lives" % (free_gb(root), where))
    if re.match(r"^/mnt/[a-z]/", where):
        line(False, "vm folder", "on a Windows drive (DrvFs): slow qcow2 I/O; use a VHDX mounted with tools/macvm-vhdx.ps1")
    return 0 if all(results) else 1


def cmd_plan(args):
    names = args.vm or sorted(VMS)
    print("VM folder: %s  (%.1f GB free there)" % (vm_root(), free_gb(vm_root())))
    for vm in names:
        known(vm)
        s = VMS[vm]
        print("\n%s: %s\n  for:    %s\n  boot:   %s, %d MB RAM, smp %s, virtual disk %d GB (sparse)\n  ssh:    127.0.0.1:%d, vnc :%d (headless)"
              % (vm, s["title"], s["purpose"], s["boot"], s["ram_mb"], s["smp"], s["disk_gb"], s["ssh_port"], s["vnc"]))
        print("  media:  %s\n  size:   %s" % (s["media"], s["estimate"]))
        f = missing_files(vm, install=True)
        print("  missing now: %s" % (", ".join(f) if f else "nothing"))
    return 0


def cmd_fetch(args):
    known(args.vm)
    spec = VMS[args.vm]
    recovery = spec.get("recovery")
    if recovery is None and spec["boot"] != "opencore":
        raise SetupError("%s has no download: its media are supplied by the owner (see `plan %s`)" % (args.vm, args.vm))
    d, kvm = vm_dir(args.vm), osxkvm_dir()
    steps = []
    if not (kvm / "fetch-macOS-v2.py").exists():
        steps.append(["git", "clone", "--depth", "1", "--recursive", OSXKVM_URL, str(kvm)])
    if recovery:
        # An old OS: the recovery image of the board that shipped it, asked by board id and MLB.
        steps.append([sys.executable, str(kvm / "fetch-macOS-v2.py"), "--action", "download",
                      "-b", recovery["board"], "-m", recovery["mlb"], "-os", recovery["os_type"],
                      "-n", "BaseSystem", "-o", str(d)])
    else:
        steps.append([sys.executable, str(kvm / "fetch-macOS-v2.py"), "--shortname", args.vm, "--outdir", str(d)])
    steps.append(["dmg2img", "-i", str(d / "BaseSystem.dmg"), str(d / "install.img")])
    if not args.yes_download:
        print("Nothing was downloaded. These steps use the network (OSX-KVM is about 170 MB; the recovery image "
              "size is printed by the script when it starts):")
        for s in steps:
            print("  " + " ".join(shlex.quote(x) for x in s))
        print("Run again with --yes-download once disk space and data allowance are checked.")
        return 2
    if free_gb(d) < 20:
        raise SetupError("less than 20 GB free where %s lives; refusing to download" % d)
    d.mkdir(parents=True, exist_ok=True)
    for s in steps:
        print("+ " + " ".join(shlex.quote(x) for x in s), flush=True)
        # fetch-macOS-v2.py writes to the current folder whatever --outdir says, and its verification calls
        # os.get_terminal_size(), which fails without a terminal: run it in the VM folder under a pty.
        if "fetch-macOS-v2.py" in " ".join(s) and shutil.which("script"):
            s = ["script", "-qec", " ".join(shlex.quote(x) for x in s), "/dev/null"]
        if subprocess.call(s, cwd=str(d)) != 0:
            return 1
    return 0


def cmd_create(args):
    known(args.vm)
    p, spec = paths(args.vm), VMS[args.vm]
    if p["disk"].exists():
        raise SetupError("%s already exists; refusing to overwrite" % p["disk"])
    if shutil.which("qemu-img") is None:
        raise SetupError("qemu-img not found: sudo apt install qemu-utils")
    if free_gb(p["dir"]) < 10:
        raise SetupError("less than 10 GB free where %s lives" % p["dir"])
    p["dir"].mkdir(parents=True, exist_ok=True)
    subprocess.check_call(["qemu-img", "create", "-f", "qcow2", str(p["disk"]), "%dG" % spec["disk_gb"]])
    if spec["boot"] == "opencore":
        src = osxkvm_dir() / "OVMF_VARS-1920x1080.fd"
        if src.exists():
            shutil.copyfile(str(src), str(p["ovmf_vars"]))
        else:
            print("NOTE: %s is missing (run `fetch`); copy it to %s before the first start" % (src, p["ovmf_vars"]))
    print("created %s (sparse, %d GB virtual)" % (p["disk"], spec["disk_gb"]))
    return 0


def cmd_cmd(args):
    print(" ".join(shlex.quote(x) for x in build_command(args.vm, args.install, args.headless)))
    return 0


def monitor(vm, text):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(5)
    s.connect(str(paths(vm)["mon"]))
    s.sendall((text + "\n").encode())
    time.sleep(0.5)
    s.close()


def running(vm):
    pid = paths(vm)["pid"]
    try:
        os.kill(int(pid.read_text()), 0)
        return True
    except (OSError, ValueError):
        return False


MIN_FREE_GB = 15  # a guest installing macOS writes tens of GB; a full VHDX ends the VM abruptly


def host_state(vm):
    """What the host looks like now; the values a stopped or black-screen VM is judged against."""
    msrs = Path("/sys/module/kvm/parameters/ignore_msrs")
    mem = None
    try:
        m = re.search(r"MemAvailable:\s+(\d+) kB", Path("/proc/meminfo").read_text())
        mem = int(m.group(1)) // 1024 if m else None
    except OSError:
        pass
    return {"ignore_msrs": msrs.read_text().strip() if msrs.exists() else None,
            "free_gb": free_gb(vm_dir(vm)), "mem_available_mb": mem}


def preflight_problems(state):
    """Reasons not to start a macOS guest, from host_state(). Pure."""
    out = []
    if state["ignore_msrs"] not in ("Y", "1"):
        out.append("kvm ignore_msrs is %s: macOS guests reboot-loop or stay on a black screen "
                   "(echo 1 | sudo tee /sys/module/kvm/parameters/ignore_msrs; it is lost at each `wsl --shutdown`)"
                   % state["ignore_msrs"])
    if state["free_gb"] < MIN_FREE_GB:
        out.append("only %.1f GB free where the VM lives (need %d): the guest would stop when the disk is full"
                   % (state["free_gb"], MIN_FREE_GB))
    return out


def cmd_start(args):
    known(args.vm)
    if running(args.vm):
        raise SetupError("%s is already running" % args.vm)
    miss = missing_files(args.vm, args.install)
    if miss:
        raise SetupError("missing: %s (see `plan %s`)" % (", ".join(miss), args.vm))
    if not Path("/dev/kvm").exists():
        raise SetupError("/dev/kvm is missing (run `check`)")
    state = host_state(args.vm)
    problems = preflight_problems(state)
    if problems and not args.force:
        raise SetupError("refusing to start (--force to override):\n  - " + "\n  - ".join(problems))
    for pr in problems:
        print("WARNING (forced): " + pr)
    p = paths(args.vm)
    cmd = build_command(args.vm, args.install, args.headless)
    cmdline = " ".join(shlex.quote(x) for x in cmd)
    log = open(str(p["log"]), "ab")
    log.write(("\n[macvm] %s start of %s, install=%s headless=%s; ignore_msrs=%s, %.1f GB free, %s MB RAM available\n"
               "[macvm] %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), args.vm, args.install, args.headless,
                                 state["ignore_msrs"], state["free_gb"], state["mem_available_mb"], cmdline)).encode())
    log.flush()
    # The shell records how QEMU ended: status 0 is a guest shutdown, 137 or 143 a kill (WSL stopped,
    # out of memory), another value a QEMU error.
    script = '%s; echo "[macvm] qemu exited with status $? at $(date +%%Y-%%m-%%d_%%H:%%M:%%S)" >> %s' % (
        cmdline, shlex.quote(str(p["log"])))
    subprocess.Popen(["sh", "-c", script], stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True)
    print("%s started; log %s; debug log %s; ssh port %d\n"
          "If the screen goes black: `macvm.py status %s` and `macvm.py screenshot %s` (do not close the window); "
          "`--headless` avoids the WSLg window (VNC on 127.0.0.1:%d)."
          % (args.vm, p["log"], p["debug"], VMS[args.vm]["ssh_port"], args.vm, args.vm, 5900 + VMS[args.vm]["vnc"]))
    return 0


def monitor_query(vm, text):
    """Send one QEMU monitor command and return its reply (the monitor must be up)."""
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(2)
    s.connect(str(paths(vm)["mon"]))

    def drain():
        data = b""
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
        except socket.timeout:
            pass
        return data.decode("utf-8", "replace")

    drain()  # banner
    s.sendall((text + "\n").encode())
    reply = drain()
    s.close()
    return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", reply).replace("\r", "")


def cmd_status(args):
    """Read-only: is the VM running, what does QEMU say about the guest, what did the logs record."""
    known(args.vm)
    p = paths(args.vm)
    st = host_state(args.vm)
    print("host: ignore_msrs=%s, %.1f GB free in %s, %s MB RAM available" % (
        st["ignore_msrs"], st["free_gb"], vm_dir(args.vm), st["mem_available_mb"]))
    for pr in preflight_problems(st):
        print("  PROBLEM: " + pr)
    if p["disk"].exists() and shutil.which("qemu-img"):
        info = run(["qemu-img", "info", "--output=json", str(p["disk"])]).stdout
        m = re.search(r'"actual-size":\s*(\d+)', info)
        v = re.search(r'"virtual-size":\s*(\d+)', info)
        if m and v:
            print("disk: %.1f GB written of %.0f GB virtual" % (int(m.group(1)) / 2 ** 30, int(v.group(1)) / 2 ** 30))
    if running(args.vm):
        print("qemu: running (pid %s)" % p["pid"].read_text().strip())
        try:
            # running = the guest runs (a black screen with `running` is a guest that is up but not drawing,
            # or a display problem); paused, guest-panicked or internal-error say what stopped it.
            reply = [l for l in monitor_query(args.vm, "info status").splitlines() if l.startswith("VM status")]
            print("monitor: %s" % (reply[0] if reply else "no `VM status` line in the reply"))
        except (OSError, socket.timeout) as e:
            print("monitor: no answer (%s)" % e)
    else:
        print("qemu: not running")
    for key in ("log", "debug"):
        if p[key].exists():
            lines = p[key].read_text(errors="replace").splitlines()
            print("\n--- last lines of %s (%d lines in all) ---" % (p[key], len(lines)))
            print("\n".join(lines[-15:]))
    return 0


def cmd_screenshot(args):
    """Ask QEMU for the guest screen, as it is, in the VM folder (a diagnostic, never evidence)."""
    known(args.vm)
    if not running(args.vm):
        raise SetupError("%s is not running" % args.vm)
    shot = paths(args.vm)["shot"]
    monitor_query(args.vm, "screendump %s" % shot)
    time.sleep(1)
    if not shot.exists():
        raise SetupError("no screen dump written to %s" % shot)
    data = shot.read_bytes()
    head = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    black = bool(head) and not any(data[head.end():])
    print("%s written (%d bytes, %s); open it in a viewer that reads PPM. Every pixel black: %s"
          % (shot, len(data), "%sx%s" % head.groups()[:2] if head else "not a P6 PPM", "yes" if black else "no"))
    return 0


def cmd_stop(args):
    known(args.vm)
    if not running(args.vm):
        print("%s is not running" % args.vm)
        return 0
    monitor(args.vm, "system_powerdown")
    print("power-off requested; a guest without ACPI support needs `quit` in the monitor: %s" % paths(args.vm)["mon"])
    return 0


def snapshot_tool(args, create):
    known(args.vm)
    if running(args.vm):
        raise SetupError("stop %s first (snapshots of a running VM are not taken by this tool)" % args.vm)
    flag = "-c" if create else "-a"
    return subprocess.call(["qemu-img", "snapshot", flag, args.name, str(paths(args.vm)["disk"])])


def cmd_ssh(args):
    known(args.vm)
    cmd = ["ssh", "-p", str(VMS[args.vm]["ssh_port"]), "-o", "StrictHostKeyChecking=accept-new",
           "-o", "UserKnownHostsFile=%s" % (vm_dir(args.vm) / "known_hosts"), "127.0.0.1"] + (args.rest or [])
    return subprocess.call(cmd)


def relay_to_wsl(argv):
    """From a Windows host: run the same command in WSL (ADR 0015)."""
    distro = os.environ.get("LOOMORA_WSL_DISTRO", "Ubuntu-24.04")
    root = Path(__file__).resolve().parent.parent
    drive = root.drive[0].lower()
    wsl_root = "/mnt/%s/%s" % (drive, str(root)[3:].replace("\\", "/"))
    env = []
    drive_letter = distro_drive_on_windows(distro)
    if drive_letter:
        env = ["env", "LOOMORA_HOST_DRIVE=" + drive_letter]
    return subprocess.call(["wsl", "-d", distro, "--cd", wsl_root, "--"] + env + ["python3", "tools/macvm.py"] + argv)


def selftest():
    with tempfile.TemporaryDirectory() as tmp:
        os.environ["LOOMORA_VM_DIR"] = tmp
        fails = []

        def ok(cond, what):
            print("%-4s %s" % ("PASS" if cond else "FAIL", what))
            if not cond:
                fails.append(what)

        for vm in VMS:
            c = build_command(vm, install=True)
            joined = " ".join(c)
            ok("-enable-kvm" in c and "isa-applesmc,osk=" + OSK in c, "%s: kvm and SMC key" % vm)
            ok(str(paths(vm)["disk"]) in joined and str(paths(vm)["install"]) in joined, "%s: disk and install media" % vm)
            ok(str(paths(vm)["install"]) not in " ".join(build_command(vm)), "%s: no install media outside --install" % vm)
            ok("hostfwd=tcp:127.0.0.1:%d-:22" % VMS[vm]["ssh_port"] in joined, "%s: ssh forwarded on loopback only" % vm)
            h = build_command(vm, headless=True)
            ok("-vnc" in h and "127.0.0.1:%d" % VMS[vm]["vnc"] in h, "%s: headless VNC on loopback" % vm)
        ok(len({v["ssh_port"] for v in VMS.values()}) == len(VMS), "distinct ssh ports")
        ok("-D" in build_command("tahoe") and str(paths("tahoe")["debug"]) in build_command("tahoe"),
           "QEMU writes its guest-error log to qemu-debug.log")
        good = {"ignore_msrs": "Y", "free_gb": 50.0, "mem_available_mb": 9000}
        ok(preflight_problems(good) == [], "preflight: a ready host has no problem")
        ok(len(preflight_problems(dict(good, ignore_msrs="N"))) == 1, "preflight: ignore_msrs N refused")
        ok(len(preflight_problems(dict(good, ignore_msrs=None, free_gb=3.0))) == 2, "preflight: missing ignore_msrs and low disk")
        ok("Skylake-Client" in " ".join(build_command("tahoe")), "tahoe: Skylake-Client CPU")
        ok("Penryn" in " ".join(build_command("snowleopard")), "snowleopard: Penryn CPU")
        # fetch must refuse to touch the network without the flag
        ns = argparse.Namespace(vm="tahoe", yes_download=False)
        ok(cmd_fetch(ns) == 2, "fetch without --yes-download downloads nothing (status 2)")
        ok(not (vm_dir("tahoe")).exists(), "fetch without the flag creates nothing")
        ns = argparse.Namespace(vm="mavericks", yes_download=False)
        ok(cmd_fetch(ns) == 2 and not vm_dir("mavericks").exists(), "fetch mavericks without the flag downloads nothing")
        ok(VMS["mavericks"]["recovery"]["board"] == "Mac-F60DEB81FF30ACF6"
           and len(VMS["mavericks"]["recovery"]["mlb"]) == 17, "mavericks: recovery board id and 17-character MLB")
        try:
            cmd_fetch(argparse.Namespace(vm="snowleopard", yes_download=True))
            ok(False, "fetch of a legacy VM is refused")
        except SetupError:
            ok(True, "fetch of a legacy VM is refused")
        try:
            known("nope")
            ok(False, "unknown VM refused")
        except SetupError:
            ok(True, "unknown VM refused")
        ok(len(missing_files("tahoe", True)) >= 4, "missing_files lists what is absent")
    print("selftest: %s" % ("FAILED (%d)" % len(fails) if fails else "all passed"))
    return 1 if fails else 0


def main(argv=None):
    setup_utf8_output()
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--selftest" in argv:
        return selftest()
    if os.name == "nt":
        return relay_to_wsl(argv)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="action")
    sub.required = True
    sub.add_parser("check").set_defaults(fn=cmd_check)
    s = sub.add_parser("plan")
    s.add_argument("vm", nargs="*")
    s.set_defaults(fn=cmd_plan)
    s = sub.add_parser("fetch")
    s.add_argument("vm")
    s.add_argument("--yes-download", action="store_true")
    s.set_defaults(fn=cmd_fetch)
    s = sub.add_parser("create")
    s.add_argument("vm")
    s.set_defaults(fn=cmd_create)
    for name, fn in (("cmd", cmd_cmd), ("start", cmd_start)):
        s = sub.add_parser(name)
        s.add_argument("vm")
        s.add_argument("--install", action="store_true")
        s.add_argument("--headless", action="store_true")
        if name == "start":
            s.add_argument("--force", action="store_true", help="start although the preflight found a problem")
        s.set_defaults(fn=fn)
    for name, fn in (("status", cmd_status), ("screenshot", cmd_screenshot)):
        s = sub.add_parser(name)
        s.add_argument("vm")
        s.set_defaults(fn=fn)
    s = sub.add_parser("stop")
    s.add_argument("vm")
    s.set_defaults(fn=cmd_stop)
    for name, create in (("snapshot", True), ("restore", False)):
        s = sub.add_parser(name)
        s.add_argument("vm")
        s.add_argument("name", nargs="?", default="clean")
        s.set_defaults(fn=lambda a, c=create: snapshot_tool(a, c))
    s = sub.add_parser("ssh")
    s.add_argument("vm")
    s.add_argument("rest", nargs=argparse.REMAINDER)
    s.set_defaults(fn=cmd_ssh)
    args = ap.parse_args(argv)
    if getattr(args, "rest", None) and args.rest[:1] == ["--"]:
        args.rest = args.rest[1:]
    try:
        return args.fn(args)
    except SetupError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
