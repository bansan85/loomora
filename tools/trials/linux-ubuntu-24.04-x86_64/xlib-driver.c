/* Test driver of the Linux Legacy (Xlib) UI: finds the window titled "Loomora" on $DISPLAY, then
   runs the commands given on the command line, in order:

     wait <ms>             sleep
     type <utf8 text>      send each character as a KeyPress/KeyRelease (SYNTHETIC XSendEvent, a
                           spare keycode remapped to the Unicode keysym; no physical keyboard)
     sym <hex>             same for one keysym given in hexadecimal (0x7e1 is Greek alpha)
     key <name>            a key by keysym name (BackSpace, Return, ...)
     shot <file.ppm>       the window contents, binary PPM (P6)
     size <w> <h>          resize the window
     close                 WM_DELETE_WINDOW

   The remapped keycode is freed again when the commands are done.

   Build: gcc -O2 -o xlib-driver xlib-driver.c -lX11. Prints one line per command. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

static Window find_window(Display *dpy, Window w, const char *title) {
    char *name = NULL;
    Window root, parent, *kids = NULL, found = 0;
    unsigned int n = 0, i;
    if (XFetchName(dpy, w, &name) && name != NULL) {
        int same = strcmp(name, title) == 0;
        XFree(name);
        if (same)
            return w;
    }
    if (!XQueryTree(dpy, w, &root, &parent, &kids, &n))
        return 0;
    for (i = 0; i < n && !found; i++)
        found = find_window(dpy, kids[i], title);
    if (kids != NULL)
        XFree(kids);
    return found;
}

static int spare_keycode(Display *dpy) {
    int min, max, kc, per;
    KeySym *map;
    int found = 0;
    XDisplayKeycodes(dpy, &min, &max);
    map = XGetKeyboardMapping(dpy, (KeyCode)min, max - min + 1, &per);
    for (kc = max; kc >= min && !found; kc--) {
        int i, empty = 1;
        for (i = 0; i < per; i++)
            if (map[(kc - min) * per + i] != NoSymbol)
                empty = 0;
        if (empty)
            found = kc;
    }
    XFree(map);
    return found;
}

static void send_key(Display *dpy, Window win, int keycode, KeySym sym, unsigned int state) {
    XEvent ev;
    KeySym syms[2];
    syms[0] = sym;
    syms[1] = sym;
    XChangeKeyboardMapping(dpy, keycode, 2, syms, 1);
    XSync(dpy, False);
    usleep(30000);
    memset(&ev, 0, sizeof(ev));
    ev.xkey.type = KeyPress;
    ev.xkey.display = dpy;
    ev.xkey.window = win;
    ev.xkey.root = DefaultRootWindow(dpy);
    ev.xkey.subwindow = None;
    ev.xkey.time = CurrentTime;
    ev.xkey.same_screen = True;
    ev.xkey.keycode = (unsigned int)keycode;
    ev.xkey.state = state;
    XSendEvent(dpy, win, True, KeyPressMask, &ev);
    ev.xkey.type = KeyRelease;
    XSendEvent(dpy, win, True, KeyReleaseMask, &ev);
    XSync(dpy, False);
    usleep(30000);
}

static int utf8_next(const unsigned char *s, unsigned long *cp) {
    int n = s[0] < 0x80 ? 1 : s[0] < 0xE0 ? 2 : s[0] < 0xF0 ? 3 : 4;
    unsigned long c = n == 1 ? s[0] : n == 2 ? (s[0] & 0x1F) : n == 3 ? (s[0] & 0x0F) : (s[0] & 7);
    int i;
    for (i = 1; i < n; i++)
        c = (c << 6) | (s[i] & 0x3F);
    *cp = c;
    return n;
}

static int shot(Display *dpy, Window win, const char *file) {
    XWindowAttributes at;
    XImage *img;
    FILE *f;
    int x, y;
    XGetWindowAttributes(dpy, win, &at);
    img = XGetImage(dpy, win, 0, 0, (unsigned)at.width, (unsigned)at.height, AllPlanes, ZPixmap);
    if (img == NULL)
        return 0;
    f = fopen(file, "wb");
    if (f == NULL)
        return 0;
    fprintf(f, "P6\n%d %d\n255\n", at.width, at.height);
    for (y = 0; y < at.height; y++)
        for (x = 0; x < at.width; x++) {
            unsigned long p = XGetPixel(img, x, y);
            fputc((int)((p >> 16) & 255), f);
            fputc((int)((p >> 8) & 255), f);
            fputc((int)(p & 255), f);
        }
    fclose(f);
    XDestroyImage(img);
    printf("shot %dx%d -> %s\n", at.width, at.height, file);
    return 1;
}

static void free_keycode(Display *dpy, int keycode) {
    KeySym none = NoSymbol;
    XChangeKeyboardMapping(dpy, keycode, 1, &none, 1);
    XSync(dpy, False);
}

int main(int argc, char **argv) {
    Display *dpy = XOpenDisplay(NULL);
    Window win = 0;
    int kc, i, tries;
    if (dpy == NULL) {
        fprintf(stderr, "no display\n");
        return 1;
    }
    for (tries = 0; tries < 100 && win == 0; tries++) {
        win = find_window(dpy, DefaultRootWindow(dpy), "Loomora");
        if (win == 0)
            usleep(100000);
    }
    if (win == 0) {
        fprintf(stderr, "window Loomora not found\n");
        return 1;
    }
    kc = spare_keycode(dpy);
    if (kc == 0) {
        fprintf(stderr, "no spare keycode\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        const char *c = argv[i];
        if (strcmp(c, "wait") == 0 && i + 1 < argc) {
            usleep((useconds_t)atoi(argv[++i]) * 1000);
        } else if (strcmp(c, "type") == 0 && i + 1 < argc) {
            const unsigned char *s = (const unsigned char *)argv[++i];
            int count = 0;
            while (*s != '\0') {
                unsigned long cp;
                s += utf8_next(s, &cp);
                send_key(dpy, win, kc,
                         cp < 0x100 && cp >= 0x20 && cp != 0x7F ? (KeySym)cp
                                                                : (KeySym)(0x01000000UL + cp),
                         0);
                count++;
            }
            printf("typed %d characters\n", count);
        } else if (strcmp(c, "sym") == 0 && i + 1 < argc) {
            send_key(dpy, win, kc, (KeySym)strtoul(argv[++i], NULL, 16), 0);
            printf("sym %s\n", argv[i]);
        } else if (strcmp(c, "key") == 0 && i + 1 < argc) {
            KeySym k = XStringToKeysym(argv[++i]);
            send_key(dpy, win, kc, k, 0);
            printf("key %s\n", argv[i]);
        } else if (strcmp(c, "shot") == 0 && i + 1 < argc) {
            XSync(dpy, False);
            usleep(150000);
            if (!shot(dpy, win, argv[++i]))
                return 1;
        } else if (strcmp(c, "size") == 0 && i + 2 < argc) {
            int w = atoi(argv[i + 1]), h = atoi(argv[i + 2]);
            i += 2;
            XResizeWindow(dpy, win, (unsigned)w, (unsigned)h);
            XSync(dpy, False);
            usleep(200000);
        } else if (strcmp(c, "close") == 0) {
            XEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.xclient.type = ClientMessage;
            ev.xclient.window = win;
            ev.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
            ev.xclient.format = 32;
            ev.xclient.data.l[0] = (long)XInternAtom(dpy, "WM_DELETE_WINDOW", False);
            ev.xclient.data.l[1] = CurrentTime;
            XSendEvent(dpy, win, False, NoEventMask, &ev);
            XSync(dpy, False);
            printf("close sent\n");
        } else {
            fprintf(stderr, "bad command %s\n", c);
            return 2;
        }
    }
    free_keycode(dpy, kc);
    XCloseDisplay(dpy);
    return 0;
}
