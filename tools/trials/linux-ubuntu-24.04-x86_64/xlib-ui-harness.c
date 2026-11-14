/* Harness for the Linux Legacy (Xlib) UI: includes main.c with its main() renamed, sets up the
   window and the text engine the way main() does, then feeds hard texts straight to the drawing
   code (redraw: block_make, wrap, draw), including invalid UTF-8 that the greeting backend and
   --probe-input would refuse, so that a sanitizer build can check the memory behaviour.

   Build (from ui/legacy/linux-xlib):
     gcc -g -fsanitize=address,undefined -fno-omit-frame-pointer -std=gnu89 -Dmain=loomora_ui_main
         -I../../../core/include -I. -x c
   ../../../tools/trials/linux-ubuntu-24.04-x86_64/xlib-ui-harness.c probe.c xftdl.c keys.c -x none
   <libloomora.a> -lX11 -ldl -lstdc++ Run with $DISPLAY set; LOOMORA_XLIB_NO_XFT=1 exercises the
   core font path, LOOMORA_XLIB_NO_XIM=1 the key path without an input method. Prints the engine in
   use and one line per text. */
#include "../../../ui/legacy/linux-xlib/main.c"

#undef main

static void run_text(Ui *ui, const char *name, const unsigned char *bytes, size_t len, int how) {
    size_t i;
    struct timeval t0;
    struct timeval t1;
    gettimeofday(&t0, NULL);
    if (len > MAX_INPUT_BYTES)
        len = MAX_INPUT_BYTES;
    memcpy(ui->input, bytes, len);
    ui->input[len] = '\0';
    ui->input_len = len;
    ui->input_chars = 0;
    for (i = 0; i < len; i++)
        if (((unsigned char)ui->input[i] & 0xC0) != 0x80)
            ui->input_chars++;
    if (how == 0) {
        update_greeting(ui); /* the backend refuses invalid UTF-8: an error message is shown */
    } else {
        /* straight to the drawing code: the greeting holds the same raw bytes */
        memcpy(ui->greeting, bytes, len);
        ui->greeting_len = len;
        ui->greeting[len] = '\0';
        {
            Block b;
            if (block_make(ui, ui->greeting, ui->greeting_len, 1 << 20, 1, &b)) {
                ui->substituted = b.substituted > 0;
                ui->missing = b.missing > 0;
                block_free(&b);
            }
            if (block_make(ui, ui->input, ui->input_len, 1 << 20, 0, &b))
                block_free(&b);
        }
    }
    redraw(ui);
    XSync(ui->dpy, False);
    gettimeofday(&t1, NULL);
    printf("%-44s bytes %4lu chars %4lu substituted %d missing %d  %ld ms\n", name,
           (unsigned long)len, (unsigned long)ui->input_chars, ui->substituted, ui->missing,
           (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000));
}

