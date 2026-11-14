/* Loomora Legacy UI for Linux: plain C and Xlib (libX11 is the only toolkit), no GTK, no
   FreeType, no input method.

   One input line pre-filled "Coucou" (limited to 1,000 bytes) and the greeting returned by the
   C++ backend, wrapped and centred below it, drawn with the core X11 font `fixed`; an
   end-of-life banner at the top when the backend says this kernel is no longer maintained. This
   file only creates the window, runs the event loop, converts between the keys, the font codes
   and UTF-8, and draws; the greeting, the end-of-life test and its text and the probe line all
   come from the backend through loomora.h. Probe mode is in probe.c.

   The code is C89 (comments, declarations at the top of blocks); the only newer thing is
   <stdint.h> through loomora.h. Nothing newer than glibc 2.12 is called.

   Text that is not Latin-1 (known limits, none of them hidden): the input is UTF-8; the core
   font draws what it has. If `fixed` is an ISO 10646-1 font (CHARSET_REGISTRY/ENCODING of the
   font) code points of the Basic Multilingual Plane are looked up in it with XDrawString16,
   otherwise the font is taken as ISO 8859-1 and only U+0020..U+00FF can be drawn. A code point
   the font does not have (a missing glyph, a control character, a code point above U+FFFF, an
   invalid byte) is drawn as '?'; when that happens the UI reports "glyph-substitution" in
   `disabled_features` of the probe line. No bidirectional layout, no shaping, no combining
   marks, no double-width alignment: a Core font has none of these. Keys: XLookupString gives the
   Latin-1 characters; for the other keysyms only the Unicode keysyms (0x01000100..0x0110FFFF)
   and the currency keysyms 0x20A0..0x20AC are converted, so no dead key, compose sequence or
   input method, and no paste. The caret is always at the end: Backspace (or Delete) deletes
   the last character. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/utsname.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "loomora.h"
#include "probe.h"

#define MAX_INPUT_BYTES 1000
/* Largest greeting: 4,096 bytes of input at most, plus "Bonjour ". */
#define GREETING_CAP (4096 + 64)
#define MARGIN 12
#define BANNER_PAD 8
#define FIELD_PAD 5
#define START_WIDTH 480
#define START_HEIGHT 320
#define NO_SPACE ((size_t)-1)

typedef struct {
    Display *dpy;
    int screen;
    Window win;
    GC gc;
    XFontStruct *font;
    int unicode_font; /* 1: the font is ISO 10646-1, 0: it is taken as ISO 8859-1 */
    unsigned long white;
    unsigned long black;
    unsigned long banner_bg;
    int width;
    int height;
    int eol;
    char input[MAX_INPUT_BYTES + 1];
    size_t input_len;
    char greeting[GREETING_CAP + 1];
    size_t greeting_len;
    int substituted; /* the greeting holds a character the font cannot draw */
    int quit;
} Ui;

typedef struct {
    size_t start;
    size_t end; /* exclusive */
} Line;

typedef struct {
    unsigned long *cps;    /* code points of the text */
    unsigned long *glyphs; /* code drawn for each of them (a '?' for the ones the font lacks) */
    Line *lines;
    size_t n;
    size_t nlines;
    size_t substituted;
} Block;

/* ---- UTF-8 ------------------------------------------------------------------------------- */

/* Decodes one code point at s[0..len). Returns the number of bytes used, or 0 when the bytes
   there are not valid UTF-8 (overlong form, surrogate, above U+10FFFF, truncated). */
static size_t utf8_next(const unsigned char *s, size_t len, unsigned long *cp) {
    unsigned long c;
    size_t need;
    size_t i;
    if (len == 0)
        return 0;
    c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        need = 1;
        c &= 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 2;
        c &= 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 3;
        c &= 0x07;
    } else {
        return 0;
    }
    if (len < need + 1)
        return 0;
    for (i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        c = (c << 6) | (unsigned long)(s[i] & 0x3F);
    }
    if ((need == 2 && c < 0x800) || (need == 3 && c < 0x10000) || c > 0x10FFFF ||
        (c >= 0xD800 && c <= 0xDFFF))
        return 0;
    *cp = c;
    return need + 1;
}

