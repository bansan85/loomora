// Loomora Legacy UI for Linux: C++11 and Xlib (libX11 is the only library linked besides the C
// library and, in a native build, the C++ runtime), no GTK, no FreeType at link time.
//
// One input line pre-filled "Coucou" (limited to 1,000 characters, Unicode code points, as the
// GTK UI) and the greeting returned by the C++ backend, wrapped and centred below it; an
// end-of-life banner at the top when the backend says this kernel is no longer maintained. This
// file only creates the window, runs the event loop, converts between the keys, UTF-8 and the
// code points drawn, and draws; the greeting, the end-of-life test and its text and the probe
// line all come from the backend through loomora.h (the C API is the only link to the core).
// Probe mode is in probe.cpp, the Xft loader in xftdl.cpp, the key symbol table in keys.cpp.
//
// The Xlib resources (display, window, GC, core font, input method) and the Xft ones (font list,
// colour, drawing context) are owned by RAII classes, so every path out of main() releases them in
// the right order. No exception crosses the Xlib callback (OnXError) or the backend's C API; an
// exception that reaches main() (out of memory) ends the program with status 1. The program uses
// <cstdio>, <string> and <vector> only: no <iostream>, no <thread>, nothing that would need a
// C library newer than glibc 2.12.
//
// TEXT. Two paths, chosen once at start:
// * Xft (the normal path, xftdl.cpp): client-side fonts through libXft and fontconfig, both opened
//   at RUN TIME with dlopen (nothing links against them). Each code point is drawn with the first
//   font of fontconfig's list for "sans-serif" that has it, so Latin, Greek, Cyrillic, Hebrew,
//   Arabic, CJK, symbols and emoji come from whichever installed font covers them.
//   A code point NO installed font has is drawn as an empty box (a "tofu", drawn by this UI),
//   never as '?'; the probe line then carries "missing-glyph" in `disabled_features`. The
//   invisible format characters (zero width space and joiners U+200B..U+200F, bidi controls
//   U+202A..U+202E, U+2060..U+2064, variation selectors U+FE00..U+FE0F and U+E0100..U+E01EF,
//   U+034F, U+00AD, U+180B..U+180D, U+FEFF) take no room and draw nothing. THERE IS NO SHAPING
//   AND NO BIDIRECTIONAL LAYOUT: Xft draws the code points one after the other, in logical order,
//   from left to right. Arabic letters are therefore drawn in their isolated forms and
//   left to right, Hebrew and Arabic words are visually reversed, a combining mark is drawn
//   at the pen position of the character before it only when its font gives it a zero advance, a
//   ZWJ sequence (a family emoji, a flag) is drawn as its separate components, an Indic
//   conjunct is not formed.
// * Core font (no libXft or no fontconfig on the system, or LOOMORA_XLIB_NO_XFT set): the core X11
//   font `fixed`. If it is an ISO 10646-1 font code points of the Basic Multilingual Plane are
//   looked up in it with XDrawString16, otherwise it is taken as ISO 8859-1 and only U+0020..U+00FF
//   can be drawn. A code point the font does not have (a missing glyph, a control character, a
//   code point above U+FFFF, an invalid byte) is drawn as '?'; when that happens the UI reports
//   "glyph-substitution" in `disabled_features` of the probe line. This is the only case where
//   the substitution applies.
//
// KEYS. Xutf8LookupString through an X input method (XOpenIM, whatever the system provides, the
// built-in one included: dead keys and the Compose key work) when one opens; otherwise
// XLookupString (Latin-1) plus the key symbol to Unicode conversion of keys.cpp. No paste. The
// caret is always at the end: Backspace (or Delete) deletes the last character. The environment
// switches LOOMORA_XLIB_NO_XFT and LOOMORA_XLIB_NO_XIM force the core font and the no-input-method
// paths (tests).
#include <sys/select.h>
#include <sys/time.h>
#include <sys/utsname.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cerrno>
#include <clocale>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "keys.hpp"
#include "loomora.h"
#include "probe.hpp"
#include "xftdl.hpp"

