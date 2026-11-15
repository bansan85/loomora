"""Screenshot (and optional typing check) of the Windows Modern UI (WinUI 3) on a Windows host.

  python tools/trials/windows-11-x64/winui-shoot.py EXE OUTPNG [--probe-input TEXT] [--dark]
                                                    [--type TEXT] [--wait SECONDS]

Tooling, not evidence: the script that produced the screenshots stored under
validation/evidence/winui3/windows-11-x64/. It starts EXE (the published Loomora.WinUI.exe) in
its own folder, waits for its top-level window, waits SECONDS (default 0.6) for the first frames
to be drawn, captures the window with PrintWindow (PW_RENDERFULLCONTENT) into OUTPNG, then
closes the window (WM_CLOSE) and prints one JSON object with the facts it measured.

--probe-input TEXT  starts the UI with `--probe --probe-input TEXT` (the window lives 2 seconds,
                    the capture is taken inside that window); without it the UI starts normally.
--dark              sets HKCU ...\\Themes\\Personalize\\AppsUseLightTheme to 0 for the duration
                    of the run and restores the old value.
--type TEXT         (normal start only) sends TEXT to the foreground window as keyboard input
                    (SendInput, Unicode) after selecting the field content with Ctrl+A, then
                    captures again into OUTPNG with "-typed" before the extension.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import struct
import subprocess
import sys
import time
import winreg
import zlib
from pathlib import Path

USER32 = ctypes.WinDLL("user32", use_last_error=True)
GDI32 = ctypes.WinDLL("gdi32", use_last_error=True)
KEY = r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
USER32.EnumWindows.argtypes = [WNDENUMPROC, wt.LPARAM]
USER32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
USER32.IsWindowVisible.argtypes = [wt.HWND]
USER32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
USER32.PrintWindow.argtypes = [wt.HWND, wt.HDC, wt.UINT]
USER32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
USER32.SetForegroundWindow.argtypes = [wt.HWND]
USER32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wt.DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wt.DWORD),
                ("biClrImportant", wt.DWORD)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD), ("time", wt.DWORD),
                ("dwExtraInfo", ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long), ("mouseData", wt.DWORD),
                ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wt.DWORD), ("u", _U)]


def windows_of(pid):
    found = []

    def callback(hwnd, _lparam):
        owner = wt.DWORD()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and USER32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(256)
            USER32.GetWindowTextW(hwnd, buf, 256)
            if buf.value == "Loomora":
                found.append(hwnd)
        return True

    USER32.EnumWindows(WNDENUMPROC(callback), 0)
    return found


def png_bytes(width, height, rows):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rows)
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
    # PrintWindow includes the invisible resize border of the window (black in the capture):
    # keep only the visible frame (DWMWA_EXTENDED_FRAME_BOUNDS).
    frame = wt.RECT()
    if ctypes.windll.dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(frame), ctypes.sizeof(frame)) == 0:
        left, top = max(frame.left - rect.left, 0), max(frame.top - rect.top, 0)
        right, bottom = min(frame.right - rect.left, width), min(frame.bottom - rect.top, height)
    else:
        left, top, right, bottom = 0, 0, width, height
    rows = []
    for y in range(top, bottom):
        line = data[(y * width + left) * 4:(y * width + right) * 4]
        rows.append(b"".join(line[i + 2:i + 3] + line[i + 1:i + 2] + line[i:i + 1]
                             for i in range(0, len(line), 4)))
    Path(path).write_bytes(png_bytes(right - left, bottom - top, rows))
    distinct = len(set(data[i:i + 4] for i in range(0, len(data), 4 * 7)))
    return right - left, bottom - top, distinct, contrast_of(data, width, left, top, right, bottom)


def luminance(bgra):
    def channel(v):
        v = v / 255.0
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    return 0.2126 * channel(bgra[2]) + 0.7152 * channel(bgra[1]) + 0.0722 * channel(bgra[0])


def contrast_of(data, width, left, top, right, bottom):
    """Contrast ratio (WCAG) between the background and the text of the greeting area: the most
    common colour of the lower half of the window and the colour farthest from it in luminance."""
    counts = {}
    y0 = top + (bottom - top) // 2
    for y in range(y0, bottom - 2):
        for x in range(left + 2, right - 2):
            pixel = data[(y * width + x) * 4:(y * width + x) * 4 + 3]
            counts[pixel] = counts.get(pixel, 0) + 1
    background = max(counts, key=counts.get)
    lb = luminance(background)
    far = max(counts, key=lambda c: abs(luminance(c) - lb))
    lf = luminance(far)
    hi, lo = max(lb, lf), min(lb, lf)
    return {"background_rgb": "#%02x%02x%02x" % (background[2], background[1], background[0]),
            "text_rgb": "#%02x%02x%02x" % (far[2], far[1], far[0]),
            "ratio": round((hi + 0.05) / (lo + 0.05), 2)}


def send_text(text):
    """Unicode keyboard input to the foreground window."""
    events = []
    units =struct.unpack("<%dH" % (len(text.encode("utf-16-le")) // 2), text.encode("utf-16-le"))
    for unit in units:
        for flags in (0x0004, 0x0004 | 0x0002):  # KEYEVENTF_UNICODE, then with KEYEVENTF_KEYUP
            item = INPUT(type=1)
            item.ki = KEYBDINPUT(0, unit, flags, 0, 0)
            events.append(item)
    array = (INPUT * len(events))(*events)
    USER32.SendInput(len(events), array, ctypes.sizeof(INPUT))


def key(vk, up):
    item = INPUT(type=1)
    item.ki = KEYBDINPUT(vk, 0, 0x0002 if up else 0, 0, 0)
    return item


def run(args):
    exe = Path(args.exe).resolve()
    cmd = [str(exe)]
    if args.probe_input is not None:
        cmd += ["--probe", "--probe-input", args.probe_input]
    started = time.time()
    proc = subprocess.Popen(cmd, cwd=str(exe.parent))
    top = None
    while time.time() - started < 20:
        wins = windows_of(proc.pid)
        if wins:
            top = wins[0]
            break
        time.sleep(0.01)
    if top is None:
        proc.kill()
        raise SystemExit("no window within 20 seconds")
    info = {"window_after_s": round(time.time() - started, 3), "command": cmd[1:]}
    time.sleep(args.wait)
    width, height, distinct, contrast = capture(top, args.out)
    info.update(png=str(args.out), width=width, height=height, sampled_distinct_pixels=distinct,
                contrast=contrast)
    if args.type is not None and args.probe_input is None:
        USER32.SetForegroundWindow(top)
        time.sleep(0.3)
        ctrl, a = 0x11, 0x41
        array = (INPUT * 4)(key(ctrl, False), key(a, False), key(a, True), key(ctrl, True))
        USER32.SendInput(4, array, ctypes.sizeof(INPUT))
        time.sleep(0.2)
        send_text(args.type)
        time.sleep(0.6)
        typed = Path(args.out).with_name(Path(args.out).stem + "-typed.png")
        capture(top, typed)
        info["typed_png"] = str(typed)
    if args.probe_input is None:
        USER32.PostMessageW(top, 0x0010, 0, 0)  # WM_CLOSE
    info["exit"] = proc.wait(timeout=20)
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("out")
    ap.add_argument("--probe-input")
    ap.add_argument("--dark", action="store_true")
    ap.add_argument("--type")
    ap.add_argument("--wait", type=float, default=0.6)
    args = ap.parse_args()
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    old = None
    try:
        if args.dark:
            k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_READ | winreg.KEY_SET_VALUE)
            old = winreg.QueryValueEx(k, "AppsUseLightTheme")[0]
            winreg.SetValueEx(k, "AppsUseLightTheme", 0, winreg.REG_DWORD, 0)
            winreg.CloseKey(k)
            time.sleep(0.5)
        info = run(args)
    finally:
        if old is not None:
            k = winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_SET_VALUE)
            winreg.SetValueEx(k, "AppsUseLightTheme", 0, winreg.REG_DWORD, old)
            winreg.CloseKey(k)
    info["dark"] = args.dark
    print(json.dumps(info, indent=1))


if __name__ == "__main__":
    sys.exit(main())
