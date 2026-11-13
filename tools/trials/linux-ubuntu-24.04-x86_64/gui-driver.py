#!/usr/bin/env python3
"""Window driver of the Linux Modern trial, run inside WSL (WSLg, X11 backend under XWayland).

usage: python3 gui-driver.py <exe> <outdir> <suffix> <scenario> [scenario ...]

Starts <exe> WITHOUT --probe with GDK_BACKEND=x11 and drives the real window:
  * keys are REAL X11 key events injected with the XTEST extension (XTestFakeKeyEvent through
    libXtst), not XSendEvent: the X server delivers them to the focused window like a keyboard;
  * the mouse wheel is XTestFakeButtonEvent (buttons 4 and 5) over the greeting area;
  * a screenshot is the client area of the window read with Gdk (pixbuf_get_from_window);
  * the window is closed with a WM_DELETE_WINDOW client message (what a window manager's close
    button sends); exit status, standard error and leftover processes are recorded;
  * the contrast ratio of the greeting is measured on the screenshot with the WCAG formula: the
    band below the entry (y from 64 to the bottom border), the most frequent colour is the
    background, the pixel with the highest contrast against it is the text.

Scenarios (each starts its own process; results in <outdir>/gui-<scenario>-<suffix>.json and
PNGs named gui-<scenario>-<suffix>-<step>.png):
  startup              6 s without --probe on the X11 backend: still running, standard error, close
  startup-wayland      6 s without --probe on the default (Wayland) backend, closed with SIGTERM
  default|dark|highcontrast|highcontrast-inverse
                       theme (GTK_THEME unset, Adwaita:dark, HighContrast, HighContrastInverse):
                       initial shot, Ctrl+A, M a r i e, shot, measured delay, contrast, close
  limit                types 1,100 characters ("0001 0002 ... 0220 ", 5 characters per token)
                       into the field, shots at the top and at the bottom of the greeting
                       (mouse wheel), to see where the 1,000-character limit cuts
  eol                  LD_PRELOAD=<shim> (fake uname, see --preload): banner shot, then a click on
                       the close button of the banner, shot
Option: --preload <path of the .so that fakes uname> for the eol scenario.
Files are written with LF line endings; paths in the JSON are relative to <outdir>.
"""
import ctypes, ctypes.util, json, os, signal, subprocess, sys, time

args = sys.argv[1:]
preload = None
if "--preload" in args:
    i = args.index("--preload")
    preload = args[i + 1]
    del args[i:i + 2]
exe, outdir, suffix = args[:3]
scenarios = args[3:]

os.environ["GDK_BACKEND"] = "x11"  # the driver reads X11 windows
import gi
gi.require_version("Gdk", "3.0"); gi.require_version("GdkX11", "3.0")
from gi.repository import Gdk, GdkX11

xlib = ctypes.CDLL(ctypes.util.find_library("X11"))
xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
c_void_p, c_ulong, c_int, c_uint = ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_uint
xlib.XOpenDisplay.restype = c_void_p
xlib.XOpenDisplay.argtypes = [ctypes.c_char_p]
xlib.XDefaultRootWindow.restype = c_ulong
xlib.XDefaultRootWindow.argtypes = [c_void_p]
xlib.XQueryTree.argtypes = [c_void_p, c_ulong, ctypes.POINTER(c_ulong), ctypes.POINTER(c_ulong),
                            ctypes.POINTER(ctypes.POINTER(c_ulong)), ctypes.POINTER(c_uint)]
xlib.XFetchName.argtypes = [c_void_p, c_ulong, ctypes.POINTER(ctypes.c_char_p)]
xlib.XStringToKeysym.restype = c_ulong
xlib.XStringToKeysym.argtypes = [ctypes.c_char_p]
xlib.XKeysymToKeycode.restype = ctypes.c_ubyte
xlib.XKeysymToKeycode.argtypes = [c_void_p, c_ulong]
xlib.XSetInputFocus.argtypes = [c_void_p, c_ulong, c_int, c_ulong]
xlib.XInternAtom.restype = c_ulong
xlib.XInternAtom.argtypes = [c_void_p, ctypes.c_char_p, c_int]
xlib.XSendEvent.argtypes = [c_void_p, c_ulong, c_int, ctypes.c_long, c_void_p]
xlib.XTranslateCoordinates.argtypes = [c_void_p, c_ulong, c_ulong, c_int, c_int, ctypes.POINTER(c_int),
                                       ctypes.POINTER(c_int), ctypes.POINTER(c_ulong)]