namespace {

// ui-contract.md and spec.md ("Very long text"): 1,000 characters, counted as Unicode code points
// (the GTK UI counts the same way).
const std::size_t kMaxInputChars = 1000;
// Largest greeting: "Bonjour " and 4,000 bytes of input (the backend takes 4,096 at most).
const std::size_t kGreetingCap = 4096 + 64;
const int kMargin = 12;
const int kBannerPad = 8;
const int kFieldPad = 5;
const int kStartWidth = 480;
const int kStartHeight = 320;
const std::size_t kNoSpace = static_cast<std::size_t>(-1);
// A line is never wider than this when no wrapping is wanted (the input line, the measures).
const int kNoWrap = 1 << 20;

// What a character of a block is drawn with (Block::font); a value >= 0 is the index of the Xft
// font in the FontSet.
const int kFontTofu = -1; // an empty box: no installed font has the code point
const int kFontNone = -2; // nothing, no width: an invisible format character
const int kFontCore = -3; // the core font `fixed`, glyph code in Block::glyphs

// ---- UTF-8 ---------------------------------------------------------------------------------

// Decodes one code point at s[0..len). Returns the number of bytes used, or 0 when the bytes
// there are not valid UTF-8 (overlong form, surrogate, above U+10FFFF, truncated).
std::size_t Utf8Next(const unsigned char *s, std::size_t len, unsigned long *cp) {
    if (len == 0)
        return 0;
    unsigned long c = s[0];
    std::size_t need;
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
    for (std::size_t i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        c = (c << 6) | static_cast<unsigned long>(s[i] & 0x3F);
    }
    if ((need == 2 && c < 0x800) || (need == 3 && c < 0x10000) || c > 0x10FFFF ||
        (c >= 0xD800 && c <= 0xDFFF))
        return 0;
    *cp = c;
    return need + 1;
}

bool Utf8Valid(const char *s) {
    const std::size_t len = std::strlen(s);
    std::size_t i = 0;
    unsigned long cp;
    while (i < len) {
        const std::size_t used = Utf8Next(reinterpret_cast<const unsigned char *>(s) + i, len - i, &cp);
        if (used == 0)
            return false;
        i += used;
    }
    return true;
}

// Encodes cp (at most U+10FFFF) into out[0..4); returns the number of bytes.
std::size_t Utf8Encode(unsigned long cp, char *out) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

// ---- Xlib resources, released by their destructors -------------------------------------------

// The connection to the X server.
class XDisplay {
  public:
    XDisplay() : dpy_(nullptr) {}
    ~XDisplay() {
        if (dpy_ != nullptr)
            XCloseDisplay(dpy_);
    }
    XDisplay(const XDisplay &) = delete;
    XDisplay &operator=(const XDisplay &) = delete;

    // Connects to the display named by $DISPLAY; false when it cannot be opened.
    bool Open() {
        dpy_ = XOpenDisplay(nullptr);
        return dpy_ != nullptr;
    }
    Display *get() const { return dpy_; }

  private:
    Display *dpy_;
};

// The top-level window.
class XWindow {
  public:
    XWindow() : dpy_(nullptr), win_(None) {}
    ~XWindow() {
        if (win_ != None)
            XDestroyWindow(dpy_, win_);
    }
    XWindow(const XWindow &) = delete;
    XWindow &operator=(const XWindow &) = delete;

    void Create(Display *dpy, int width, int height, unsigned long background, long eventMask) {
        XSetWindowAttributes attrs;
        std::memset(&attrs, 0, sizeof(attrs));
        attrs.background_pixel = background;
        attrs.event_mask = eventMask;
        dpy_ = dpy;
        win_ = XCreateWindow(dpy, RootWindow(dpy, DefaultScreen(dpy)), 0, 0,
                             static_cast<unsigned int>(width), static_cast<unsigned int>(height),
                             0, CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWEventMask, &attrs);
    }
    Window id() const { return win_; }

  private:
    Display *dpy_;
    Window win_;
};

// The graphics context of the window.
class XGc {
  public:
    XGc() : dpy_(nullptr), gc_(nullptr) {}
    ~XGc() {
        if (gc_ != nullptr)
            XFreeGC(dpy_, gc_);
    }
    XGc(const XGc &) = delete;
    XGc &operator=(const XGc &) = delete;

    void Create(Display *dpy, Window win) {
        dpy_ = dpy;
        gc_ = XCreateGC(dpy, win, 0, nullptr);
    }
    GC get() const { return gc_; }

  private:
    Display *dpy_;
    GC gc_;
};

// An X input method and the input context of the window. Stays empty when there is no input
// method (then the keys are read with XLookupString and keys.cpp). The context is destroyed
// before the method.
class InputMethod {
  public:
    InputMethod() : xim_(nullptr), ic_(nullptr) {}
    ~InputMethod() {
        if (ic_ != nullptr)
            XDestroyIC(ic_);
        if (xim_ != nullptr)
            XCloseIM(xim_);
    }
    InputMethod(const InputMethod &) = delete;
    InputMethod &operator=(const InputMethod &) = delete;

    // Opens an X input method and a context for the window. LOOMORA_XLIB_NO_XIM in the
    // environment disables it (a switch for tests).
    void Open(Display *dpy, Window win) {
        if (std::getenv("LOOMORA_XLIB_NO_XIM") != nullptr)
            return;
        if (!XSupportsLocale())
            return;
        XSetLocaleModifiers("");
        xim_ = XOpenIM(dpy, nullptr, nullptr, nullptr);
        if (xim_ == nullptr)
            return;
        ic_ = XCreateIC(xim_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                        win, XNFocusWindow, win, static_cast<char *>(nullptr));
        if (ic_ == nullptr) {
            XCloseIM(xim_);
            xim_ = nullptr;
        }
    }
    XIC ic() const { return ic_; }

