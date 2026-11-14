#!/usr/bin/env python3
"""Screenshot of the Loomora gtkmm window for a set of text vectors, run inside WSL. Derived from
glyphs-shoot.py, which stays as it was: the window is looked up by its title AND the _NET_WM_PID of the
process this script started (other windows titled "Loomora" may be on the same X server), X errors of
vanishing windows are ignored, and the vectors euro, greek and cyrillic are added.
usage: shoot.py <exe> <outdir> <tag>"""
import ctypes, ctypes.util, json, os, subprocess, sys, time
exe, outdir, tag = sys.argv[1:4]
os.environ["GDK_BACKEND"] = "x11"
import gi
gi.require_version("Gdk", "3.0"); gi.require_version("GdkX11", "3.0")
from gi.repository import Gdk, GdkX11

VECTORS = {
    "accents": "Zoé Hélène çà ñ ü ø ß",
    "euro": "Prix : 12,50 € (≈ 14 $)",
    "greek": "Γειά σου κόσμε, Αθήνα ΩΏ",
    "cyrillic": "Привет, мир! Ёжик Щука",
    "cjk": "日本語 中文 한국어",
    "emoji": "\U0001F600 \U0001F468‍\U0001F469‍\U0001F467 \U0001F1EB\U0001F1F7 \U0001F44D\U0001F3FD",
    "rtl": "שלום עולם مرحبا بالعالم",
    "combining": "é ä ñ ộ ก็ कि",
    "symbols": "€ © ™ → ← ∑ ∞ ≠ ┌─┐ ✓ ✂ ☺ ♥",
    "latinext": "Łódź Čeština Ğ ő ǅ Ɛ ẞ",
    "asc60": "abcdefghij " * 6,
    "asc120": "abcdefghij " * 12,
    "asc250": "abcdefghij " * 25,
    "cjk30": "日本語" * 10,
    "max1000": ("abcdefghi " * 100),
    "long": "Lorem ipsum dolor sit amet consectetur adipiscing elit " * 4 + "日本語" * 10,
}

xlib = ctypes.CDLL(ctypes.util.find_library("X11"))
xlib.XOpenDisplay.restype = ctypes.c_void_p
xlib.XOpenDisplay.argtypes = [ctypes.c_char_p]
xlib.XDefaultRootWindow.restype = ctypes.c_ulong
xlib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
xlib.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                            ctypes.POINTER(ctypes.c_ulong),
                            ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint)]
xlib.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
c_void_p, c_ulong, c_int, c_uint = ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_uint
xlib.XInternAtom.restype = c_ulong
xlib.XInternAtom.argtypes = [c_void_p, ctypes.c_char_p, c_int]
xdpy = xlib.XOpenDisplay(None)
root = xlib.XDefaultRootWindow(xdpy)

# ---- added for the gtkmm trial: window lookup by process id, X errors ignored ------------------
_XErrorHandler = ctypes.CFUNCTYPE(c_int, c_void_p, c_void_p)
_ignore_x_errors = _XErrorHandler(lambda display, event: 0)  # a window that vanishes is not fatal
xlib.XSetErrorHandler(_ignore_x_errors)
xlib.XGetWindowProperty.argtypes = [c_void_p, c_ulong, c_ulong, ctypes.c_long, ctypes.c_long, c_int,
                                    c_ulong, ctypes.POINTER(c_ulong), ctypes.POINTER(c_int),
                                    ctypes.POINTER(c_ulong), ctypes.POINTER(c_ulong),
                                    ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte))]
xlib.XFree.argtypes = [c_void_p]


def window_pid(win):
    """_NET_WM_PID of a window, or None."""
    atom = xlib.XInternAtom(xdpy, b"_NET_WM_PID", 0)
    kind, fmt, count, after = c_ulong(), c_int(), c_ulong(), c_ulong()
    data = ctypes.POINTER(ctypes.c_ubyte)()
    rc = xlib.XGetWindowProperty(xdpy, win, atom, 0, 1, 0, 6, ctypes.byref(kind), ctypes.byref(fmt),
                                 ctypes.byref(count), ctypes.byref(after), ctypes.byref(data))
    pid = None
    if rc == 0 and data and count.value == 1 and fmt.value == 32:
        pid = ctypes.cast(data, ctypes.POINTER(c_ulong))[0]
    if data:
        xlib.XFree(data)
    return pid


def find_pid(win, pid):
    r, p = c_ulong(), c_ulong()
    kids = ctypes.POINTER(c_ulong)(); n = c_uint()
    if not xlib.XQueryTree(xdpy, win, ctypes.byref(r), ctypes.byref(p), ctypes.byref(kids), ctypes.byref(n)):
        return None
    ch = [kids[i] for i in range(n.value)]
    for c in ch:
        nm = ctypes.c_char_p()
        if window_pid(c) == pid and xlib.XFetchName(xdpy, c, ctypes.byref(nm)) and nm.value == b"Loomora":
            return c  # the toplevel: the client leader window has the pid but no title
    for c in ch:
        f = find_pid(c, pid)
        if f:
            return f
    return None
# -----------------------------------------------------------------------------------------------

def find(win, name):
    r, p = ctypes.c_ulong(), ctypes.c_ulong()
    kids = ctypes.POINTER(ctypes.c_ulong)(); n = ctypes.c_uint()
    if not xlib.XQueryTree(xdpy, win, ctypes.byref(r), ctypes.byref(p), ctypes.byref(kids), ctypes.byref(n)):
        return None
    ch = [kids[i] for i in range(n.value)]
    for c in ch:
        nm = ctypes.c_char_p()
        if xlib.XFetchName(xdpy, c, ctypes.byref(nm)) and nm.value == name:
            return c
    for c in ch:
        f = find(c, name)
        if f:
            return f
    return None

only = sys.argv[4:] or list(VECTORS)
res = {}
for name in only:
    text = VECTORS[name]
    p = subprocess.Popen([exe, "--probe", "--probe-input", text], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    xid = None
    for _ in range(100):
        xid = find_pid(root, p.pid)
        if xid:
            break
        time.sleep(0.05)
    time.sleep(0.8)
    gw = GdkX11.X11Window.foreign_new_for_display(Gdk.Display.get_default(), xid)
    w, h = gw.get_width(), gw.get_height()
    pb = Gdk.pixbuf_get_from_window(gw, 0, 0, w, h)
    path = os.path.join(outdir, "%s-%s.png" % (tag, name))
    pb.savev(path, "png", [], [])
    out, err = p.communicate(timeout=10)
    res[name] = {"png": path, "size": [w, h], "stderr": err.decode("utf-8", "replace"),
                 "probe": json.loads(out.decode()) if out.strip() else None}
print(json.dumps(res, ensure_ascii=False, indent=1))