xlib.XFlush.argtypes = [c_void_p]
xlib.XSync.argtypes = [c_void_p, c_int]
xtst.XTestFakeKeyEvent.argtypes = [c_void_p, c_uint, c_int, c_ulong]
xtst.XTestFakeButtonEvent.argtypes = [c_void_p, c_uint, c_int, c_ulong]
xtst.XTestFakeMotionEvent.argtypes = [c_void_p, c_int, c_int, c_int, c_ulong]

xdpy = xlib.XOpenDisplay(None)
root = xlib.XDefaultRootWindow(xdpy)


class XClientMessageEvent(ctypes.Structure):
    _fields_ = [("type", c_int), ("serial", c_ulong), ("send_event", c_int), ("display", c_void_p),
                ("window", c_ulong), ("message_type", c_ulong), ("format", c_int),
                ("data", ctypes.c_long * 5), ("pad", ctypes.c_char * 64)]


def find(win, name):
    r, p = c_ulong(), c_ulong()
    kids = ctypes.POINTER(c_ulong)(); n = c_uint()
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


def keycode(name):
    sym = xlib.XStringToKeysym(name.encode())
    code = xlib.XKeysymToKeycode(xdpy, sym)
    if not code:
        raise RuntimeError("no keycode for keysym " + name)
    return code


def key(code, press):
    xtst.XTestFakeKeyEvent(xdpy, code, 1 if press else 0, 0)
    xlib.XFlush(xdpy)


def tap(name, shift=False, ctrl=False, pause=0.03):
    mods = []
    if ctrl:
        mods.append(keycode("Control_L"))
    if shift:
        mods.append(keycode("Shift_L"))
    for m in mods:
        key(m, True)
    code = keycode(name)
    key(code, True); key(code, False)
    for m in reversed(mods):
        key(m, False)
    time.sleep(pause)


def type_text(text, pause=0.012):
    for ch in text:
        if ch == " ":
            tap("space", pause=pause)
        elif ch.isdigit():
            tap(ch, pause=pause)
        elif ch.isalpha() and ch.isascii():
            tap(ch.lower(), shift=ch.isupper(), pause=pause)
        else:
            raise RuntimeError("character not typeable with this driver: %r" % ch)


def move_pointer(x, y):
    xtst.XTestFakeMotionEvent(xdpy, -1, x, y, 0)
    xlib.XFlush(xdpy)


def click(x, y):
    move_pointer(x, y)
    time.sleep(0.1)
    xtst.XTestFakeButtonEvent(xdpy, 1, 1, 0); xlib.XFlush(xdpy)
    time.sleep(0.05)
    xtst.XTestFakeButtonEvent(xdpy, 1, 0, 0); xlib.XFlush(xdpy)


def wheel(x, y, clicks, down=True):
    move_pointer(x, y)
    time.sleep(0.1)
    for _ in range(clicks):
        b = 5 if down else 4
        xtst.XTestFakeButtonEvent(xdpy, b, 1, 0); xtst.XTestFakeButtonEvent(xdpy, b, 0, 0)
        xlib.XFlush(xdpy)
        time.sleep(0.03)


def window_origin(xid):
    x, y, child = c_int(), c_int(), c_ulong()
    xlib.XTranslateCoordinates(xdpy, xid, root, 0, 0, ctypes.byref(x), ctypes.byref(y), ctypes.byref(child))
    return x.value, y.value


def close_window(xid):
    ev = XClientMessageEvent()
    ev.type = 33  # ClientMessage
    ev.display = xdpy
    ev.window = xid
    ev.message_type = xlib.XInternAtom(xdpy, b"WM_PROTOCOLS", 0)
    ev.format = 32
    ev.data[0] = xlib.XInternAtom(xdpy, b"WM_DELETE_WINDOW", 0)
    ev.data[1] = 0
    xlib.XSendEvent(xdpy, xid, 0, 0, ctypes.byref(ev))
    xlib.XFlush(xdpy)


