"""Screenshots of the Windows Legacy UI on the Windows 11 host (docs/adr/0004-fallback-per-case.md):
  python validation/evidence/windows-11-x64/adr-0004/shoot-window.py EXE LABEL OUTDIR [dark]

Starts EXE (no --probe), waits for the window, records the font of the controls (WM_GETFONT +
GetObject), captures the window (PrintWindow), types "Marie" with WM_CHAR, captures again, closes.
With "dark" it sets HKCU ...\\AppsUseLightTheme to 0 for the duration and restores the old value.
"""
import ctypes
import ctypes.wintypes as wt
import importlib.util
import json
import subprocess
import sys
import time
import winreg
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location(
    "td", str(ROOT / "validation" / "evidence" / "windows-11-x64" / "trial-driver.py"))
td = importlib.util.module_from_spec(spec)
spec.loader.exec_module(td)

exe, label, outdir = sys.argv[1], sys.argv[2], Path(sys.argv[3])
dark = len(sys.argv) > 4 and sys.argv[4] == "dark"
outdir.mkdir(parents=True, exist_ok=True)
U = td.USER32
G = td.GDI32
KEY = r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"


class LOGFONTW(ctypes.Structure):
    _fields_ = [("lfHeight", ctypes.c_long), ("lfWidth", ctypes.c_long),
                ("lfEscapement", ctypes.c_long), ("lfOrientation", ctypes.c_long),
                ("lfWeight", ctypes.c_long), ("lfItalic", ctypes.c_ubyte),
                ("lfUnderline", ctypes.c_ubyte), ("lfStrikeOut", ctypes.c_ubyte),
                ("lfCharSet", ctypes.c_ubyte), ("lfOutPrecision", ctypes.c_ubyte),
                ("lfClipPrecision", ctypes.c_ubyte), ("lfQuality", ctypes.c_ubyte),
                ("lfPitchAndFamily", ctypes.c_ubyte), ("lfFaceName", ctypes.c_wchar * 32)]


def font_of(hwnd):
    h = U.SendMessageW(hwnd, 0x31, 0, 0)  # WM_GETFONT
    lf = LOGFONTW()
    G.GetObjectW.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p]
    G.GetObjectW(h, ctypes.sizeof(lf), ctypes.byref(lf))
    return {"face": lf.lfFaceName, "lfHeight": lf.lfHeight, "weight": lf.lfWeight}


def run():
    proc = subprocess.Popen([exe], cwd=str(outdir))
    top = edit = static = None
    t0 = time.time()
    while time.time() - t0 < 15:
        wins = td.windows_of(proc.pid)
        if wins:
            top = wins[0]
            kids = td.children_of(top)
            edits = [k for k in kids if td.class_of(k) == "Edit"]
            statics = [k for k in kids if td.class_of(k) == "Static"]
            if edits and statics and td.text_of(statics[-1]):
                edit, static = edits[0], statics[-1]
                break
        time.sleep(0.01)
    if edit is None:
        proc.kill()
        raise SystemExit("window not found")
    time.sleep(0.6)
    info = {"label": label, "dark": dark, "edit_font": font_of(edit), "static_font": font_of(static),
            "edit_text": td.text_of(edit), "static_text": td.text_of(static)}
    td.capture(top, outdir / (label + "-1-initial.png"))
    U.SendMessageW(edit, td.EM_SETSEL, 0, -1)
    for ch in "Marie":
        U.SendMessageW(edit, td.WM_CHAR, ord(ch), 0)
    time.sleep(0.3)
    info["typed_static"] = td.text_of(static)
    td.capture(top, outdir / (label + "-2-typed-marie.png"))
    U.PostMessageW(top, td.WM_CLOSE, 0, 0)
    info["exit"] = proc.wait(timeout=10)
    return info


old = None
try:
    if dark:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_READ | winreg.KEY_SET_VALUE)
        old = winreg.QueryValueEx(k, "AppsUseLightTheme")[0]
        winreg.SetValueEx(k, "AppsUseLightTheme", 0, winreg.REG_DWORD, 0)
        winreg.CloseKey(k)
        time.sleep(0.3)
    info = run()
finally:
    if old is not None:
        k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_SET_VALUE)
        winreg.SetValueEx(k, "AppsUseLightTheme", 0, winreg.REG_DWORD, old)
        winreg.CloseKey(k)
print(json.dumps(info, indent=1))
(outdir / (label + ".json")).write_text(json.dumps(info, indent=1), encoding="utf-8")