  private:
    XIM xim_;
    XIC ic_;
};

std::string AtomName(Display *dpy, Atom atom) {
    std::string name;
    char *text = XGetAtomName(dpy, atom);
    if (text != nullptr) {
        name = text;
        XFree(text);
    }
    return name;
}

bool EqualsNoCase(const std::string &a, const char *b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n)
        return false;
    for (std::size_t i = 0; i < n; i++) {
        char x = a[i];
        char y = b[i];
        if (x >= 'a' && x <= 'z')
            x = static_cast<char>(x - 'a' + 'A');
        if (y >= 'a' && y <= 'z')
            y = static_cast<char>(y - 'a' + 'A');
        if (x != y)
            return false;
    }
    return true;
}

// The core font `fixed` (no libXft): its glyph table and metrics.
class CoreFont {
  public:
    CoreFont() : dpy_(nullptr), font_(nullptr), unicode_(false) {}
    ~CoreFont() {
        if (font_ != nullptr)
            XFreeFont(dpy_, font_);
    }
    CoreFont(const CoreFont &) = delete;
    CoreFont &operator=(const CoreFont &) = delete;

    bool Load(Display *dpy, const char *name) {
        dpy_ = dpy;
        font_ = XLoadQueryFont(dpy, name);
        if (font_ == nullptr)
            return false;
        // Indexed by Unicode: CHARSET_REGISTRY ISO10646, CHARSET_ENCODING 1.
        unicode_ = EqualsNoCase(AtomProperty("CHARSET_REGISTRY"), "ISO10646") &&
                   EqualsNoCase(AtomProperty("CHARSET_ENCODING"), "1");
        return true;
    }
    bool loaded() const { return font_ != nullptr; }
    Font id() const { return font_->fid; }
    int ascent() const { return font_->ascent; }
    int descent() const { return font_->descent; }
    // true if it is ISO 10646-1, false if it is taken as ISO 8859-1.
    bool unicode() const { return unicode_; }

    // true when the font can draw this code point.
    bool HasGlyph(unsigned long cp) const {
        if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp > 0xFFFF)
            return false;
        if (!unicode_ && cp > 0xFF)
            return false;
        const long idx = GlyphIndex(cp);
        if (idx < 0)
            return false;
        if (font_->per_char == nullptr)
            return true;
        // A character the font does not have has all-zero metrics (Xlib, XFontStruct).
        const XCharStruct &c = font_->per_char[idx];
        return !(c.width == 0 && c.lbearing == 0 && c.rbearing == 0 && c.ascent == 0 &&
                 c.descent == 0);
    }

    int Width(unsigned long code) const {
        if (font_->per_char == nullptr)
            return font_->max_bounds.width;
        const long idx = GlyphIndex(code);
        return idx < 0 ? font_->max_bounds.width : font_->per_char[idx].width;
    }

  private:
    // Value of the atom-valued font property `name` (empty when absent).
    std::string AtomProperty(const char *name) const {
        const Atom prop = XInternAtom(dpy_, name, True);
        unsigned long value = 0;
        if (prop != None && XGetFontProperty(font_, prop, &value) && value != 0)
            return AtomName(dpy_, static_cast<Atom>(value));
        return std::string();
    }

    // Index of a character in font->per_char, or -1 when the font has no slot for it.
    long GlyphIndex(unsigned long code) const {
        const unsigned int b1 = static_cast<unsigned int>(code >> 8);
        const unsigned int b2 = static_cast<unsigned int>(code & 0xFF);
        if (b1 < font_->min_byte1 || b1 > font_->max_byte1 || b2 < font_->min_char_or_byte2 ||
            b2 > font_->max_char_or_byte2)
            return -1;
        return static_cast<long>(b1 - font_->min_byte1) *
                   static_cast<long>(font_->max_char_or_byte2 - font_->min_char_or_byte2 + 1) +
               static_cast<long>(b2 - font_->min_char_or_byte2);
    }

    Display *dpy_;
    XFontStruct *font_;
    bool unicode_;
};

// ---- text blocks ---------------------------------------------------------------------------

struct Line {
    std::size_t start;
    std::size_t end; // exclusive
};

// A text decoded, matched with a font and wrapped. The per-character vectors have the same size.
struct Block {
    std::vector<unsigned int> cps;     // code points of the text
    std::vector<unsigned int> glyphs;  // core font: code drawn for each (a '?' for the missing)
    std::vector<int> font;             // kFont* or Xft font index of each
    std::vector<int> adv;              // advance in pixels of each
    std::vector<Line> lines;
    std::size_t substituted = 0; // core font
    std::size_t missing = 0;     // Xft

