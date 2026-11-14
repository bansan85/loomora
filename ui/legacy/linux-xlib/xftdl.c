/* Run-time loading of libXft and fontconfig, and the font list: see xftdl.h. */
#include "xftdl.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#define FC_MATCH_PATTERN 0
#define FC_RESULT_MATCH 0
#define FONT_REQUEST "sans-serif:size=11"

static void *load_library(const char *const *names) {
    void *h = NULL;
    for (; *names != NULL && h == NULL; names++)
        h = dlopen(*names, RTLD_NOW | RTLD_LOCAL);
    return h;
}

/* dlsym for a function pointer: the cast through void ** is the POSIX way to store it. */
#define LOAD(lib, field)                                                                           \
    do {                                                                                           \
        *(void **)(&api->field) = dlsym((lib), #field);                                            \
        if (api->field == NULL)                                                                    \
            return 0;                                                                              \
    } while (0)

static int load_api(XftApi *api) {
    static const char *const xft_names[] = {"libXft.so.2", "libXft.so", NULL};
    static const char *const fc_names[] = {"libfontconfig.so.1", "libfontconfig.so", NULL};
    memset(api, 0, sizeof(*api));
    /* libXft needs fontconfig and freetype itself: the dynamic loader finds them. */
    api->fc_lib = load_library(fc_names);
    api->xft_lib = load_library(xft_names);
    if (api->fc_lib == NULL || api->xft_lib == NULL)
        return 0;
    LOAD(api->fc_lib, FcInit);
    LOAD(api->fc_lib, FcNameParse);
    LOAD(api->fc_lib, FcConfigSubstitute);
    LOAD(api->fc_lib, FcFontSort);
    LOAD(api->fc_lib, FcFontRenderPrepare);
    LOAD(api->fc_lib, FcPatternDestroy);
    LOAD(api->fc_lib, FcFontSetDestroy);
    LOAD(api->fc_lib, FcPatternGetCharSet);
    LOAD(api->fc_lib, FcCharSetHasChar);
    LOAD(api->xft_lib, XftFontOpenPattern);
    LOAD(api->xft_lib, XftFontClose);
    LOAD(api->xft_lib, XftDefaultSubstitute);
    LOAD(api->xft_lib, XftDrawCreate);
    LOAD(api->xft_lib, XftDrawDestroy);
    LOAD(api->xft_lib, XftColorAllocName);
    LOAD(api->xft_lib, XftColorFree);
    LOAD(api->xft_lib, XftTextExtents32);
    LOAD(api->xft_lib, XftDrawString32);
    return 1;
}

static void note_metrics(FontSet *fs, const DlXftFont *f) {
    if (f->ascent > fs->ascent)
        fs->ascent = f->ascent;
    if (f->descent > fs->descent)
        fs->descent = f->descent;
}

/* Opens (once) the font at `index` of the sorted list, sized by the requested pattern. */
static DlXftFont *open_index(FontSet *fs, int index) {
    DlFcPattern *prepared;
    DlXftFont *font;
    if (fs->open[index] != NULL)
        return fs->open[index];
    if (fs->failed[index])
        return NULL;
    prepared = fs->api.FcFontRenderPrepare(NULL, fs->pattern, fs->sorted->fonts[index]);
    font = prepared != NULL ? fs->api.XftFontOpenPattern(fs->dpy, prepared) : NULL;
    if (font == NULL) {
        if (prepared != NULL)
            fs->api.FcPatternDestroy(prepared); /* Xft owns it only when the open worked */
        fs->failed[index] = 1;
        return NULL;
    }
    fs->open[index] = font;
    note_metrics(fs, font);
    return font;
}

int fontset_open(FontSet *fs, Display *dpy, int screen) {
    int result = FC_RESULT_MATCH;
    int i;
    int first = -1;
    DlGlyphInfo gi;
    unsigned int zero = '0';
    memset(fs, 0, sizeof(*fs));
    if (getenv("LOOMORA_XLIB_NO_XFT") != NULL)
        return 0;
    fs->dpy = dpy;
    if (!load_api(&fs->api)) {
        fontset_close(fs);
        return 0;
    }
    if (!fs->api.FcInit()) {
        fontset_close(fs);
        return 0;
    }
    fs->pattern = fs->api.FcNameParse((const unsigned char *)FONT_REQUEST);
    if (fs->pattern == NULL) {
        fontset_close(fs);
        return 0;
    }
    fs->api.FcConfigSubstitute(NULL, fs->pattern, FC_MATCH_PATTERN);
    fs->api.XftDefaultSubstitute(dpy, screen, fs->pattern);
    fs->sorted = fs->api.FcFontSort(NULL, fs->pattern, 1, NULL, &result);
    if (fs->sorted == NULL || fs->sorted->nfont <= 0) {
        fontset_close(fs);
        return 0;
    }
    fs->open = (DlXftFont **)calloc((size_t)fs->sorted->nfont, sizeof(DlXftFont *));
    fs->failed = (unsigned char *)calloc((size_t)fs->sorted->nfont, 1);
    if (fs->open == NULL || fs->failed == NULL) {
        fontset_close(fs);
        return 0;
    }
    /* The font that gives Latin text must open, otherwise there is nothing to draw with. */
    for (i = 0; i < fs->sorted->nfont && first < 0; i++) {
        DlFcCharSet *cs = NULL;
        if (fs->api.FcPatternGetCharSet(fs->sorted->fonts[i], "charset", 0, &cs) ==
                FC_RESULT_MATCH &&
            cs != NULL && fs->api.FcCharSetHasChar(cs, 'M') && open_index(fs, i) != NULL)
            first = i;
    }
    if (first < 0) {
        fontset_close(fs);
        return 0;
    }
    fs->api.XftTextExtents32(dpy, fs->open[first], &zero, 1, &gi);
    fs->tofu_width = gi.xOff > 4 ? gi.xOff : fs->open[first]->max_advance_width / 2 + 2;
    fs->tofu_height = fs->open[first]->ascent > 3 ? fs->open[first]->ascent - 1 : 3;
    return 1;
}

void fontset_close(FontSet *fs) {
    int i;
    if (fs->sorted != NULL && fs->open != NULL)
        for (i = 0; i < fs->sorted->nfont; i++)
            if (fs->open[i] != NULL)
                fs->api.XftFontClose(fs->dpy, fs->open[i]);
    if (fs->sorted != NULL)
        fs->api.FcFontSetDestroy(fs->sorted);
    if (fs->pattern != NULL)
        fs->api.FcPatternDestroy(fs->pattern);
    free(fs->open);
    free(fs->failed);
    /* The libraries stay loaded: Xlib and Xft keep per-display data until the process exits. */
    memset(fs, 0, sizeof(*fs));
}

DlXftFont *fontset_pick(FontSet *fs, unsigned long cp, int *index) {
    int i;
    for (i = 0; i < fs->sorted->nfont; i++) {
        DlFcCharSet *cs = NULL;
        if (fs->failed[i])
            continue;
        if (fs->api.FcPatternGetCharSet(fs->sorted->fonts[i], "charset", 0, &cs) !=
                FC_RESULT_MATCH ||
            cs == NULL || !fs->api.FcCharSetHasChar(cs, (unsigned int)cp))
            continue;
        if (open_index(fs, i) != NULL) {
            *index = i;
            return fs->open[i];
        }
    }
    return NULL;
}

DlXftFont *fontset_font(const FontSet *fs, int index) { return fs->open[index]; }

int fontset_advance(const FontSet *fs, int index, unsigned long cp) {
    DlGlyphInfo gi;
    unsigned int ch = (unsigned int)cp;
    memset(&gi, 0, sizeof(gi));
    fs->api.XftTextExtents32(fs->dpy, fs->open[index], &ch, 1, &gi);
    return gi.xOff;
}
