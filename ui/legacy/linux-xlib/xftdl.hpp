// Client-side fonts for the Linux Legacy UI: libXft and fontconfig loaded at RUN TIME.
//
// Nothing here is linked: libXft.so.2 and libfontconfig.so.1 are opened with dlopen() when the
// program starts and the dozen functions used are looked up with dlsym(). An old system without
// them (or a build host without their development files: this header declares by itself the few
// types used, their layout being the stable public ABI of Xft 2 and fontconfig 2) still runs: the
// caller sees FontSet::Open() return nullptr and uses the core X11 font path. Names are prefixed
// Dl so that this header never clashes with <X11/Xft/Xft.h> should a file include both.
//
// One FontSet is the fontconfig-sorted list of fonts for "sans-serif": for a code point the first
// font of the list that has it is used (so Latin comes from the best sans font and CJK, Greek,
// Cyrillic, Hebrew, Arabic, symbols, emoji from whatever installed font covers them), each font
// opened on demand. The libraries stay loaded until the process exits (no dlclose): Xlib and Xft
// keep per-display data until then. Everything allocated through them is owned by RAII objects
// (FontSet, InkColor, DrawHandle) that must be destroyed before the Display is closed; none of
// them throws except through std::bad_alloc of the standard containers.
#ifndef LOOMORA_XLIB_XFTDL_HPP
#define LOOMORA_XLIB_XFTDL_HPP

#include <X11/Xlib.h>

#include <memory>
#include <vector>

namespace xftdl {

struct DlFcPattern;
struct DlFcCharSet;
struct DlFcConfig;
struct DlXftDraw;

struct DlFcFontSet { // FcFontSet
    int nfont;
    int sfont;
    DlFcPattern **fonts;
};

struct DlXftFont { // XftFont
    int ascent;
    int descent;
    int height;
    int max_advance_width;
    DlFcCharSet *charset;
    DlFcPattern *pattern;
};

struct DlGlyphInfo { // XGlyphInfo
    unsigned short width;
    unsigned short height;
    short x;
    short y;
    short xOff;
    short yOff;
};

struct DlRenderColor { // XRenderColor
    unsigned short red;
    unsigned short green;
    unsigned short blue;
    unsigned short alpha;
};

struct DlXftColor { // XftColor
    unsigned long pixel;
    DlRenderColor color;
};

// The loaded libraries and the functions used (names as in Xft and fontconfig).
struct XftApi {
    void *xftLib = nullptr;
    void *fcLib = nullptr;
    int (*FcInit)(void) = nullptr;
    DlFcPattern *(*FcNameParse)(const unsigned char *name) = nullptr;
    int (*FcConfigSubstitute)(DlFcConfig *config, DlFcPattern *pattern, int kind) = nullptr;
    DlFcFontSet *(*FcFontSort)(DlFcConfig *config, DlFcPattern *pattern, int trim,
                               DlFcCharSet **csp, int *result) = nullptr;
    DlFcPattern *(*FcFontRenderPrepare)(DlFcConfig *config, DlFcPattern *pattern,
                                        DlFcPattern *font) = nullptr;
    void (*FcPatternDestroy)(DlFcPattern *pattern) = nullptr;
    void (*FcFontSetDestroy)(DlFcFontSet *set) = nullptr;
    int (*FcPatternGetCharSet)(const DlFcPattern *pattern, const char *object, int id,
                               DlFcCharSet **charset) = nullptr;
    int (*FcCharSetHasChar)(const DlFcCharSet *charset, unsigned int ucs4) = nullptr;
    DlXftFont *(*XftFontOpenPattern)(Display *dpy, DlFcPattern *pattern) = nullptr;
    void (*XftFontClose)(Display *dpy, DlXftFont *font) = nullptr;
    void (*XftDefaultSubstitute)(Display *dpy, int screen, DlFcPattern *pattern) = nullptr;
    DlXftDraw *(*XftDrawCreate)(Display *dpy, Drawable drawable, Visual *visual,
                                Colormap cmap) = nullptr;
    void (*XftDrawDestroy)(DlXftDraw *draw) = nullptr;
    int (*XftColorAllocName)(Display *dpy, const Visual *visual, Colormap cmap, const char *name,
                             DlXftColor *result) = nullptr;
    void (*XftColorFree)(Display *dpy, Visual *visual, Colormap cmap,
                         DlXftColor *color) = nullptr;
    void (*XftTextExtents32)(Display *dpy, DlXftFont *font, const unsigned int *text, int len,
                             DlGlyphInfo *extents) = nullptr;
    void (*XftDrawString32)(DlXftDraw *draw, const DlXftColor *color, DlXftFont *font, int x,
                            int y, const unsigned int *text, int len) = nullptr;
};

// The font list of "sans-serif". Owns the loaded API, the fontconfig patterns and every Xft font
// it opened; the Display it was opened on must outlive it.
class FontSet {
  public:
    // Loads the libraries and builds the font list. Returns nullptr (nothing left allocated)
    // when a library or a symbol is missing, no font can be opened or LOOMORA_XLIB_NO_XFT is
    // set in the environment (a switch for tests, "1" or any value).
    static std::unique_ptr<FontSet> Open(Display *dpy, int screen);

