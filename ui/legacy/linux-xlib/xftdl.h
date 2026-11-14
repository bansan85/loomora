/* Client-side fonts for the Linux Legacy UI: libXft and fontconfig loaded at RUN TIME.

   Nothing here is linked: libXft.so.2 and libfontconfig.so.1 are opened with dlopen() when the
   program starts and the dozen functions used are looked up with dlsym(). An old system without
   them (or a build host without their development files: this header declares by itself the few
   types used, their layout being the stable public ABI of Xft 2 and fontconfig 2) still runs: the
   caller sees xftdl_open() fail and uses the core X11 font path. Names are prefixed Dl so that this
   header never clashes with <X11/Xft/Xft.h> should a file include both.

   One FontSet is the fontconfig-sorted list of fonts for "sans-serif": for a code point the first
   font of the list that has it is used (so Latin comes from the best sans font and CJK, Greek,
   Cyrillic, Hebrew, Arabic, symbols, emoji from whatever installed font covers them), each font
   opened on demand. Plain C89. */
#ifndef LOOMORA_XLIB_XFTDL_H
#define LOOMORA_XLIB_XFTDL_H

#include <X11/Xlib.h>

typedef struct DlFcPatternRec DlFcPattern;
typedef struct DlFcCharSetRec DlFcCharSet;
typedef struct DlFcConfigRec DlFcConfig;
typedef struct DlXftDrawRec DlXftDraw;

typedef struct {
    int nfont;
    int sfont;
    DlFcPattern **fonts;
} DlFcFontSet; /* FcFontSet */

typedef struct {
    int ascent;
    int descent;
    int height;
    int max_advance_width;
    DlFcCharSet *charset;
    DlFcPattern *pattern;
} DlXftFont; /* XftFont */

typedef struct {
    unsigned short width;
    unsigned short height;
    short x;
    short y;
    short xOff;
    short yOff;
} DlGlyphInfo; /* XGlyphInfo */

typedef struct {
    unsigned short red;
    unsigned short green;
    unsigned short blue;
    unsigned short alpha;
} DlRenderColor; /* XRenderColor */

typedef struct {
    unsigned long pixel;
    DlRenderColor color;
} DlXftColor; /* XftColor */

/* The loaded libraries and the functions used (names as in Xft and fontconfig). */
typedef struct {
    void *xft_lib;
    void *fc_lib;
    int (*FcInit)(void);
    DlFcPattern *(*FcNameParse)(const unsigned char *name);
    int (*FcConfigSubstitute)(DlFcConfig *config, DlFcPattern *pattern, int kind);
    DlFcFontSet *(*FcFontSort)(DlFcConfig *config, DlFcPattern *pattern, int trim,
                               DlFcCharSet **csp, int *result);
    DlFcPattern *(*FcFontRenderPrepare)(DlFcConfig *config, DlFcPattern *pattern,
                                        DlFcPattern *font);
    void (*FcPatternDestroy)(DlFcPattern *pattern);
    void (*FcFontSetDestroy)(DlFcFontSet *set);
    int (*FcPatternGetCharSet)(const DlFcPattern *pattern, const char *object, int id,
                               DlFcCharSet **charset);
    int (*FcCharSetHasChar)(const DlFcCharSet *charset, unsigned int ucs4);
    DlXftFont *(*XftFontOpenPattern)(Display *dpy, DlFcPattern *pattern);
    void (*XftFontClose)(Display *dpy, DlXftFont *font);
    void (*XftDefaultSubstitute)(Display *dpy, int screen, DlFcPattern *pattern);
    DlXftDraw *(*XftDrawCreate)(Display *dpy, Drawable drawable, Visual *visual, Colormap cmap);
    void (*XftDrawDestroy)(DlXftDraw *draw);
    int (*XftColorAllocName)(Display *dpy, const Visual *visual, Colormap cmap, const char *name,
                             DlXftColor *result);
    void (*XftColorFree)(Display *dpy, Visual *visual, Colormap cmap, DlXftColor *color);
    void (*XftTextExtents32)(Display *dpy, DlXftFont *font, const unsigned int *text, int len,
                             DlGlyphInfo *extents);
    void (*XftDrawString32)(DlXftDraw *draw, const DlXftColor *color, DlXftFont *font, int x, int y,
                            const unsigned int *text, int len);
} XftApi;

typedef struct {
    XftApi api;
    Display *dpy;
    DlFcPattern *pattern;  /* the requested pattern, after substitution */
    DlFcFontSet *sorted;   /* fonts in order of preference */
    DlXftFont **open;      /* per sorted font: opened Xft font, NULL if not opened (yet) */
    unsigned char *failed; /* per sorted font: 1 when opening it failed */
    int ascent;            /* largest ascent of the opened fonts */
    int descent;           /* largest descent of the opened fonts */
    int tofu_width;        /* advance of a box drawn for a code point no font has */
    int tofu_height;
} FontSet;

/* Loads the libraries and builds the font list. Returns 1 when text can be drawn with Xft; 0
   (nothing left allocated) when a library or a symbol is missing, no font can be opened or
   LOOMORA_XLIB_NO_XFT is set in the environment (a switch for tests, "1" or any value). */
int fontset_open(FontSet *fs, Display *dpy, int screen);

void fontset_close(FontSet *fs);

/* The Xft font to draw code point `cp` with, and its index in the list (*index), or NULL when no
   installed font has it. */
DlXftFont *fontset_pick(FontSet *fs, unsigned long cp, int *index);

DlXftFont *fontset_font(const FontSet *fs, int index);

/* Advance in pixels of one code point in the font at `index`. */
int fontset_advance(const FontSet *fs, int index, unsigned long cp);

#endif