    std::size_t size() const { return cps.size(); }
};

// true for the format characters that have no picture and no width of their own (see the top of
// this file): drawn as nothing instead of a box when no shaping is available.
bool IsInvisible(unsigned long cp) {
    return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           (cp >= 0xE0100 && cp <= 0xE01EF) || (cp >= 0x180B && cp <= 0x180D) || cp == 0x034F ||
           cp == 0x00AD || cp == 0xFEFF;
}

int TextWidth(const Block &b, std::size_t from, std::size_t to) {
    int w = 0;
    for (std::size_t i = from; i < to; i++)
        w += b.adv[i];
    return w;
}

// Breaks the glyphs into lines of at most `avail` pixels: on the last space when there is one in
// the line, inside a word otherwise, and at a newline (when newlineBreaks). A line holds at least
// one glyph, so a window narrower than a character cannot loop.
void WrapBlock(Block &b, int avail, bool newlineBreaks) {
    const std::size_t n = b.size();
    std::size_t i = 0;
    b.lines.clear();
    while (i < n) {
        const std::size_t start = i;
        std::size_t j = i;
        std::size_t lastSpace = kNoSpace;
        int w = 0;
        bool hardBreak = false;
        bool softBreak = false;
        while (j < n) {
            if (newlineBreaks && b.cps[j] == 0x0A) {
                hardBreak = true;
                break;
            }
            const int cw = b.adv[j];
            if (w + cw > avail && j > start) {
                softBreak = true;
                break;
            }
            if (b.cps[j] == 0x20)
                lastSpace = j;
            w += cw;
            j++;
        }
        Line line;
        line.start = start;
        if (softBreak && lastSpace != kNoSpace) {
            line.end = lastSpace;
            i = lastSpace + 1;
        } else {
            line.end = j;
            i = hardBreak ? j + 1 : j;
        }
        b.lines.push_back(line);
    }
}

// Xlib calls this from C: nothing may be thrown out of it.
extern "C" {
static int OnXError(Display *dpy, XErrorEvent *ev) {
    char text[128];
    XGetErrorText(dpy, ev->error_code, text, static_cast<int>(sizeof(text)));
    std::fprintf(stderr, "loomora: X error: %s (request %d)\n", text,
                 static_cast<int>(ev->request_code));
    return 0; // carry on: a failed request must not end the program
}
}

unsigned long AllocColour(Display *dpy, int screen, const char *name, unsigned long fallback) {
    XColor exact;
    XColor screenDef;
    if (XAllocNamedColor(dpy, DefaultColormap(dpy, screen), name, &screenDef, &exact))
        return screenDef.pixel;
    return fallback;
}

// Major and minor kernel version, the version the backend's Linux end-of-life rule is expressed
// in. Returns false when it cannot be read.
bool KernelVersion(int *major, int *minor) {
    utsname un;
    if (uname(&un) != 0)
        return false;
    return std::sscanf(un.release, "%d.%d", major, minor) == 2;
}

// ---- the window ----------------------------------------------------------------------------

class App {
  public:
    App() {}
    App(const App &) = delete;
    App &operator=(const App &) = delete;

    // Opens the display, the fonts, the window and the input method. Returns 0 on success, else
    // the exit status of the program (the message is already printed).
    int Init(const std::string &initialInput);
    // Event loop; returns the exit status.
    int Run(const probe::Options &probeOptions);

  private:
    int TextAscent() const { return fonts_ ? fonts_->Ascent() : core_.ascent(); }
    int LineHeight() const {
        const int asc = TextAscent();
        const int desc = fonts_ ? fonts_->Descent() : core_.descent();
        return asc + desc + 2;
    }

    Block MakeBlock(const std::string &utf8, int avail, bool newlineBreaks);
    void DrawGlyphs(const Block &b, std::size_t from, std::size_t to, int x, int y);
    void DrawBlock(const Block &b, int left, int avail, int top, bool centred);
    void UpdateGreeting();
    void Redraw();
    void DeleteLast();
    bool AppendChar(unsigned long cp);
    bool AppendPrintable(unsigned long cp);
    bool HandleKey(XKeyEvent *ev);

