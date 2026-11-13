"""VM screenshots of the Windows Legacy UI (before = old exe, after = new exe), docs/adr/0004.
Usage: python validation/evidence/windows-2000-x86/adr-0004/vm-shots.py OUTDIR BEFORE_EXE AFTER_EXE
Both executables must lie under the repository (the launcher prints their relative path)
Uses the launcher machinery of the committed Windows 2000 trial driver; a NEW guest folder is used
(LOOMORA_VM_DIR must be set to one that does not exist) and removed at the end.
"""
import importlib.util
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location(
    "vmtd", str(ROOT / "validation" / "evidence" / "windows-2000-x86" / "trial-driver.py"))
td = importlib.util.module_from_spec(spec)
spec.loader.exec_module(td)

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
before_exe, after_exe = Path(sys.argv[2]), Path(sys.argv[3])

trial = td.Trial()
try:
    rc, _o, _e = trial.guest.guestcontrol("stat", [trial.launcher.guest_dir])
    if rc == 0:
        sys.exit("guest folder %s already exists" % trial.launcher.guest_dir)
    for label, exe in (("vm-before", before_exe), ("vm-after", after_exe)):
        trial.launcher.exe = exe
        print(trial.launcher.prepare())
        pid = trial.launcher.start_exe([])
        time.sleep(4)
        trial.vbm("controlvm", trial.vm, "screenshotpng", str(out / (label + "-1-initial.png")))
        codes = td.key(td.HOME) + [td.SHIFT] + td.key(td.END) + [td.SHIFT | 0x80]
        codes += td.typing_scancodes("Marie", azerty=True)
        trial.scancodes(codes)
        time.sleep(1.5)
        trial.vbm("controlvm", trial.vm, "screenshotpng", str(out / (label + "-2-typed-marie.png")))
        trial.scancodes([td.ALT, td.F4, td.F4 | 0x80, td.ALT | 0x80])
        time.sleep(3)
        count, _txt = trial.procs()
        print(label, "pid", pid, "processes after Alt+F4:", count)
finally:
    try:
        trial.step_cleanup()
    finally:
        trial.close()
