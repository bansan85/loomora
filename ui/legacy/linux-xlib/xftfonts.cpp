// The Xft font list: see xftfonts.hpp.
#include "xftfonts.hpp"

#include <cstdlib>
#include <cstring>

namespace xftfonts {

namespace {

const char kFontRequest[] = "sans-serif:size=11";

} // namespace

FontSet::FontSet(Display *dpy) : dpy_(dpy) {}

// Members are released in reverse order of declaration: the Xft fonts first, then the sorted list
// and the pattern.
FontSet::~FontSet() = default;

// Opens (once) the font at `index` of the sorted list, sized by the requested pattern.
XftFont *FontSet::OpenIndex(int index) {
    const std::size_t i = static_cast<std::size_t>(index);
    if (open_[i])
        return open_[i].get();
    if (failed_[i])
        return nullptr;
    FcPattern *prepared = FcFontRenderPrepare(nullptr, pattern_.get(), sorted_->fonts[i]);
    XftFont *font = prepared != nullptr ? XftFontOpenPattern(dpy_, prepared) : nullptr;
    if (font == nullptr) {
        if (prepared != nullptr)
            FcPatternDestroy(prepared); // Xft owns it only when the open worked
        failed_[i] = 1;
        return nullptr;
    }
    open_[i] = FontPtr(font, FontCloser(dpy_));
    if (font->ascent > ascent_)
        ascent_ = font->ascent;
    if (font->descent > descent_)
        descent_ = font->descent;
    return font;
}

bool FontSet::Init(int screen) {
    if (!FcInit())
        return false;
    pattern_.reset(FcNameParse(reinterpret_cast<const FcChar8 *>(kFontRequest)));
    if (!pattern_)
        return false;
    FcConfigSubstitute(nullptr, pattern_.get(), FcMatchPattern);
    XftDefaultSubstitute(dpy_, screen, pattern_.get());
    FcResult result = FcResultMatch;
    sorted_.reset(FcFontSort(nullptr, pattern_.get(), FcTrue, nullptr, &result));
    if (!sorted_ || sorted_->nfont <= 0)
        return false;
    const std::size_t count = static_cast<std::size_t>(sorted_->nfont);
    open_.resize(count);
    failed_.assign(count, 0);
    // The font that gives Latin text must open, otherwise there is nothing to draw with.
    int first = -1;
    for (int i = 0; i < sorted_->nfont && first < 0; i++) {
        FcCharSet *cs = nullptr;
        if (FcPatternGetCharSet(sorted_->fonts[i], FC_CHARSET, 0, &cs) == FcResultMatch &&
            cs != nullptr && FcCharSetHasChar(cs, 'M') && OpenIndex(i) != nullptr)
            first = i;
    }
    if (first < 0)
        return false;
    XGlyphInfo gi;
    std::memset(&gi, 0, sizeof(gi));
    const FcChar32 zero = '0';
    XftTextExtents32(dpy_, Font(first), &zero, 1, &gi);
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

XftFont *FontSet::Pick(unsigned long cp, int *index) {
    for (int i = 0; i < sorted_->nfont; i++) {
        FcCharSet *cs = nullptr;
        if (failed_[static_cast<std::size_t>(i)])
            continue;
        if (FcPatternGetCharSet(sorted_->fonts[i], FC_CHARSET, 0, &cs) != FcResultMatch ||
            cs == nullptr || !FcCharSetHasChar(cs, static_cast<FcChar32>(cp)))
            continue;
        XftFont *font = OpenIndex(i);
        if (font != nullptr) {
            *index = i;
            return font;
        }
    }
    return nullptr;
}

int FontSet::Advance(int index, unsigned long cp) const {
    XGlyphInfo gi;
    std::memset(&gi, 0, sizeof(gi));
    const FcChar32 ch = static_cast<FcChar32>(cp);
    XftTextExtents32(dpy_, Font(index), &ch, 1, &gi);
    return gi.xOff;
}

std::unique_ptr<InkColor> InkColor::Alloc(Display *dpy, Visual *visual, Colormap cmap,
                                          const char *name) {
    std::unique_ptr<InkColor> ink(new InkColor(dpy, visual, cmap));
    if (!XftColorAllocName(dpy, visual, cmap, name, &ink->color_)) {
        // Nothing was allocated: the destructor must not call XftColorFree.
        ink->dpy_ = nullptr;
        return std::unique_ptr<InkColor>();
    }
    return ink;
}

InkColor::~InkColor() {
    if (dpy_ != nullptr)
        XftColorFree(dpy_, visual_, cmap_, &color_);
}

std::unique_ptr<DrawHandle> DrawHandle::Create(Display *dpy, Drawable drawable, Visual *visual,
                                               Colormap cmap) {
    XftDraw *draw = XftDrawCreate(dpy, drawable, visual, cmap);
    if (draw == nullptr)
        return std::unique_ptr<DrawHandle>();
    return std::unique_ptr<DrawHandle>(new DrawHandle(draw));
}

DrawHandle::~DrawHandle() { XftDrawDestroy(draw_); }

void DrawHandle::DrawString(const InkColor &ink, XftFont *font, int x, int y,
                            const unsigned int *text, int count) const {
    XftDrawString32(draw_, &ink.Color(), font, x, y, text, count);
}

} // namespace xftfonts