    XDisplay display_;
    XWindow window_;
    CoreFont core_; // loaded on the core font path only
    XGc gc_;
    // The Xft objects, valid when fonts_ is set; ink_ and draw_ must die before fonts_.
    std::unique_ptr<xftdl::FontSet> fonts_;
    std::unique_ptr<xftdl::InkColor> ink_;
    std::unique_ptr<xftdl::DrawHandle> draw_;
    InputMethod input_method_;
    Visual *visual_ = nullptr;
    Colormap cmap_ = 0;
    int screen_ = 0;
    unsigned long white_ = 0;
    unsigned long black_ = 0;
    unsigned long bannerBg_ = 0;
    int width_ = kStartWidth;
    int height_ = kStartHeight;
    bool eol_ = false;
    std::string input_;
    std::size_t inputChars_ = 0; // code points
    std::string greeting_;
    bool substituted_ = false; // core font: the greeting holds a character drawn as '?'
    bool missing_ = false;     // Xft: the greeting holds a character no font has (a box)
    bool quit_ = false;
};

// Decodes `utf8`, picks the font (or the code to draw) and the advance of each character and
// wraps.
Block App::MakeBlock(const std::string &utf8, int avail, bool newlineBreaks) {
    Block b;
    const std::size_t len = utf8.size();
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(utf8.data());
    b.cps.reserve(len);
    b.glyphs.reserve(len);
    b.font.reserve(len);
    b.adv.reserve(len);
    std::size_t i = 0;
    while (i < len) {
        unsigned long cp;
        std::size_t used = Utf8Next(bytes + i, len - i, &cp);
        if (used == 0) {
            cp = 0xFFFD; // an invalid byte: one replacement character per byte
            used = 1;
        }
        i += used;
        unsigned int glyph = static_cast<unsigned int>(cp);
        int font;
        int advance;
        if (newlineBreaks && cp == 0x0A) {
            font = kFontNone; // the break itself is made by WrapBlock
            advance = 0;
        } else if (fonts_) {
            int index = 0;
            if (IsInvisible(cp)) {
                font = kFontNone;
                advance = 0;
            } else if (fonts_->Pick(cp, &index) != nullptr) {
                font = index;
                advance = fonts_->Advance(index, cp);
            } else {
                font = kFontTofu;
                advance = fonts_->TofuWidth();
                b.missing++;
            }
        } else {
            font = kFontCore;
            if (!core_.HasGlyph(cp)) {
                glyph = '?';
                b.substituted++;
            }
            advance = core_.Width(glyph);
        }
        b.cps.push_back(static_cast<unsigned int>(cp));
        b.glyphs.push_back(glyph);
        b.font.push_back(font);
        b.adv.push_back(advance);
    }
    WrapBlock(b, avail, newlineBreaks);
    return b;
}

// Draws the glyphs from..to of a block with the pen at x and the baseline at y.
void App::DrawGlyphs(const Block &b, std::size_t from, std::size_t to, int x, int y) {
    Display *dpy = display_.get();
    std::size_t i = from;
    while (i < to) {
        const int f = b.font[i];
        std::size_t j = i + 1;
        int runW = b.adv[i];
        if (f == kFontNone) {
            // nothing to draw
        } else if (f == kFontTofu) {
            const int tw = fonts_->TofuWidth();
            const int th = fonts_->TofuHeight();
            XDrawRectangle(dpy, window_.id(), gc_.get(), x + 1, y - th + 1,
                           static_cast<unsigned int>(tw > 3 ? tw - 3 : 1),
                           static_cast<unsigned int>(th > 2 ? th - 2 : 1));
        } else if (f >= 0) {
            while (j < to && b.font[j] == f) {
                runW += b.adv[j];
                j++;
            }
            draw_->DrawString(*ink_, fonts_->Font(f), x, y, &b.cps[i], static_cast<int>(j - i));
        } else {
            // core font: the run of core glyphs
            while (j < to && b.font[j] == kFontCore) {
                runW += b.adv[j];
                j++;
            }
            const std::size_t count = j - i;
            if (core_.unicode()) {
                std::vector<XChar2b> text(count);
                for (std::size_t k = 0; k < count; k++) {
                    text[k].byte1 = static_cast<unsigned char>(b.glyphs[i + k] >> 8);
                    text[k].byte2 = static_cast<unsigned char>(b.glyphs[i + k] & 0xFF);
                }
                XDrawString16(dpy, window_.id(), gc_.get(), x, y, &text[0],
                              static_cast<int>(count));
            } else {
                std::string text(count, '\0');
                for (std::size_t k = 0; k < count; k++)
                    text[k] = static_cast<char>(b.glyphs[i + k]);
                XDrawString(dpy, window_.id(), gc_.get(), x, y, text.data(),
                            static_cast<int>(count));
            }
        }
        x += runW;
        i = j;
    }
}

// Draws the lines of a block, each centred (or left aligned) in [left, left + avail], the first
// one with its top at `top`. Lines below the window are not sent.
void App::DrawBlock(const Block &b, int left, int avail, int top, bool centred) {
    const int lh = LineHeight();
    for (std::size_t i = 0; i < b.lines.size(); i++) {
        const int y = top + static_cast<int>(i) * lh;
        int x = left;
        if (y > height_)
            break;
        const int w = TextWidth(b, b.lines[i].start, b.lines[i].end);
        if (centred && w < avail)
            x = left + (avail - w) / 2;
        DrawGlyphs(b, b.lines[i].start, b.lines[i].end, x, y + TextAscent() + 1);
    }
}

