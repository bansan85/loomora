// Client-side fonts for the Linux Legacy UI: libXft and fontconfig, linked normally. Built only
// when CMake finds them (LOOMORA_XLIB_HAVE_XFT, see CMakeLists.txt); without them the UI draws
// with the core X11 font.
//
// One FontSet is the fontconfig-sorted list of fonts for "sans-serif": for a code point the first
// font of the list that has it is used (so Latin comes from the best sans font and CJK, Greek,
// Cyrillic, Hebrew, Arabic, symbols, emoji from whatever installed font covers them), each font
// opened on demand. Everything allocated through Xft and fontconfig is owned by RAII objects
// (FontSet, InkColor, DrawHandle) that must be destroyed before the Display is closed; none of
// them throws except through std::bad_alloc of the standard containers.
#ifndef LOOMORA_XLIB_XFTFONTS_HPP
#define LOOMORA_XLIB_XFTFONTS_HPP

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <fontconfig/fontconfig.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace xftfonts {

// The font list of "sans-serif". Owns the fontconfig patterns and every Xft font it opened; the
// Display it was opened on must outlive it.
class FontSet {
  public:
    // Builds the font list. Returns nullptr (nothing left allocated) when fontconfig cannot be
    // initialised, no font can be opened or LOOMORA_XLIB_NO_XFT is set in the environment (a
    // switch for tests: it forces the core-font path on a machine that has Xft).
    static std::unique_ptr<FontSet> Open(Display *dpy, int screen);

    ~FontSet();
    FontSet(const FontSet &) = delete;
    FontSet &operator=(const FontSet &) = delete;

    // The Xft font to draw code point `cp` with, and its index in the list (*index), or nullptr
    // when no installed font has it.
    XftFont *Pick(unsigned long cp, int *index);

    XftFont *Font(int index) const { return open_[static_cast<std::size_t>(index)].get(); }

    // Advance in pixels of one code point in the font at `index`.
    int Advance(int index, unsigned long cp) const;

    int Ascent() const { return ascent_; }   // largest ascent of the opened fonts
    int Descent() const { return descent_; } // largest descent of the opened fonts
    // Advance and height of a box drawn for a code point no font has.
    int TofuWidth() const { return tofuWidth_; }
    int TofuHeight() const { return tofuHeight_; }

  private:
    explicit FontSet(Display *dpy);

    struct PatternDeleter {
        void operator()(FcPattern *p) const { FcPatternDestroy(p); }
    };
    struct SetDeleter {
        void operator()(FcFontSet *s) const { FcFontSetDestroy(s); }
    };
    struct FontCloser {
        FontCloser() : dpy(nullptr) {}
        explicit FontCloser(Display *d) : dpy(d) {}
        void operator()(XftFont *f) const { XftFontClose(dpy, f); }
        Display *dpy;
    };
    typedef std::unique_ptr<XftFont, FontCloser> FontPtr;

    bool Init(int screen);
    XftFont *OpenIndex(int index);

    Display *dpy_;
    std::unique_ptr<FcPattern, PatternDeleter> pattern_; // the requested pattern, substituted
    std::unique_ptr<FcFontSet, SetDeleter> sorted_;      // fonts in order of preference
    std::vector<FontPtr> open_;         // per sorted font: opened Xft font, null if not (yet)
    std::vector<unsigned char> failed_; // per sorted font: 1 when opening it failed
    int ascent_ = 0;
    int descent_ = 0;
    int tofuWidth_ = 0;
    int tofuHeight_ = 0;
};

// An Xft colour allocated by name, freed with the object.
class InkColor {
  public:
    // Returns nullptr when the colour cannot be allocated.
    static std::unique_ptr<InkColor> Alloc(Display *dpy, Visual *visual, Colormap cmap,
                                           const char *name);
    ~InkColor();
    InkColor(const InkColor &) = delete;
    InkColor &operator=(const InkColor &) = delete;

    const XftColor &Color() const { return color_; }

  private:
    InkColor(Display *dpy, Visual *visual, Colormap cmap)
        : dpy_(dpy), visual_(visual), cmap_(cmap), color_() {}

    Display *dpy_;
    Visual *visual_;
    Colormap cmap_;
    XftColor color_;
};

// An Xft drawing context on a window, destroyed with the object.
class DrawHandle {
  public:
    // Returns nullptr when the context cannot be created.
    static std::unique_ptr<DrawHandle> Create(Display *dpy, Drawable drawable, Visual *visual,
                                              Colormap cmap);
    ~DrawHandle();
    DrawHandle(const DrawHandle &) = delete;
    DrawHandle &operator=(const DrawHandle &) = delete;

    // Draws `count` code points of one font with the pen at x and the baseline at y.
    void DrawString(const InkColor &ink, XftFont *font, int x, int y, const unsigned int *text,
                    int count) const;

  private:
    explicit DrawHandle(XftDraw *draw) : draw_(draw) {}

    XftDraw *draw_;
};

} // namespace xftfonts

#endif