    ~FontSet();
    FontSet(const FontSet &) = delete;
    FontSet &operator=(const FontSet &) = delete;

    // The Xft font to draw code point `cp` with, and its index in the list (*index), or nullptr
    // when no installed font has it.
    DlXftFont *Pick(unsigned long cp, int *index);

    DlXftFont *Font(int index) const { return open_[static_cast<std::size_t>(index)].get(); }

    // Advance in pixels of one code point in the font at `index`.
    int Advance(int index, unsigned long cp) const;

    const XftApi &Api() const { return api_; }
    int Ascent() const { return ascent_; }   // largest ascent of the opened fonts
    int Descent() const { return descent_; } // largest descent of the opened fonts
    // Advance and height of a box drawn for a code point no font has.
    int TofuWidth() const { return tofuWidth_; }
    int TofuHeight() const { return tofuHeight_; }

  private:
    FontSet(Display *dpy);

    struct PatternDeleter {
        PatternDeleter() : api(nullptr) {}
        explicit PatternDeleter(const XftApi *a) : api(a) {}
        void operator()(DlFcPattern *p) const;
        const XftApi *api;
    };
    struct SetDeleter {
        SetDeleter() : api(nullptr) {}
        explicit SetDeleter(const XftApi *a) : api(a) {}
        void operator()(DlFcFontSet *s) const;
        const XftApi *api;
    };
    struct FontCloser {
        FontCloser() : api(nullptr), dpy(nullptr) {}
        FontCloser(const XftApi *a, Display *d) : api(a), dpy(d) {}
        void operator()(DlXftFont *f) const;
        const XftApi *api;
        Display *dpy;
    };
    typedef std::unique_ptr<DlXftFont, FontCloser> FontPtr;

    bool Init(int screen);
    bool LoadApi();
    DlXftFont *OpenIndex(int index);

    XftApi api_;
    Display *dpy_;
    std::unique_ptr<DlFcPattern, PatternDeleter> pattern_; // the requested pattern, substituted
    std::unique_ptr<DlFcFontSet, SetDeleter> sorted_;      // fonts in order of preference
    std::vector<FontPtr> open_;           // per sorted font: opened Xft font, null if not (yet)
    std::vector<unsigned char> failed_;   // per sorted font: 1 when opening it failed
    int ascent_ = 0;
    int descent_ = 0;
    int tofuWidth_ = 0;
    int tofuHeight_ = 0;
};

// An Xft colour allocated by name, freed with the object. The FontSet must outlive it.
class InkColor {
  public:
    // Returns nullptr when the colour cannot be allocated.
    static std::unique_ptr<InkColor> Alloc(const FontSet &fonts, Display *dpy, Visual *visual,
                                           Colormap cmap, const char *name);
    ~InkColor();
    InkColor(const InkColor &) = delete;
    InkColor &operator=(const InkColor &) = delete;

    const DlXftColor &Color() const { return color_; }

  private:
    InkColor(const FontSet &fonts, Display *dpy, Visual *visual, Colormap cmap)
        : api_(&fonts.Api()), dpy_(dpy), visual_(visual), cmap_(cmap), color_() {}

    const XftApi *api_;
    Display *dpy_;
    Visual *visual_;
    Colormap cmap_;
    DlXftColor color_;
};

// An Xft drawing context on a window, destroyed with the object. The FontSet must outlive it.
class DrawHandle {
  public:
    // Returns nullptr when the context cannot be created.
    static std::unique_ptr<DrawHandle> Create(const FontSet &fonts, Display *dpy,
                                              Drawable drawable, Visual *visual, Colormap cmap);
    ~DrawHandle();
    DrawHandle(const DrawHandle &) = delete;
    DrawHandle &operator=(const DrawHandle &) = delete;

    // Draws `count` code points of one font with the pen at x and the baseline at y.
    void DrawString(const InkColor &ink, DlXftFont *font, int x, int y, const unsigned int *text,
                    int count) const;

  private:
    DrawHandle(const XftApi *api, DlXftDraw *draw) : api_(api), draw_(draw) {}

    const XftApi *api_;
    DlXftDraw *draw_;
};

} // namespace xftdl

#endif