// Recomputes the greeting from the input. Also decodes the input and the end-of-life text once,
// so that every Xft font they need is opened (and the line height known) before the next redraw.
void App::UpdateGreeting() {
    std::string out(kGreetingCap + 1, '\0');
    const int32_t n = loomora_greet(input_.data(), input_.size(),
                                    reinterpret_cast<uint8_t *>(&out[0]), kGreetingCap);
    if (n >= 0) {
        out.resize(static_cast<std::size_t>(n));
        greeting_.swap(out);
    } else {
        char text[96];
        std::snprintf(text, sizeof(text), "The greeting could not be computed (error %d).",
                      static_cast<int>(n));
        greeting_ = text;
    }
    const Block g = MakeBlock(greeting_, kNoWrap, true);
    substituted_ = g.substituted > 0;
    missing_ = g.missing > 0;
    MakeBlock(input_, kNoWrap, false);
    if (eol_)
        MakeBlock(loomora_eol_warning(), kNoWrap, false);
}

void App::Redraw() {
    Display *dpy = display_.get();
    const int lh = LineHeight();
    int y = 0;
    int avail = width_ - 2 * kMargin;
    const int fieldH = lh + 2 * kFieldPad;

    if (avail < 1)
        avail = 1;
    XClearWindow(dpy, window_.id());
    XSetForeground(dpy, gc_.get(), black_);

    if (eol_) {
        const Block banner = MakeBlock(loomora_eol_warning(), avail, false);
        const int bannerH = static_cast<int>(banner.lines.size()) * lh + 2 * kBannerPad;
        XSetForeground(dpy, gc_.get(), bannerBg_);
        XFillRectangle(dpy, window_.id(), gc_.get(), 0, 0, static_cast<unsigned int>(width_),
                       static_cast<unsigned int>(bannerH));
        XSetForeground(dpy, gc_.get(), black_);
        DrawBlock(banner, kMargin, avail, kBannerPad, false);
        y = bannerH;
    }

    // The input line: a frame, the end of the text when it does not fit, a caret.
    y += kMargin;
    XDrawRectangle(dpy, window_.id(), gc_.get(), kMargin, y, static_cast<unsigned int>(avail - 1),
                   static_cast<unsigned int>(fieldH - 1));
    int fieldTextW = avail - 2 * kFieldPad - 2; // room for the caret
    if (fieldTextW < 1)
        fieldTextW = 1;
    {
        const Block field = MakeBlock(input_, kNoWrap, false);
        std::size_t first = field.size();
        int shownW = 0;
        while (first > 0) {
            const int cw = field.adv[first - 1];
            if (shownW + cw > fieldTextW)
                break;
            shownW += cw;
            first--;
        }
        DrawGlyphs(field, first, field.size(), kMargin + kFieldPad + 1,
                   y + kFieldPad + TextAscent() + 1);
        XFillRectangle(dpy, window_.id(), gc_.get(), kMargin + kFieldPad + 1 + shownW + 1,
                       y + kFieldPad, 1, static_cast<unsigned int>(lh));
    }
    y += fieldH + kMargin;

    // The greeting: centred in the rest of the window, from the top when it does not fit.
    {
        const Block greeting = MakeBlock(greeting_, avail, true);
        const int areaH = height_ - kMargin - y;
        const int blockH = static_cast<int>(greeting.lines.size()) * lh;
        const int top = areaH > blockH ? y + (areaH - blockH) / 2 : y;
        DrawBlock(greeting, kMargin, avail, top, true);
    }
}

// Deletes the last character of the input.
void App::DeleteLast() {
    if (input_.empty())
        return;
    std::size_t len = input_.size() - 1;
    while (len > 0 && (static_cast<unsigned char>(input_[len]) & 0xC0) == 0x80)
        len--;
    input_.resize(len);
    if (inputChars_ > 0)
        inputChars_--;
}

// Appends one character; refused (silently) when the input already holds 1,000 characters.
bool App::AppendChar(unsigned long cp) {
    char bytes[4];
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return false;
    if (inputChars_ >= kMaxInputChars)
        return false;
    const std::size_t n = Utf8Encode(cp, bytes);
    input_.append(bytes, n);
    inputChars_++;
    return true;
}

// Accepts a printable code point: not a control character (Backspace, Return, Escape, ...).
bool App::AppendPrintable(unsigned long cp) {
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F))
        return false;
    return AppendChar(cp);
}