static int utf8_valid(const char *s) {
    size_t len = strlen(s);
    size_t i = 0;
    unsigned long cp;
    while (i < len) {
        size_t used = utf8_next((const unsigned char *)s + i, len - i, &cp);
        if (used == 0)
            return 0;
        i += used;
    }
    return 1;
}

/* Encodes cp (at most U+10FFFF) into out[0..4); returns the number of bytes. */
static size_t utf8_encode(unsigned long cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- the core font ----------------------------------------------------------------------- */

static int equals_nocase(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char x = *a;
        char y = *b;
        if (x >= 'a' && x <= 'z')
            x = (char)(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z')
            y = (char)(y - 'a' + 'A');
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* Value of the atom-valued font property `name`, copied into out (empty when absent). */
static void font_atom_property(Display *dpy, XFontStruct *font, const char *name, char *out,
                               size_t cap) {
    Atom prop = XInternAtom(dpy, name, True);
    unsigned long value = 0;
    out[0] = '\0';
    if (prop != None && XGetFontProperty(font, prop, &value) && value != 0) {
        char *text = XGetAtomName(dpy, (Atom)value);
        if (text != NULL) {
            strncpy(out, text, cap - 1);
            out[cap - 1] = '\0';
            XFree(text);
        }
    }
}

/* 1 when the font is indexed by Unicode (CHARSET_REGISTRY ISO10646, CHARSET_ENCODING 1). */
static int font_is_unicode(Display *dpy, XFontStruct *font) {
    char registry[64];
    char encoding[64];
    font_atom_property(dpy, font, "CHARSET_REGISTRY", registry, sizeof(registry));
    font_atom_property(dpy, font, "CHARSET_ENCODING", encoding, sizeof(encoding));
    return equals_nocase(registry, "ISO10646") && equals_nocase(encoding, "1");
}

/* Index of a character in font->per_char, or -1 when the font has no slot for it. */
static long glyph_index(const XFontStruct *f, unsigned long code) {
    unsigned int b1 = (unsigned int)(code >> 8);
    unsigned int b2 = (unsigned int)(code & 0xFF);
    if (b1 < f->min_byte1 || b1 > f->max_byte1 || b2 < f->min_char_or_byte2 ||
        b2 > f->max_char_or_byte2)
        return -1;
    return (long)(b1 - f->min_byte1) * (long)(f->max_char_or_byte2 - f->min_char_or_byte2 + 1) +
           (long)(b2 - f->min_char_or_byte2);
}

/* 1 when the font can draw this code point. */
static int glyph_exists(const Ui *ui, unsigned long cp) {
    const XFontStruct *f = ui->font;
    long idx;
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp > 0xFFFF)
        return 0;
    if (!ui->unicode_font && cp > 0xFF)
        return 0;
    idx = glyph_index(f, cp);
    if (idx < 0)
        return 0;
    if (f->per_char == NULL)
        return 1;
    /* A character the font does not have has all-zero metrics (Xlib, XFontStruct). */
    return !(f->per_char[idx].width == 0 && f->per_char[idx].lbearing == 0 &&
             f->per_char[idx].rbearing == 0 && f->per_char[idx].ascent == 0 &&
             f->per_char[idx].descent == 0);
}

static int glyph_width(const Ui *ui, unsigned long code) {
    const XFontStruct *f = ui->font;
    long idx;
    if (f->per_char == NULL)
        return f->max_bounds.width;
    idx = glyph_index(f, code);
    return idx < 0 ? f->max_bounds.width : f->per_char[idx].width;
}

static int line_height(const Ui *ui) { return ui->font->ascent + ui->font->descent + 2; }

/* ---- text blocks: decoded, substituted and wrapped --------------------------------------- */

static void block_free(Block *b) {
    free(b->cps);
    free(b->glyphs);
    free(b->lines);
    memset(b, 0, sizeof(*b));
}

static int text_width(const Ui *ui, const Block *b, size_t from, size_t to) {
    int w = 0;
    size_t i;
    for (i = from; i < to; i++)
        w += glyph_width(ui, b->glyphs[i]);
    return w;
}

/* Breaks the glyphs into lines of at most `avail` pixels: on the last space when there is one
   in the line, inside a word otherwise, and at a newline (when newline_breaks). A line holds at
   least one glyph, so a window narrower than a character cannot loop. */
static void block_wrap(const Ui *ui, Block *b, int avail, int newline_breaks) {
    size_t i = 0;
    b->nlines = 0;
    while (i < b->n) {
        size_t start = i;
        size_t j = i;
        size_t last_space = NO_SPACE;
        int w = 0;
        int hard_break = 0;
        int soft_break = 0;
        while (j < b->n) {
            int cw;
            if (newline_breaks && b->cps[j] == 0x0A) {
                hard_break = 1;
                break;
            }
            cw = glyph_width(ui, b->glyphs[j]);
            if (w + cw > avail && j > start) {
                soft_break = 1;
                break;
            }
            if (b->cps[j] == 0x20)
                last_space = j;
            w += cw;
            j++;
        }
        if (soft_break && last_space != NO_SPACE) {
            b->lines[b->nlines].start = start;
            b->lines[b->nlines].end = last_space;
            i = last_space + 1;
        } else {
            b->lines[b->nlines].start = start;
            b->lines[b->nlines].end = j;
            i = hard_break ? j + 1 : j;
        }
        b->nlines++;
    }
}

/* Decodes `utf8`, picks the code to draw for each character and wraps. Returns 0 (and an empty
   block) when memory is short. */
static int block_make(const Ui *ui, const char *utf8, size_t len, int avail, int newline_breaks,
                      Block *b) {
    size_t i = 0;
    memset(b, 0, sizeof(*b));
    b->cps = (unsigned long *)malloc((len + 1) * sizeof(unsigned long));
    b->glyphs = (unsigned long *)malloc((len + 1) * sizeof(unsigned long));
    b->lines = (Line *)malloc((len + 2) * sizeof(Line));
    if (b->cps == NULL || b->glyphs == NULL || b->lines == NULL) {
        block_free(b);
        return 0;
    }
    while (i < len) {
        unsigned long cp;
        size_t used = utf8_next((const unsigned char *)utf8 + i, len - i, &cp);
        if (used == 0) {
            cp = 0xFFFD; /* an invalid byte: one replacement character per byte */
            used = 1;
        }
        i += used;
        b->cps[b->n] = cp;
        if (newline_breaks && cp == 0x0A) {
            b->glyphs[b->n] = 0x20;
        } else if (glyph_exists(ui, cp)) {
            b->glyphs[b->n] = cp;
        } else {
            b->glyphs[b->n] = '?';
            b->substituted++;
        }
        b->n++;
    }
    block_wrap(ui, b, avail, newline_breaks);
    return 1;
}

static void draw_glyphs(const Ui *ui, const Block *b, size_t from, size_t to, int x, int y) {
    size_t count = to - from;
    size_t i;
    if (count == 0)
        return;
    if (ui->unicode_font) {
        XChar2b *text = (XChar2b *)malloc(count * sizeof(XChar2b));
        if (text == NULL)
            return;
        for (i = 0; i < count; i++) {
            text[i].byte1 = (unsigned char)(b->glyphs[from + i] >> 8);
            text[i].byte2 = (unsigned char)(b->glyphs[from + i] & 0xFF);
        }
        XDrawString16(ui->dpy, ui->win, ui->gc, x, y, text, (int)count);
        free(text);
    } else {
        char *text = (char *)malloc(count);
        if (text == NULL)
            return;
        for (i = 0; i < count; i++)
            text[i] = (char)b->glyphs[from + i];
        XDrawString(ui->dpy, ui->win, ui->gc, x, y, text, (int)count);
        free(text);
    }
}

/* Draws the lines of a block, each centred (or left aligned) in [left, left + avail], the
   first one with its top at `top`. Lines below the window are not sent. */
static void block_draw(const Ui *ui, const Block *b, int left, int avail, int top, int centred) {
    int lh = line_height(ui);
    size_t i;
    for (i = 0; i < b->nlines; i++) {
        int y = top + (int)i * lh;
        int w;
        int x = left;
        if (y > ui->height)
            break;
        w = text_width(ui, b, b->lines[i].start, b->lines[i].end);
        if (centred && w < avail)
            x = left + (avail - w) / 2;
        draw_glyphs(ui, b, b->lines[i].start, b->lines[i].end, x, y + ui->font->ascent + 1);
    }
}

/* ---- the window -------------------------------------------------------------------------- */

static void update_greeting(Ui *ui) {
    int32_t n = loomora_greet(ui->input, ui->input_len, (uint8_t *)ui->greeting, GREETING_CAP);
    Block b;
    if (n >= 0) {
        ui->greeting_len = (size_t)n;
        ui->greeting[n] = '\0';
    } else {
        sprintf(ui->greeting, "The greeting could not be computed (error %d).", (int)n);
        ui->greeting_len = strlen(ui->greeting);
    }
    ui->substituted = 0;
    if (block_make(ui, ui->greeting, ui->greeting_len, 1 << 20, 1, &b)) {
        ui->substituted = b.substituted > 0;
        block_free(&b);
    }
}

static void redraw(Ui *ui) {
    int lh = line_height(ui);
    int y = 0;
    int avail = ui->width - 2 * MARGIN;
    Block banner;
    Block field;
    Block greeting;
    int field_h = lh + 2 * FIELD_PAD;
    int field_text_w;
    size_t first;
    int shown_w;

    if (avail < 1)
        avail = 1;
    XClearWindow(ui->dpy, ui->win);
    XSetForeground(ui->dpy, ui->gc, ui->black);

    if (ui->eol && block_make(ui, loomora_eol_warning(), strlen(loomora_eol_warning()),
                              avail, 0, &banner)) {
        int banner_h = (int)banner.nlines * lh + 2 * BANNER_PAD;
        XSetForeground(ui->dpy, ui->gc, ui->banner_bg);
        XFillRectangle(ui->dpy, ui->win, ui->gc, 0, 0, (unsigned int)ui->width,
                       (unsigned int)banner_h);
        XSetForeground(ui->dpy, ui->gc, ui->black);
        block_draw(ui, &banner, MARGIN, avail, BANNER_PAD, 0);
        y = banner_h;
        block_free(&banner);
    }

    /* The input line: a frame, the end of the text when it does not fit, a caret. */
    y += MARGIN;
    XDrawRectangle(ui->dpy, ui->win, ui->gc, MARGIN, y, (unsigned int)(avail - 1),
                   (unsigned int)(field_h - 1));
    field_text_w = avail - 2 * FIELD_PAD - 2; /* room for the caret */
    if (field_text_w < 1)
        field_text_w = 1;
    if (block_make(ui, ui->input, ui->input_len, 1 << 20, 0, &field)) {
        first = field.n;
        shown_w = 0;
        while (first > 0) {
            int cw = glyph_width(ui, field.glyphs[first - 1]);
            if (shown_w + cw > field_text_w)
                break;
            shown_w += cw;
            first--;
        }
        draw_glyphs(ui, &field, first, field.n, MARGIN + FIELD_PAD + 1,
                    y + FIELD_PAD + ui->font->ascent + 1);
        XFillRectangle(ui->dpy, ui->win, ui->gc, MARGIN + FIELD_PAD + 1 + shown_w + 1,
                       y + FIELD_PAD, 1, (unsigned int)lh);
        block_free(&field);
    }
    y += field_h + MARGIN;

    /* The greeting: centred in the rest of the window, from the top when it does not fit. */
    if (block_make(ui, ui->greeting, ui->greeting_len, avail, 1, &greeting)) {
        int area_h = ui->height - MARGIN - y;
        int block_h = (int)greeting.nlines * lh;
        int top = area_h > block_h ? y + (area_h - block_h) / 2 : y;
        block_draw(ui, &greeting, MARGIN, avail, top, 1);
        block_free(&greeting);
    }
}

/* Deletes the last character of the input. */
static void delete_last(Ui *ui) {
    if (ui->input_len == 0)
        return;
    ui->input_len--;
    while (ui->input_len > 0 && ((unsigned char)ui->input[ui->input_len] & 0xC0) == 0x80)
        ui->input_len--;
    ui->input[ui->input_len] = '\0';
}

/* Appends one character; refused (silently) when it would pass 1,000 bytes. */
static int append_char(Ui *ui, unsigned long cp) {
    char bytes[4];
    size_t n;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    n = utf8_encode(cp, bytes);
    if (ui->input_len + n > MAX_INPUT_BYTES)
        return 0;
    memcpy(ui->input + ui->input_len, bytes, n);
    ui->input_len += n;
    ui->input[ui->input_len] = '\0';
    return 1;
}

/* A KeyPress: returns 1 when the input changed. */
static int handle_key(Ui *ui, XKeyEvent *ev) {
    char buf[32];
    KeySym sym = NoSymbol;
    int n = XLookupString(ev, buf, (int)sizeof(buf), &sym, NULL);
    int changed = 0;
    int i;
    if (sym == XK_BackSpace || sym == XK_Delete) {
        size_t before = ui->input_len;
        delete_last(ui);
        return ui->input_len != before;
    }
    if (ev->state & ControlMask)
        return 0;
    if (n > 0) {
        /* XLookupString returns Latin-1: a byte is the code point. */
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if ((c >= 0x20 && c < 0x7F) || c >= 0xA0)
                changed |= append_char(ui, c);
        }
    } else if (sym >= 0x01000100 && sym <= 0x0110FFFF) {
        changed = append_char(ui, (unsigned long)(sym - 0x01000000));
    } else if (sym >= 0x20A0 && sym <= 0x20AC) {
        changed = append_char(ui, (unsigned long)sym);
    }
    return changed;
}

static int on_x_error(Display *dpy, XErrorEvent *ev) {
    char text[128];
    XGetErrorText(dpy, ev->error_code, text, (int)sizeof(text));
    fprintf(stderr, "loomora: X error: %s (request %d)\n", text, (int)ev->request_code);
    return 0; /* carry on: a failed request must not end the program */
}

/* Major and minor kernel version, the version the backend's Linux end-of-life rule is expressed
   in. Returns 0 when it cannot be read. */
static int kernel_version(int *major, int *minor) {
    struct utsname un;
    if (uname(&un) != 0)
        return 0;
    return sscanf(un.release, "%d.%d", major, minor) == 2;
}

static unsigned long alloc_colour(Display *dpy, int screen, const char *name,
                                  unsigned long fallback) {
    XColor exact;
    XColor screen_def;
    if (XAllocNamedColor(dpy, DefaultColormap(dpy, screen), name, &screen_def, &exact))
        return screen_def.pixel;
    return fallback;
}

int main(int argc, char **argv) {
    ProbeOptions probe;
    Ui ui;
    XSetWindowAttributes attrs;
    XSizeHints hints;
    XClassHint class_hint;
    Atom wm_delete;
    int exit_status = 0;
    int kernel_major = 0;
    int kernel_minor = 0;
    const char *initial;
    size_t initial_len;

    probe_start();
    memset(&ui, 0, sizeof(ui));

    if (loomora_abi_version() != LOOMORA_ABI_VERSION) {
        fprintf(stderr, "loomora: backend ABI version %u, expected %d\n",
                (unsigned)loomora_abi_version(), LOOMORA_ABI_VERSION);
        return 1;
    }
    probe_parse_args(argc, argv, &probe);
    if (probe.bad_usage || (probe.input != NULL && !utf8_valid(probe.input))) {
        fprintf(stderr, "usage: %s [--probe [--probe-input <utf8>]]\n", argv[0]);
        return 2;
    }

    /* The initial text, cut to 1,000 bytes at a character boundary. */
    initial = probe.input != NULL ? probe.input : "Coucou";
    initial_len = strlen(initial);
    if (initial_len > MAX_INPUT_BYTES) {
        initial_len = MAX_INPUT_BYTES;
        while (initial_len > 0 && ((unsigned char)initial[initial_len] & 0xC0) == 0x80)
            initial_len--;
    }
    memcpy(ui.input, initial, initial_len);
    ui.input[initial_len] = '\0';
    ui.input_len = initial_len;

    ui.dpy = XOpenDisplay(NULL);
    if (ui.dpy == NULL) {
        fprintf(stderr, "loomora: cannot open the display\n");
        return 1;
    }
    XSetErrorHandler(on_x_error);
    ui.screen = DefaultScreen(ui.dpy);
    ui.white = WhitePixel(ui.dpy, ui.screen);
    ui.black = BlackPixel(ui.dpy, ui.screen);
    ui.banner_bg = alloc_colour(ui.dpy, ui.screen, "#FFE9A0", ui.white);

    ui.font = XLoadQueryFont(ui.dpy, "fixed");
    if (ui.font == NULL) {
        fprintf(stderr, "loomora: the core font \"fixed\" is not available\n");
        XCloseDisplay(ui.dpy);
        return 1;
    }
    ui.unicode_font = font_is_unicode(ui.dpy, ui.font);

    if (kernel_version(&kernel_major, &kernel_minor) &&
        loomora_is_eol(LOOMORA_FAMILY_LINUX, kernel_major, kernel_minor) == 1)
        ui.eol = 1;

    ui.width = START_WIDTH;
    ui.height = START_HEIGHT;
    attrs.background_pixel = ui.white;
    attrs.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;
    ui.win = XCreateWindow(ui.dpy, RootWindow(ui.dpy, ui.screen), 0, 0, (unsigned int)ui.width,
                           (unsigned int)ui.height, 0, CopyFromParent, InputOutput,
                           CopyFromParent, CWBackPixel | CWEventMask, &attrs);
    XStoreName(ui.dpy, ui.win, "Loomora");
    memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize;
    hints.min_width = 200;
    hints.min_height = 120;
    XSetWMNormalHints(ui.dpy, ui.win, &hints);
    class_hint.res_name = (char *)"loomora";
    class_hint.res_class = (char *)"Loomora";
    XSetClassHint(ui.dpy, ui.win, &class_hint);
    wm_delete = XInternAtom(ui.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(ui.dpy, ui.win, &wm_delete, 1);
    ui.gc = XCreateGC(ui.dpy, ui.win, 0, NULL);
    XSetFont(ui.dpy, ui.gc, ui.font->fid);
    XSetForeground(ui.dpy, ui.gc, ui.black);
    XSetBackground(ui.dpy, ui.gc, ui.white);

    update_greeting(&ui);
    XMapWindow(ui.dpy, ui.win);
    XFlush(ui.dpy);
    if (probe.enabled)
        probe_arm();

    while (!ui.quit) {
        long left;
        fd_set readable;
        struct timeval wait;
        int fd;
        int ready;

        while (!ui.quit && XPending(ui.dpy) > 0) {
            XEvent ev;
            XNextEvent(ui.dpy, &ev);
            if (ev.type == Expose) {
                if (ev.xexpose.count == 0) {
                    redraw(&ui);
                    XSync(ui.dpy, False);
                    probe_mark_displayed(); /* only the first call counts */
                }
            } else if (ev.type == ConfigureNotify) {
                if (ev.xconfigure.width != ui.width || ev.xconfigure.height != ui.height) {
                    ui.width = ev.xconfigure.width;
                    ui.height = ev.xconfigure.height;
                    redraw(&ui);
                    XFlush(ui.dpy);
                }
            } else if (ev.type == KeyPress) {
                if (handle_key(&ui, &ev.xkey)) {
                    update_greeting(&ui);
                    redraw(&ui);
                    XFlush(ui.dpy);
                }
            } else if (ev.type == MappingNotify) {
                XRefreshKeyboardMapping(&ev.xmapping); /* the keys changed their meaning */
            } else if (ev.type == ClientMessage) {
                if ((Atom)ev.xclient.data.l[0] == wm_delete)
                    ui.quit = 1;
            }
        }
        if (ui.quit)
            break;

        left = probe_ms_left();
        if (left == 0) {
            exit_status = probe_emit(ui.input, ui.greeting, ui.eol,
                                     ui.substituted ? "glyph-substitution" : "");
            break;
        }
        fd = ConnectionNumber(ui.dpy);
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        wait.tv_sec = left / 1000;
        wait.tv_usec = (left % 1000) * 1000;
        ready = select(fd + 1, &readable, NULL, NULL, left > 0 ? &wait : NULL);
        if (ready < 0 && errno != EINTR)
            break;
    }

    XFreeGC(ui.dpy, ui.gc);
    XFreeFont(ui.dpy, ui.font);
    XDestroyWindow(ui.dpy, ui.win);
    XCloseDisplay(ui.dpy);
    return exit_status;
}