def lum(c):
    def f(v):
        v = v / 255.0
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2])


def ratio(a, b):
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def contrast(pb, y0=64, border=12):
    w, h, rs, nc = pb.get_width(), pb.get_height(), pb.get_rowstride(), pb.get_n_channels()
    px = pb.get_pixels()
    counts = {}
    for y in range(y0, h - border):
        for x in range(border, w - border):
            o = y * rs + x * nc
            c = (px[o], px[o + 1], px[o + 2])
            counts[c] = counts.get(c, 0) + 1
    bg = max(counts, key=counts.get)
    best, best_c = 0.0, bg
    for c in counts:
        r = ratio(c, bg)
        if r > best:
            best, best_c = r, c
    return {"background_rgb": list(bg), "text_rgb": list(best_c), "ratio": round(best, 2)}


def band(pb, y0=64):
    """Pixels of the greeting area (below the entry): the caret blink of the entry is not in it."""
    return pb.get_pixels()[y0 * pb.get_rowstride():]


def pgrep():
    name = os.path.basename(exe)[:15]
    return subprocess.run(["pgrep", "-x", name], stdout=subprocess.PIPE).stdout.decode().split()


class Run:
    def __init__(self, scenario, env_extra=None, wayland=False, argv=None):
        self.scenario = scenario
        env = dict(os.environ)
        env.pop("GTK_THEME", None)
        env.pop("LD_PRELOAD", None)
        if not wayland:
            env["GDK_BACKEND"] = "x11"
        else:
            env.pop("GDK_BACKEND", None)
        env.update(env_extra or {})
        self.env_note = {k: v for k, v in (env_extra or {}).items()}
        self.proc = subprocess.Popen([exe] + (argv or []), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.xid = None
        self.t_start = time.monotonic()
        self.shots = {}
        if not wayland:
            for _ in range(200):
                self.xid = find(root, b"Loomora")
                if self.xid:
                    break
                time.sleep(0.02)
            self.found_after_s = round(time.monotonic() - self.t_start, 3)
            time.sleep(0.6)
            xlib.XSetInputFocus(xdpy, self.xid, 1, 0)
            xlib.XSync(xdpy, 0)
            time.sleep(0.2)

    def pixbuf(self):
        gw = GdkX11.X11Window.foreign_new_for_display(Gdk.Display.get_default(), self.xid)
        return Gdk.pixbuf_get_from_window(gw, 0, 0, gw.get_width(), gw.get_height())

    def shot(self, step, measure=False):
        pb = self.pixbuf()
        name = "gui-%s-%s-%s.png" % (self.scenario, suffix, step)
        pb.savev(os.path.join(outdir, name), "png", [], [])
        info = {"png": name, "size": [pb.get_width(), pb.get_height()]}
        if measure:
            info["contrast"] = contrast(pb)
        self.shots[step] = info
        return pb, info

    def finish(self, via_delete=True):
        res = {"scenario": self.scenario, "env": self.env_note, "argv": [], "xid": hex(self.xid or 0)}
        still = self.proc.poll() is None
        res["still_running_before_close"] = still
        t0 = time.monotonic()
        if via_delete and self.xid:
            close_window(self.xid)
        else:
            self.proc.send_signal(signal.SIGTERM)
        try:
            rc = self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill(); rc = self.proc.wait()
            res["killed"] = True
        res["close_method"] = "WM_DELETE_WINDOW" if (via_delete and self.xid) else "SIGTERM"
        res["exit_status"] = rc
        res["close_to_exit_ms"] = round((time.monotonic() - t0) * 1000.0)
        res["stdout"] = self.proc.stdout.read().decode("utf-8", "replace")
        err = self.proc.stderr.read()
        res["stderr_bytes"] = len(err)
        res["stderr"] = err.decode("utf-8", "replace")
        time.sleep(0.2)
        res["leftover_after_exit"] = pgrep()
        res["shots"] = self.shots
        return res


def save(scenario, data):
    with open(os.path.join(outdir, "gui-%s-%s.json" % (scenario, suffix)), "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(data, indent=2, ensure_ascii=False) + "\n")


def scenario_startup():
    r = Run("startup")
    time.sleep(6.0)
    r.shot("after-6s", measure=True)
    res = r.finish()
    res["window_found_after_s"] = r.found_after_s
    res["observation_s"] = 6
    save("startup", res)


def scenario_startup_wayland():
    r = Run("startup-wayland", wayland=True)
    time.sleep(6.0)
    res = r.finish(via_delete=False)
    res["observation_s"] = 6
    res["note"] = "default GDK backend (Wayland under WSLg): no screenshot, closed with SIGTERM"
    save("startup-wayland", res)


THEMES = {"default": None, "dark": "Adwaita:dark", "highcontrast": "HighContrast",
          "highcontrast-inverse": "HighContrastInverse"}


def scenario_theme(name):
    theme = THEMES[name]
    r = Run(name, env_extra={"GTK_THEME": theme} if theme else None)
    r.shot("1-initial", measure=True)
    pb0 = band(r.pixbuf())
    t_first = time.monotonic()
    tap("a", ctrl=True)
    for ch in "Mari":
        tap(ch.lower(), shift=ch.isupper())
    time.sleep(0.4)
    base = band(r.pixbuf())
    # delay of the update: the key "e" is injected, then the window is captured about every 10 ms
    # until the greeting area (below the entry, so without the caret blink) differs from `base`
    t_key = time.monotonic()
    tap("e", pause=0)
    changed = None
    while time.monotonic() - t_key < 2.0:
        if band(r.pixbuf()) != base:
            changed = time.monotonic()
            break
    time.sleep(0.5)
    final = band(r.pixbuf())
    pb, info = r.shot("2-typed-marie", measure=True)
    res = r.finish()
    res["window_found_after_s"] = r.found_after_s
    res["theme"] = theme
    res["last_key_to_changed_greeting_pixels_ms"] = round((changed - t_key) * 1000.0) if changed else None
    res["delay_note"] = "upper bound: key injected with XTest, then polled captures (each capture costs a few ms)"
    res["greeting_area_changed_by_typing"] = pb0 != final
    save(name, res)


def scenario_limit():
    r = Run("limit")
    r.shot("1-initial")
    tap("a", ctrl=True)
    text = "".join("%04d " % i for i in range(1, 221))  # 1,100 characters
    t0 = time.monotonic()
    type_text(text)
    typing_s = round(time.monotonic() - t0, 2)
    time.sleep(0.8)
    r.shot("2-after-typing-top")
    ox, oy = window_origin(r.xid)
    wheel(ox + 240, oy + 150, 60, down=True)
    time.sleep(0.6)
    r.shot("3-greeting-scrolled-bottom")
    wheel(ox + 240, oy + 150, 60, down=False)
    time.sleep(0.6)
    r.shot("4-greeting-scrolled-back-top")
    res = r.finish()
    res["window_found_after_s"] = r.found_after_s
    res["typed_characters"] = len(text)
    res["typing_seconds"] = typing_s
    res["expectation"] = "token 0200 ends exactly at character 1000: the last visible token must be 0200 and 0201 must not appear"
    save("limit", res)


def scenario_eol():
    if not preload:
        sys.exit("eol needs --preload")
    r = Run("eol", env_extra={"LD_PRELOAD": preload})
    pb, info = r.shot("1-banner")
    w, h = info["size"]
    ox, oy = window_origin(r.xid)
    res_pre = {"window_size": [w, h]}
    # the close button of a GtkInfoBar is at the right end of the bar, near the top
    click(ox + w - 24, oy + 48)
    time.sleep(0.6)
    r.shot("2-after-click-close-button")
    res = r.finish()
    res["window_found_after_s"] = r.found_after_s
    res["close_button_click"] = [w - 24, 48]
    res["note"] = "uname faked by LD_PRELOAD=%s (simulation, not a real old kernel)" % os.path.basename(preload)
    res.update(res_pre)
    save("eol", res)


for s in scenarios:
    if s == "startup":
        scenario_startup()
    elif s == "startup-wayland":
        scenario_startup_wayland()
    elif s in THEMES:
        scenario_theme(s)
    elif s == "limit":
        scenario_limit()
    elif s == "eol":
        scenario_eol()
    else:
        sys.exit("unknown scenario " + s)
    print("done", s)