int main(void) {
    Ui ui;
    XSetWindowAttributes attrs;
    unsigned char big[MAX_INPUT_BYTES + 8];
    unsigned char rnd[MAX_INPUT_BYTES + 8];
    size_t i;
    unsigned int seed = 12345;

    memset(&ui, 0, sizeof(ui));
    setlocale(LC_CTYPE, "");
    ui.dpy = XOpenDisplay(NULL);
    if (ui.dpy == NULL) {
        fprintf(stderr, "no display\n");
        return 1;
    }
    ui.screen = DefaultScreen(ui.dpy);
    ui.white = WhitePixel(ui.dpy, ui.screen);
    ui.black = BlackPixel(ui.dpy, ui.screen);
    ui.banner_bg = alloc_colour(ui.dpy, ui.screen, "#FFE9A0", ui.white);
    ui.visual = DefaultVisual(ui.dpy, ui.screen);
    ui.cmap = DefaultColormap(ui.dpy, ui.screen);
    ui.xft = fontset_open(&ui.fs, ui.dpy, ui.screen);
    if (ui.xft && !ui.fs.api.XftColorAllocName(ui.dpy, ui.visual, ui.cmap, "black", &ui.ink)) {
        fontset_close(&ui.fs);
        ui.xft = 0;
    }
    if (!ui.xft) {
        ui.font = XLoadQueryFont(ui.dpy, "fixed");
        ui.unicode_font = font_is_unicode(ui.dpy, ui.font);
    }
    ui.width = START_WIDTH;
    ui.height = START_HEIGHT;
    ui.eol = 1; /* draws the banner path as well */
    attrs.background_pixel = ui.white;
    ui.win = XCreateWindow(ui.dpy, RootWindow(ui.dpy, ui.screen), 0, 0, (unsigned)ui.width,
                           (unsigned)ui.height, 0, CopyFromParent, InputOutput, CopyFromParent,
                           CWBackPixel, &attrs);
    ui.gc = XCreateGC(ui.dpy, ui.win, 0, NULL);
    if (ui.font != NULL)
        XSetFont(ui.dpy, ui.gc, ui.font->fid);
    XSetForeground(ui.dpy, ui.gc, ui.black);
    if (ui.xft)
        ui.draw = ui.fs.api.XftDrawCreate(ui.dpy, ui.win, ui.visual, ui.cmap);
    open_input_method(&ui);
    XMapWindow(ui.dpy, ui.win);
    XSync(ui.dpy, False);
    printf("engine: %s; input method: %s\n", ui.xft ? "Xft" : "core font",
           ui.ic != NULL ? "yes (Xutf8LookupString)" : "none (XLookupString + keysym table)");

    run_text(&ui, "empty", (const unsigned char *)"", 0, 0);
    run_text(&ui, "mixed",
             (const unsigned char *)"Zo\xc3\xa9 \xe2\x82\xac \xce\x95\xce\xbb \xd7\xa9 \xd9\x85 "
                                    "\xe6\x97\xa5 \xf0\x9f\x98\x80",
             28, 0);
    run_text(&ui, "newline and tab", (const unsigned char *)"a\nb\tc\r\n", 7, 0);
    run_text(&ui, "NUL-free control chars", (const unsigned char *)"\x01\x02\x1b\x7f", 4, 1);
    run_text(&ui, "invalid: lone continuation", (const unsigned char *)"a\x80\x80z", 4, 1);
    run_text(&ui, "invalid: truncated 3-byte", (const unsigned char *)"ab\xe6\x97", 4, 1);
    run_text(&ui, "invalid: overlong",
             (const unsigned char *)"\xc0\x80\xe0\x80\x80\xf0\x80\x80\x80", 9, 1);
    run_text(&ui, "invalid: surrogate D800", (const unsigned char *)"\xed\xa0\x80", 3, 1);
    run_text(&ui, "invalid: above 10FFFF",
             (const unsigned char *)"\xf4\x90\x80\x80\xf8\x88\x80\x80\x80", 9, 1);
    run_text(&ui, "invalid: 0xFF 0xFE", (const unsigned char *)"\xff\xfe", 2, 1);
    run_text(&ui, "invalid through the backend", (const unsigned char *)"\xff\xfe\xe6", 3, 0);
    run_text(&ui, "noncharacters + private use",
             (const unsigned char *)"\xef\xbf\xbe\xef\xbf\xbf\xee\x80\x80\xf3\xb0\x80\x80", 13, 0);
    run_text(&ui, "format characters",
             (const unsigned char *)"a\xe2\x80\x8d\xe2\x80\x8b\xef\xb8\x8f\xe2\x80\xae"
                                    "b",
             15, 0);
    run_text(
        &ui, "combining + ZWJ family",
        (const unsigned char
             *)"e\xcc\x81 \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7",
        22, 0);
    for (i = 0; i < 1000; i++) {
        big[i * 2] = 0xD0; /* U+0436 */
        big[i * 2 + 1] = 0xB6;
    }
    run_text(&ui, "1000 two-byte (2000 bytes)", big, 2000, 0);
    for (i = 0; i < 1000; i++) {
        big[i * 3] = 0xE6; /* U+65E5 */
        big[i * 3 + 1] = 0x97;
        big[i * 3 + 2] = 0xA5;
    }
    run_text(&ui, "1000 three-byte (3000 bytes)", big, 3000, 0);
    for (i = 0; i < 1000; i++) {
        big[i * 4] = 0xF0; /* U+1F600 */
        big[i * 4 + 1] = 0x9F;
        big[i * 4 + 2] = 0x98;
        big[i * 4 + 3] = 0x80;
    }
    run_text(&ui, "1000 four-byte (4000 bytes)", big, 4000, 0);
    memset(big, 'x', 1000);
    run_text(&ui, "1000 ASCII, one word", big, 1000, 0);
    /* random bytes, valid or not, drawn raw */
    for (i = 0; i < sizeof(rnd); i++) {
        seed = seed * 1103515245u + 12345u;
        rnd[i] = (unsigned char)(seed >> 16);
    }
    run_text(&ui, "4000 random bytes, drawn raw", rnd, 4000, 1);
    for (i = 0; i < sizeof(rnd); i++) {
        seed = seed * 1103515245u + 12345u;
        rnd[i] = (unsigned char)(0x80 | ((seed >> 16) & 0x7F));
    }
    run_text(&ui, "4000 continuation bytes, drawn raw", rnd, 4000, 1);
    /* the append and delete paths: 1,010 characters typed, the limit holds */
    ui.input_len = 0;
    ui.input_chars = 0;
    ui.input[0] = '\0';
    for (i = 0; i < 1010; i++)
        append_char(&ui, i % 3 == 0 ? 0x65E5 : (i % 3 == 1 ? 0x436 : 0x1F600));
    printf("typed 1010 characters: %lu characters, %lu bytes (limit %d characters)\n",
           (unsigned long)ui.input_chars, (unsigned long)ui.input_len, MAX_INPUT_CHARS);
    for (i = 0; i < 1005; i++)
        delete_last(&ui);
    printf("after 1005 deletions: %lu characters, %lu bytes, valid UTF-8 %d\n",
           (unsigned long)ui.input_chars, (unsigned long)ui.input_len, utf8_valid(ui.input));

    if (ui.ic != NULL)
        XDestroyIC(ui.ic);
    if (ui.xim != NULL)
        XCloseIM(ui.xim);
    if (ui.xft) {
        ui.fs.api.XftDrawDestroy(ui.draw);
        ui.fs.api.XftColorFree(ui.dpy, ui.visual, ui.cmap, &ui.ink);
        fontset_close(&ui.fs);
    }
    XFreeGC(ui.dpy, ui.gc);
    if (ui.font != NULL)
        XFreeFont(ui.dpy, ui.font);
    XDestroyWindow(ui.dpy, ui.win);
    XCloseDisplay(ui.dpy);
    return 0;
}
