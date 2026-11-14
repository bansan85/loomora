// Run-time loading of libXft and fontconfig, and the font list: see xftdl.hpp.
#include "xftdl.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>

namespace xftdl {

namespace {

const int kFcMatchPattern = 0;
const int kFcResultMatch = 0;
const char kFontRequest[] = "sans-serif:size=11";

void *LoadLibrary(const char *const *names) {
    void *h = nullptr;
    for (; *names != nullptr && h == nullptr; names++)
        h = dlopen(*names, RTLD_NOW | RTLD_LOCAL);
    return h;
}

// dlsym for a function pointer: POSIX requires the conversion from void * to work.
template <class Fn> bool Resolve(void *lib, const char *name, Fn &out) {
    out = reinterpret_cast<Fn>(dlsym(lib, name));
    return out != nullptr;
}

#define LOAD(lib, field)                                                                           \
    do {                                                                                           \
        if (!Resolve((lib), #field, api_.field))                                                   \
            return false;                                                                          \
    } while (0)

} // namespace

void FontSet::PatternDeleter::operator()(DlFcPattern *p) const {
    if (api != nullptr && p != nullptr)
        api->FcPatternDestroy(p);
}

void FontSet::SetDeleter::operator()(DlFcFontSet *s) const {
    if (api != nullptr && s != nullptr)
        api->FcFontSetDestroy(s);
}

void FontSet::FontCloser::operator()(DlXftFont *f) const {
    if (api != nullptr && f != nullptr)
        api->XftFontClose(dpy, f);
}

FontSet::FontSet(Display *dpy) : dpy_(dpy) {}

// Members are released in reverse order of declaration: the Xft fonts first, then the sorted list
// and the pattern, and the function table (api_) last, which the deleters use. The libraries
// stay loaded.
FontSet::~FontSet() = default;

bool FontSet::LoadApi() {
    static const char *const xftNames[] = {"libXft.so.2", "libXft.so", nullptr};
    static const char *const fcNames[] = {"libfontconfig.so.1", "libfontconfig.so", nullptr};
    // libXft needs fontconfig and freetype itself: the dynamic loader finds them.
    api_.fcLib = LoadLibrary(fcNames);
    api_.xftLib = LoadLibrary(xftNames);
    if (api_.fcLib == nullptr || api_.xftLib == nullptr)
        return false;
    LOAD(api_.fcLib, FcInit);
    LOAD(api_.fcLib, FcNameParse);
    LOAD(api_.fcLib, FcConfigSubstitute);
    LOAD(api_.fcLib, FcFontSort);
    LOAD(api_.fcLib, FcFontRenderPrepare);
    LOAD(api_.fcLib, FcPatternDestroy);
    LOAD(api_.fcLib, FcFontSetDestroy);
    LOAD(api_.fcLib, FcPatternGetCharSet);
    LOAD(api_.fcLib, FcCharSetHasChar);
    LOAD(api_.xftLib, XftFontOpenPattern);
    LOAD(api_.xftLib, XftFontClose);
    LOAD(api_.xftLib, XftDefaultSubstitute);
    LOAD(api_.xftLib, XftDrawCreate);
    LOAD(api_.xftLib, XftDrawDestroy);
    LOAD(api_.xftLib, XftColorAllocName);
    LOAD(api_.xftLib, XftColorFree);
    LOAD(api_.xftLib, XftTextExtents32);
    LOAD(api_.xftLib, XftDrawString32);
    return true;
}

// Opens (once) the font at `index` of the sorted list, sized by the requested pattern.
DlXftFont *FontSet::OpenIndex(int index) {
    const std::size_t i = static_cast<std::size_t>(index);
    if (open_[i])
        return open_[i].get();
    if (failed_[i])
        return nullptr;
    DlFcPattern *prepared = api_.FcFontRenderPrepare(nullptr, pattern_.get(), sorted_->fonts[i]);
    DlXftFont *font = prepared != nullptr ? api_.XftFontOpenPattern(dpy_, prepared) : nullptr;
    if (font == nullptr) {
        if (prepared != nullptr)
            api_.FcPatternDestroy(prepared); // Xft owns it only when the open worked
        failed_[i] = 1;
        return nullptr;
    }
    open_[i] = FontPtr(font, FontCloser(&api_, dpy_));
    if (font->ascent > ascent_)
        ascent_ = font->ascent;
    if (font->descent > descent_)
        descent_ = font->descent;
    return font;
}

bool FontSet::Init(int screen) {
    if (!LoadApi())
        return false;
    if (!api_.FcInit())
        return false;
    pattern_ = std::unique_ptr<DlFcPattern, PatternDeleter>(
        api_.FcNameParse(reinterpret_cast<const unsigned char *>(kFontRequest)),
        PatternDeleter(&api_));
    if (!pattern_)
        return false;
    api_.FcConfigSubstitute(nullptr, pattern_.get(), kFcMatchPattern);
    api_.XftDefaultSubstitute(dpy_, screen, pattern_.get());
    int result = kFcResultMatch;
    sorted_ = std::unique_ptr<DlFcFontSet, SetDeleter>(
        api_.FcFontSort(nullptr, pattern_.get(), 1, nullptr, &result), SetDeleter(&api_));
    if (!sorted_ || sorted_->nfont <= 0)
        return false;
    const std::size_t count = static_cast<std::size_t>(sorted_->nfont);
    open_.resize(count);
    failed_.assign(count, 0);
    // The font that gives Latin text must open, otherwise there is nothing to draw with.
    int first = -1;
    for (int i = 0; i < sorted_->nfont && first < 0; i++) {
        DlFcCharSet *cs = nullptr;
        if (api_.FcPatternGetCharSet(sorted_->fonts[i], "charset", 0, &cs) == kFcResultMatch &&
            cs != nullptr && api_.FcCharSetHasChar(cs, 'M') && OpenIndex(i) != nullptr)
            first = i;
    }
    if (first < 0)
        return false;
    DlGlyphInfo gi;
    std::memset(&gi, 0, sizeof(gi));
    unsigned int zero = '0';
    api_.XftTextExtents32(dpy_, Font(first), &zero, 1, &gi);
    tofuWidth_ = gi.xOff > 4 ? gi.xOff : Font(first)->max_advance_width / 2 + 2;
    tofuHeight_ = Font(first)->ascent > 3 ? Font(first)->ascent - 1 : 3;
    return true;
}

std::unique_ptr<FontSet> FontSet::Open(Display *dpy, int screen) {
    if (std::getenv("LOOMORA_XLIB_NO_XFT") != nullptr)
        return std::unique_ptr<FontSet>();
    std::unique_ptr<FontSet> fonts(new FontSet(dpy));
    if (!fonts->Init(screen))
        return std::unique_ptr<FontSet>(); // the destructor frees what was allocated
    return fonts;
}

DlXftFont *FontSet::Pick(unsigned long cp, int *index) {
    for (int i = 0; i < sorted_->nfont; i++) {
        DlFcCharSet *cs = nullptr;
        if (failed_[static_cast<std::size_t>(i)])
            continue;
        if (api_.FcPatternGetCharSet(sorted_->fonts[i], "charset", 0, &cs) != kFcResultMatch ||
            cs == nullptr || !api_.FcCharSetHasChar(cs, static_cast<unsigned int>(cp)))
            continue;
        DlXftFont *font = OpenIndex(i);
        if (font != nullptr) {
            *index = i;
            return font;
        }
    }
    return nullptr;
}

int FontSet::Advance(int index, unsigned long cp) const {
    DlGlyphInfo gi;
    std::memset(&gi, 0, sizeof(gi));
    unsigned int ch = static_cast<unsigned int>(cp);
    api_.XftTextExtents32(dpy_, Font(index), &ch, 1, &gi);
    return gi.xOff;
}

std::unique_ptr<InkColor> InkColor::Alloc(const FontSet &fonts, Display *dpy, Visual *visual,
                                          Colormap cmap, const char *name) {
    std::unique_ptr<InkColor> ink(new InkColor(fonts, dpy, visual, cmap));
    if (!ink->api_->XftColorAllocName(dpy, visual, cmap, name, &ink->color_))
        return std::unique_ptr<InkColor>(); // nothing was allocated: no XftColorFree
    return ink;
}

InkColor::~InkColor() { api_->XftColorFree(dpy_, visual_, cmap_, &color_); }

std::unique_ptr<DrawHandle> DrawHandle::Create(const FontSet &fonts, Display *dpy,
                                               Drawable drawable, Visual *visual,
                                               Colormap cmap) {
    DlXftDraw *draw = fonts.Api().XftDrawCreate(dpy, drawable, visual, cmap);
    if (draw == nullptr)
        return std::unique_ptr<DrawHandle>();
    return std::unique_ptr<DrawHandle>(new DrawHandle(&fonts.Api(), draw));
}

DrawHandle::~DrawHandle() { api_->XftDrawDestroy(draw_); }

void DrawHandle::DrawString(const InkColor &ink, DlXftFont *font, int x, int y,
                            const unsigned int *text, int count) const {
    api_->XftDrawString32(draw_, &ink.Color(), font, x, y, text, count);
}

} // namespace xftdl
