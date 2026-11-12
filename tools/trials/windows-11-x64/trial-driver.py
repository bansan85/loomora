#!/usr/bin/env python
"""Trial driver of the Windows Legacy UI on Windows 11 (T088, Windows 11 part, T094).

Run from the repository root on a Windows host after `python tools/build.py windows legacy`
(in WSL) has produced out/windows-legacy/{x64,x86}/loomora.exe:

  python validation/evidence/windows-11-x64/trial-driver.py

For each executable (x64, and x86 under WOW64) it:
  * runs `loomora.exe --probe` 10 times and `loomora.exe --probe --probe-input Marie` 10 times
    (serially, from a scratch copy of the exe), recording exit status, standard output and error,
    wall time, the probe file loomora-probe.json (checked with tools/check_probe.py) and whether a
    loomora.exe process is left over (tasklist);
  * starts the window WITHOUT --probe, waits for it, reads the EDIT and the STATIC controls with
    Win32 messages (WM_GETTEXT), captures the window with PrintWindow, selects the EDIT content
    (EM_SETSEL) and types "Marie" with WM_CHAR messages, reads the STATIC again, captures again,
    then types 1,100 characters to check the 1,000 limit, closes with WM_CLOSE and records the
    exit status and leftover processes.
It writes trial-measurements.json, host-environment.json, gui-<arch>.json, window-<arch>-*.png and
the probe series next to itself. Typing is done with window messages, not with a real keyboard.
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import winreg
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import check_probe  # noqa: E402
import common       # noqa: E402

ARCHS = ("x64", "x86")
RUNS = 10
USER32 = ctypes.WinDLL("user32", use_last_error=True)
GDI32 = ctypes.WinDLL("gdi32", use_last_error=True)

LRESULT = ctypes.c_ssize_t
USER32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
USER32.SendMessageW.restype = LRESULT
USER32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
USER32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
USER32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
USER32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
USER32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
USER32.IsWindowVisible.argtypes = [wt.HWND]
USER32.GetDC.argtypes = [wt.HWND]
USER32.GetDC.restype = wt.HDC
USER32.ReleaseDC.argtypes = [wt.HWND, wt.HDC]
USER32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
USER32.SystemParametersInfoW.argtypes = [wt.UINT, wt.UINT, ctypes.c_void_p, wt.UINT]
USER32.GetDpiForSystem.restype = wt.UINT
GDI32.CreateCompatibleDC.argtypes = [wt.HDC]
GDI32.CreateCompatibleDC.restype = wt.HDC
GDI32.CreateCompatibleBitmap.argtypes = [wt.HDC, ctypes.c_int, ctypes.c_int]
GDI32.CreateCompatibleBitmap.restype = wt.HBITMAP
GDI32.SelectObject.argtypes = [wt.HDC, wt.HGDIOBJ]
GDI32.SelectObject.restype = wt.HGDIOBJ
GDI32.DeleteObject.argtypes = [wt.HGDIOBJ]
GDI32.DeleteDC.argtypes = [wt.HDC]

WM_CLOSE, WM_GETTEXT, WM_GETTEXTLENGTH, WM_CHAR = 0x0010, 0x000D, 0x000E, 0x0102
EM_SETSEL, EM_GETLIMITTEXT = 0x00B1, 0x00D5
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


GDI32.GetDIBits.argtypes = [wt.HDC, wt.HBITMAP, wt.UINT, wt.UINT, ctypes.c_void_p,
                            ctypes.POINTER(BITMAPINFOHEADER), wt.UINT]


def text_of(hwnd):
    length = USER32.SendMessageW(hwnd, WM_GETTEXTLENGTH, 0, 0)
    buf = ctypes.create_unicode_buffer(length + 1)
    USER32.SendMessageW(hwnd, WM_GETTEXT, length + 1, ctypes.addressof(buf))
    return buf.value


def class_of(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    USER32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def windows_of(pid):
    """Visible top-level windows of a process."""
    found = []

    def callback(hwnd, _):
        owner = wt.DWORD(0)
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and USER32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    USER32.EnumWindows(WNDENUMPROC(callback), 0)
    return found


def children_of(hwnd):
    found = []

    def callback(child, _):
        found.append(child)
        return True
    USER32.EnumChildWindows(hwnd, WNDENUMPROC(callback), 0)
    return found


def png_bytes(width, height, rgb_rows):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + row for row in rgb_rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def capture(hwnd, path):
    """PrintWindow of a top-level window into a PNG; returns (width, height)."""
    rect = wt.RECT()
    USER32.GetWindowRect(hwnd, ctypes.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    screen = USER32.GetDC(None)
    memdc = GDI32.CreateCompatibleDC(screen)
    bitmap = GDI32.CreateCompatibleBitmap(screen, width, height)
    old = GDI32.SelectObject(memdc, bitmap)
    USER32.PrintWindow(hwnd, memdc, 2)  # PW_RENDERFULLCONTENT
    header = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), width, -height, 1, 32, 0)
    pixels = ctypes.create_string_buffer(width * height * 4)
    GDI32.GetDIBits(memdc, bitmap, 0, height, pixels, ctypes.byref(header), 0)
    GDI32.SelectObject(memdc, old)
    GDI32.DeleteObject(bitmap)
    GDI32.DeleteDC(memdc)
    USER32.ReleaseDC(None, screen)
    data = pixels.raw
    rows = []
    for y in range(height):
        line = data[y * width * 4:(y + 1) * width * 4]
        rows.append(b"".join(line[i + 2:i + 3] + line[i + 1:i + 2] + line[i:i + 1]
                             for i in range(0, len(line), 4)))
    Path(path).write_bytes(png_bytes(width, height, rows))
    return width, height


def loomora_processes():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq loomora.exe", "/NH"],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout
    return out.decode("oem", "replace").count("loomora.exe")


def stats(values):
    return {"min": min(values), "median": statistics.median(values), "max": max(values)}


def powershell_json(command):
    res = subprocess.run(["powershell", "-NoProfile", "-Command", command],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return json.loads(res.stdout.decode("utf-8", "replace"))


def host_environment():
    env = {
        "python": sys.version,
        "cmd_ver": subprocess.run(["cmd", "/c", "ver"], stdout=subprocess.PIPE).stdout
        .decode("oem", "replace").strip(),
        "system_dpi": USER32.GetDpiForSystem(),
    }
    key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                         r"SOFTWARE\Microsoft\Windows NT\CurrentVersion")
    env["registry_current_version"] = {
        name: winreg.QueryValueEx(key, name)[0]
        for name in ("ProductName", "DisplayVersion", "CurrentBuild", "UBR", "EditionID",
                     "BuildLabEx")}
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                             r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize")
        env["AppsUseLightTheme"] = winreg.QueryValueEx(key, "AppsUseLightTheme")[0]
    except OSError:
        env["AppsUseLightTheme"] = None
    size = struct.calcsize("IIP")  # HIGHCONTRASTW: cbSize, dwFlags, lpszDefaultScheme
    hcbuf = ctypes.create_string_buffer(struct.pack("IIP", size, 0, 0))
    USER32.SystemParametersInfoW(0x0042, ctypes.sizeof(hcbuf), hcbuf, 0)  # SPI_GETHIGHCONTRAST
    env["high_contrast_flags"] = struct.unpack_from("I", hcbuf.raw, 4)[0]
    env["computer_info"] = powershell_json(
        "Get-ComputerInfo | Select-Object WindowsProductName,WindowsVersion,OsName,OsVersion,"
        "OsBuildNumber,OsArchitecture,OsLanguage,CsManufacturer,CsModel,CsSystemType,"
        "CsHypervisorPresent,BiosManufacturer,BiosSMBIOSBIOSVersion,CsTotalPhysicalMemory,"
        "WindowsInstallDateFromRegistry | ConvertTo-Json -Compress")
    env["win32_computersystem"] = powershell_json(
        "Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer,Model,"
        "HypervisorPresent,SystemType,NumberOfLogicalProcessors | ConvertTo-Json -Compress")
    env["win32_processor"] = powershell_json(
        "Get-CimInstance Win32_Processor | Select-Object Name | ConvertTo-Json -Compress")
    env["win32_videocontroller"] = powershell_json(
        "Get-CimInstance Win32_VideoController | Select-Object Name,CurrentHorizontalResolution,"
        "CurrentVerticalResolution | ConvertTo-Json -Compress")
    return env


def probe_series(arch, exe, args, label, scratch):
    runs = []
    lines = []
    for n in range(1, RUNS + 1):
        probe_file = Path(scratch) / "loomora-probe.json"
        if probe_file.exists():
            probe_file.unlink()
        before = loomora_processes()
        start = time.time()
        res = subprocess.run([exe] + args, cwd=scratch, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=30)
        wall = time.time() - start
        line = probe_file.read_bytes().decode("utf-8") if probe_file.exists() else ""
        problems = check_probe.check_probe_line(line.strip()) if line else ["no probe file"]
        record = json.loads(line) if line and not problems else {}
        runs.append({
            "run": n, "exit_status": res.returncode, "stdout_bytes": len(res.stdout),
            "stderr_bytes": len(res.stderr), "wall_ms": round(wall * 1000),
            "probe_file_bytes": len(line.encode("utf-8")), "problems": problems,
            "message": record.get("message"), "greeting_matches": record.get("greeting_matches"),
            "display_ms": record.get("display_ms"), "peak_rss_kb": record.get("peak_rss_kb"),
            "platform_id": record.get("platform_id"), "disabled_features": record.get("disabled_features"),
            "eol_warning_shown": record.get("eol_warning_shown"),
            "loomora_processes_before": before, "loomora_processes_after": loomora_processes(),
        })
        lines.append(line.strip())
    series = HERE / ("probe-%s-%s-runs.jsonl" % (label, arch))
    series.write_text("".join(l + "\n" for l in lines), encoding="utf-8", newline="\n")
    (HERE / ("probe-%s-%s-run1.json" % (label, arch))).write_text(lines[0] + "\n", encoding="utf-8",
                                                                    newline="\n")
    return runs


def gui_run(arch, exe, scratch):
    result = {"arch": arch}
    before = loomora_processes()
    start = time.time()
    proc = subprocess.Popen([exe], cwd=scratch)
    top = None
    greeting = None
    deadline = start + 15
    while time.time() < deadline:
        wins = windows_of(proc.pid)
        if wins:
            top = wins[0]
            kids = children_of(top)
            edits = [k for k in kids if class_of(k) == "Edit"]
            statics = [k for k in kids if class_of(k) == "Static"]
            if edits and statics and text_of(statics[-1]):
                greeting = text_of(statics[-1])
                result["greeting_seen_after_ms"] = round((time.time() - start) * 1000)
                break
        time.sleep(0.005)
    if top is None or greeting is None:
        proc.kill()
        result["error"] = "window or greeting not found"
        return result
    edit, static = edits[0], statics[-1]
    result["pid"] = proc.pid
    result["window_title"] = (lambda b: (USER32.GetWindowTextW(top, b, 256), b.value)[1])(
        ctypes.create_unicode_buffer(256))
    result["window_class"] = class_of(top)
    result["children"] = [{"class": class_of(k), "text": text_of(k)} for k in children_of(top)]
    result["initial"] = {"edit": text_of(edit), "static": greeting}
    result["em_getlimittext"] = USER32.SendMessageW(edit, EM_GETLIMITTEXT, 0, 0)
    result["screenshot_initial"] = list(capture(top, HERE / ("window-%s-1-initial.png" % arch)))

    USER32.SendMessageW(edit, EM_SETSEL, 0, -1)
    t0 = time.time()
    for ch in "Marie":
        USER32.SendMessageW(edit, WM_CHAR, ord(ch), 0)
    after = text_of(static)
    result["typed_marie"] = {"edit": text_of(edit), "static": after,
                             "ms_from_first_char_to_static_read": round((time.time() - t0) * 1000, 2)}
    result["screenshot_marie"] = list(capture(top, HERE / ("window-%s-2-typed-marie.png" % arch)))

    USER32.SendMessageW(edit, EM_SETSEL, 0, -1)
    for _ in range(1100):
        USER32.SendMessageW(edit, WM_CHAR, ord("x"), 0)
    result["typed_1100_x"] = {"edit_length": len(text_of(edit)), "static_length": len(text_of(static)),
                              "static_equals_prefix_plus_edit": text_of(static) == "Bonjour " + text_of(edit)}

    t1 = time.time()
    USER32.PostMessageW(top, WM_CLOSE, 0, 0)
    try:
        code = proc.wait(timeout=10)
        result["exit_status"] = code
        result["close_to_exit_ms"] = round((time.time() - t1) * 1000)
    except subprocess.TimeoutExpired:
        proc.kill()
        result["exit_status"] = None
        result["error"] = "no exit within 10 s after WM_CLOSE"
    result["loomora_processes_before"] = before
    result["loomora_processes_after"] = loomora_processes()
    return result


def main():
    common.setup_utf8_output()
    trial = {"trial_date": time.strftime("%Y-%m-%d"), "executables": {}, "runs": {}}
    host = host_environment()
    common.write_json(HERE / "host-environment.json", host)
    for arch in ARCHS:
        source = ROOT / "out" / "windows-legacy" / arch / "loomora.exe"
        trial["executables"][arch] = {"path": source.relative_to(ROOT).as_posix(),
                                      "size_bytes": source.stat().st_size,
                                      "sha256": common.sha256(source)}
        scratch = tempfile.mkdtemp(prefix="loomora-trial-")
        try:
            exe = os.path.join(scratch, "loomora.exe")
            shutil.copyfile(str(source), exe)
            default = probe_series(arch, exe, ["--probe"], "default", scratch)
            marie = probe_series(arch, exe, ["--probe", "--probe-input", "Marie"], "marie", scratch)
            gui = gui_run(arch, exe, scratch)
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
        common.write_json(HERE / ("gui-%s.json" % arch), gui)
        trial["runs"][arch] = {"probe_default": default, "probe_marie": marie}
        allruns = default + marie
        trial["runs"][arch]["summary"] = {
            "runs": len(allruns),
            "exit_status_all_zero": all(r["exit_status"] == 0 for r in allruns),
            "stderr_all_empty": all(r["stderr_bytes"] == 0 for r in allruns),
            "stdout_all_empty": all(r["stdout_bytes"] == 0 for r in allruns),
            "no_problems": all(not r["problems"] for r in allruns),
            "no_leftover_process": all(r["loomora_processes_after"] == 0 for r in allruns),
            "platform_ids": sorted({r["platform_id"] for r in allruns}),
            "disabled_features_all_empty": all(r["disabled_features"] == [] for r in allruns),
            "eol_warning_shown_any": any(r["eol_warning_shown"] for r in allruns),
            "display_ms_default": stats([r["display_ms"] for r in default]),
            "display_ms_marie": stats([r["display_ms"] for r in marie]),
            "display_ms_all": stats([r["display_ms"] for r in allruns]),
            "peak_rss_kb_all": stats([r["peak_rss_kb"] for r in allruns]),
            "wall_ms_all": stats([r["wall_ms"] for r in allruns]),
            "messages_default": sorted({r["message"] for r in default}),
            "messages_marie": sorted({r["message"] for r in marie}),
        }
    common.write_json(HERE / "trial-measurements.json", trial)
    print(json.dumps({a: trial["runs"][a]["summary"] for a in ARCHS}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