// A KeyPress: returns true when the input changed.
bool App::HandleKey(XKeyEvent *ev) {
    char buf[64];
    KeySym sym = NoSymbol;
    Status status = 0;
    int n = 0;
    bool changed = false;
    XIC ic = input_method_.ic();

    if (ic != nullptr) {
        n = Xutf8LookupString(ic, ev, buf, static_cast<int>(sizeof(buf)) - 1, &sym, &status);
        if (status == XBufferOverflow || status == XLookupNone)
            return false;
        const bool haveSym = status == XLookupKeySym || status == XLookupBoth;
        if (status != XLookupChars && status != XLookupBoth)
            n = 0;
        if (!haveSym)
            sym = NoSymbol;
    } else {
        n = XLookupString(ev, buf, static_cast<int>(sizeof(buf)) - 1, &sym, nullptr);
    }
    if (sym == XK_BackSpace || sym == XK_Delete ||
        (n == 1 && (buf[0] == 0x08 || buf[0] == 0x7F))) {
        const std::size_t before = input_.size();
        DeleteLast();
        return input_.size() != before;
    }
    if (ev->state & ControlMask)
        return false;
    if (n > 0) {
        if (ic != nullptr) {
            // UTF-8 text: one key can give several characters (a compose sequence).
            std::size_t pos = 0;
            while (pos < static_cast<std::size_t>(n)) {
                unsigned long cp;
                const std::size_t used = Utf8Next(reinterpret_cast<const unsigned char *>(buf) + pos,
                                                  static_cast<std::size_t>(n) - pos, &cp);
                if (used == 0)
                    break;
                changed |= AppendPrintable(cp);
                pos += used;
            }
        } else {
            // No input method. What XLookupString returns beyond ASCII depends on the locale
            // (Latin-1 bytes in the C locale, UTF-8 in a UTF-8 one), so only the key symbol is
            // trusted for those: keys.cpp converts it. The ASCII bytes are kept for the keys
            // whose symbol keys.cpp does not know (the keypad).
            const unsigned long cp = keys::KeysymToUcs(static_cast<unsigned long>(sym));
            if (cp != 0) {
                changed = AppendPrintable(cp);
            } else {
                for (int i = 0; i < n; i++)
                    if (static_cast<unsigned char>(buf[i]) < 0x80)
                        changed |= AppendPrintable(static_cast<unsigned char>(buf[i]));
            }
        }
    } else if (sym != NoSymbol) {
        const unsigned long cp = keys::KeysymToUcs(static_cast<unsigned long>(sym));
        if (cp != 0)
            changed = AppendPrintable(cp);
    }
    return changed;
}

int App::Init(const std::string &initialInput) {
    // The initial text, cut to 1,000 characters (code points) at a character boundary.
    {
        const std::size_t total = initialInput.size();
        const unsigned char *bytes = reinterpret_cast<const unsigned char *>(initialInput.data());
        std::size_t len = 0;
        std::size_t chars = 0;
        while (len < total && chars < kMaxInputChars) {
            unsigned long cp;
            const std::size_t used = Utf8Next(bytes + len, total - len, &cp);
            if (used == 0)
                break; // cannot happen: the text was checked by the caller
            len += used;
            chars++;
        }
        input_.assign(initialInput, 0, len);
        inputChars_ = chars;
    }

    // The character set of the keyboard input methods (XOpenIM) follows the locale.
    std::setlocale(LC_CTYPE, "");

    if (!display_.Open()) {
        std::fprintf(stderr, "loomora: cannot open the display\n");
        return 1;
    }
    Display *dpy = display_.get();
    XSetErrorHandler(OnXError);
    screen_ = DefaultScreen(dpy);
    white_ = WhitePixel(dpy, screen_);
    black_ = BlackPixel(dpy, screen_);
    bannerBg_ = AllocColour(dpy, screen_, "#FFE9A0", white_);
    visual_ = DefaultVisual(dpy, screen_);
    cmap_ = DefaultColormap(dpy, screen_);

    // Client-side fonts when libXft and fontconfig can be loaded, the core font otherwise.
    fonts_ = xftdl::FontSet::Open(dpy, screen_);
    if (fonts_) {
        ink_ = xftdl::InkColor::Alloc(*fonts_, dpy, visual_, cmap_, "black");
        if (!ink_)
            fonts_.reset();
    }
    if (!fonts_ && !core_.Load(dpy, "fixed")) {
        std::fprintf(stderr, "loomora: the core font \"fixed\" is not available\n");
        return 1;
    }

    int kernelMajor = 0;
    int kernelMinor = 0;
    if (KernelVersion(&kernelMajor, &kernelMinor) &&
        loomora_is_eol(LOOMORA_FAMILY_LINUX, kernelMajor, kernelMinor) == 1)
        eol_ = true;

    window_.Create(dpy, width_, height_, white_,
                   ExposureMask | KeyPressMask | StructureNotifyMask | FocusChangeMask);
    XStoreName(dpy, window_.id(), "Loomora");
    XSizeHints hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.flags = PMinSize;
    hints.min_width = 200;
    hints.min_height = 120;
    XSetWMNormalHints(dpy, window_.id(), &hints);
    static char resName[] = "loomora";
    static char resClass[] = "Loomora";
    XClassHint classHint;
    classHint.res_name = resName;
    classHint.res_class = resClass;
    XSetClassHint(dpy, window_.id(), &classHint);
    Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, window_.id(), &wmDelete, 1);
    gc_.Create(dpy, window_.id());
    if (core_.loaded())
        XSetFont(dpy, gc_.get(), core_.id());
    XSetForeground(dpy, gc_.get(), black_);
    XSetBackground(dpy, gc_.get(), white_);
    if (fonts_) {
        draw_ = xftdl::DrawHandle::Create(*fonts_, dpy, window_.id(), visual_, cmap_);
        if (!draw_) {
            std::fprintf(stderr, "loomora: cannot create the Xft drawing context\n");
            return 1;
        }
    }
    input_method_.Open(dpy, window_.id());
    return 0;
}

int App::Run(const probe::Options &probeOptions) {
    Display *dpy = display_.get();
    const Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    int exitStatus = 0;
    XIC ic = input_method_.ic();

    UpdateGreeting();
    XMapWindow(dpy, window_.id());
    XFlush(dpy);
    if (probeOptions.enabled)
        probe::Arm();

    while (!quit_) {
        while (!quit_ && XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (XFilterEvent(&ev, None))
                continue; // consumed by the input method
            if (ev.type == Expose) {
                if (ev.xexpose.count == 0) {
                    Redraw();
                    XSync(dpy, False);
                    probe::MarkDisplayed(); // only the first call counts
                }
            } else if (ev.type == ConfigureNotify) {
                if (ev.xconfigure.width != width_ || ev.xconfigure.height != height_) {
                    width_ = ev.xconfigure.width;
                    height_ = ev.xconfigure.height;
                    Redraw();
                    XFlush(dpy);
                }
            } else if (ev.type == KeyPress) {
                if (HandleKey(&ev.xkey)) {
                    UpdateGreeting();
                    Redraw();
                    XFlush(dpy);
                }
            } else if (ev.type == FocusIn) {
                if (ic != nullptr)
                    XSetICFocus(ic);
            } else if (ev.type == FocusOut) {
                if (ic != nullptr)
                    XUnsetICFocus(ic);
            } else if (ev.type == MappingNotify) {
                XRefreshKeyboardMapping(&ev.xmapping); // the keys changed their meaning
            } else if (ev.type == ClientMessage) {
                if (static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete)
                    quit_ = true;
            }
        }
        if (quit_)
            break;

        const long left = probe::MsLeft();
        if (left == 0) {
            exitStatus = probe::Emit(input_, greeting_, eol_,
                                     substituted_ ? "glyph-substitution"
                                                  : (missing_ ? "missing-glyph" : ""));
            break;
        }
        const int fd = ConnectionNumber(dpy);
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        timeval wait;
        wait.tv_sec = left / 1000;
        wait.tv_usec = (left % 1000) * 1000;
        const int ready = select(fd + 1, &readable, nullptr, nullptr, left > 0 ? &wait : nullptr);
        if (ready < 0 && errno != EINTR)
            break;
    }
    return exitStatus;
}

int RunProgram(int argc, char **argv) {
    probe::Start();

    if (loomora_abi_version() != LOOMORA_ABI_VERSION) {
        std::fprintf(stderr, "loomora: backend ABI version %u, expected %d\n",
                     static_cast<unsigned>(loomora_abi_version()), LOOMORA_ABI_VERSION);
        return 1;
    }
    const probe::Options probeOptions = probe::ParseArgs(argc, argv);
    if (probeOptions.badUsage || (probeOptions.input != nullptr && !Utf8Valid(probeOptions.input))) {
        std::fprintf(stderr, "usage: %s [--probe [--probe-input <utf8>]]\n", argv[0]);
        return 2;
    }

    // Everything the window owns is released when `app` goes out of scope, in reverse order of
    // creation (input method, Xft objects, GC, core font, window, display).
    App app;
    const int status = app.Init(probeOptions.input != nullptr ? probeOptions.input : "Coucou");
    if (status != 0)
        return status;
    return app.Run(probeOptions);
}

} // namespace

int main(int argc, char **argv) {
    try {
        return RunProgram(argc, argv);
    } catch (const std::exception &e) {
        // Out of memory, in practice: the destructors of the window have run.
        std::fprintf(stderr, "loomora: %s\n", e.what());
        return 1;
    }
}
