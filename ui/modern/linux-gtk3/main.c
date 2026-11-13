/* Loomora Modern UI for Linux: C and GTK. The floor is the GTK 3.10 API (GtkWindow, GtkBox,
   GtkEntry, GtkLabel, GtkScrolledWindow, GtkInfoBar); newer GTK features are used only behind
   the LOOMORA_GTK_HAS_* macros of gtk_compat.h, and the GTK 4 branch (-Dgtk_api=4) is selected
   by LOOMORA_GTK4. No application logic: the greeting, the end-of-life test and its warning
   text and the probe line all come from the backend through loomora.h. */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#include "gtk_compat.h"
#include "loomora.h"
#include "probe.h"

#define MAX_INPUT_CHARS 1000
/* Largest greeting: 1,000 characters of at most 4 bytes each, plus "Bonjour ". */
#define GREETING_CAP (4096 + 64)

static void show_greeting(GtkEntry *entry, GtkLabel *label) {
    const char *text = loomora_entry_get_text(entry);
    uint8_t out[GREETING_CAP];
    int32_t n = loomora_greet(text, strlen(text), out, sizeof(out));
    if (n >= 0) {
        char *greeting = g_strndup((const char *)out, (gsize)n);
        gtk_label_set_text(label, greeting);
        g_free(greeting);
    } else {
        char *error = g_strdup_printf("The greeting could not be computed (error %d).", (int)n);
        gtk_label_set_text(label, error);
        g_free(error);
    }
}

static void on_entry_changed(GtkEditable *editable, gpointer label) {
    show_greeting(GTK_ENTRY(editable), GTK_LABEL(label));
}

static void on_window_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    (void)data;
    loomora_main_loop_quit();
}

/* Major and minor kernel version, the version the backend's Linux end-of-life rule is
   expressed in. Returns FALSE when it cannot be read. */
static gboolean kernel_version(int *major, int *minor) {
    struct utsname un;
    if (uname(&un) != 0)
        return FALSE;
    return sscanf(un.release, "%d.%d", major, minor) == 2;
}

/* Horizontal alignment of the text of a wrapped label: left, mirrored in a right-to-left
   locale. GtkMisc alignment is the 3.10 way. */
static void set_label_text_start(GtkWidget *label) {
#ifdef LOOMORA_GTK_HAS_LABEL_XALIGN
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
#else
    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
#endif
}

/* The text of the greeting: centred, 1.8 times the size of the theme font, wrapped on words and
   on characters for text without spaces. */
static GtkWidget *create_greeting_label(void) {
    GtkWidget *greeting = gtk_label_new("");
    PangoAttrList *attrs;
#ifdef LOOMORA_GTK4
    gtk_label_set_wrap(GTK_LABEL(greeting), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(greeting), PANGO_WRAP_WORD_CHAR);
#else
    gtk_label_set_line_wrap(GTK_LABEL(greeting), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(greeting), PANGO_WRAP_WORD_CHAR);
#endif
    gtk_label_set_justify(GTK_LABEL(greeting), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(greeting, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(greeting, GTK_ALIGN_CENTER);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.8)); /* colours come from the theme */
    gtk_label_set_attributes(GTK_LABEL(greeting), attrs);
    pango_attr_list_unref(attrs);
    return greeting;
}

/* A greeting of up to 1,000 characters wraps to many lines: without a scrolled window the
   label overflows the window, is clipped (blank at 250 characters) and the window grows,
   instead of scrolling. */
static GtkWidget *create_scroller(GtkWidget *greeting) {
#ifdef LOOMORA_GTK4
    GtkWidget *scroller = gtk_scrolled_window_new();
#else
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
#endif
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
#ifdef LOOMORA_GTK_HAS_OVERLAY_SCROLLING
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroller), FALSE);
#endif
#ifdef LOOMORA_GTK4
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), greeting);
    gtk_widget_set_vexpand(scroller, TRUE);
#else
    gtk_container_add(GTK_CONTAINER(scroller), greeting);
#endif
    return scroller;
}

#ifdef LOOMORA_GTK4

/* GTK 4 has no "draw" signal: the first display is the end of the first frame the frame clock
   of the window paints. */
static void on_after_paint(GdkFrameClock *clock, gpointer data) {
    (void)clock;
    (void)data;
    probe_mark_displayed(); /* only the first call counts */
}

static void on_window_realize(GtkWidget *widget, gpointer data) {
    GdkFrameClock *clock = gtk_widget_get_frame_clock(widget);
    (void)data;
    if (clock != NULL)
        g_signal_connect(clock, "after-paint", G_CALLBACK(on_after_paint), NULL);
}

static void on_eol_close_clicked(GtkButton *button, gpointer bar) {
    (void)button;
    gtk_widget_set_visible(GTK_WIDGET(bar), FALSE);
}

/* GtkInfoBar is deprecated since GTK 4.10 and has no replacement: a plain box with the warning
   icon, the text and a close button. Unlike the GTK 3 bar it has no warning background. */
static GtkWidget *create_eol_bar(void) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
    GtkWidget *text = gtk_label_new(loomora_eol_warning());
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    set_label_text_start(text);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_widget_set_valign(close, GTK_ALIGN_START);
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    g_signal_connect(close, "clicked", G_CALLBACK(on_eol_close_clicked), bar);
    gtk_box_append(GTK_BOX(bar), icon);
    gtk_box_append(GTK_BOX(bar), text);
    gtk_box_append(GTK_BOX(bar), close);
    return bar;
}

#else

static gboolean on_first_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    (void)widget;
    (void)cr;
    (void)data;
    probe_mark_displayed(); /* only the first call counts */
    return FALSE;           /* let GTK carry on */
}

static void on_info_bar_response(GtkInfoBar *bar, gint response_id, gpointer data) {
    (void)response_id;
    (void)data;
    gtk_widget_hide(GTK_WIDGET(bar));
}

static GtkWidget *create_eol_bar(void) {
    GtkWidget *bar = gtk_info_bar_new();
    GtkWidget *text = gtk_label_new(loomora_eol_warning());
    gtk_info_bar_set_message_type(GTK_INFO_BAR(bar), GTK_MESSAGE_WARNING);
    gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
    set_label_text_start(text);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(bar))), text);
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(bar), TRUE);
    g_signal_connect(bar, "response", G_CALLBACK(on_info_bar_response), NULL);
    return bar;
}

#endif

int main(int argc, char **argv) {
    ProbeOptions probe;
    int exit_status = 0;
    int kernel_major = 0;
    int kernel_minor = 0;
    gboolean eol = FALSE;
    GtkWidget *window;
    GtkWidget *box;
    GtkWidget *entry;
    GtkWidget *greeting;
    GtkWidget *scroller;

    probe_start();

    if (loomora_abi_version() != LOOMORA_ABI_VERSION) {
        fprintf(stderr, "loomora: backend ABI version %u, expected %d\n",
                (unsigned)loomora_abi_version(), LOOMORA_ABI_VERSION);
        return 1;
    }
    if (!loomora_init_check(&argc, &argv)) {
        fprintf(stderr, "loomora: cannot open the display\n");
        return 1;
    }
    probe_parse_args(argc, argv, &probe);
    if (probe.bad_usage || (probe.input != NULL && !g_utf8_validate(probe.input, -1, NULL))) {
        fprintf(stderr, "usage: %s [--probe [--probe-input <utf8>]]\n", argv[0]);
        return 2;
    }

#ifdef LOOMORA_GTK4
    window = gtk_window_new();
#else
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#endif
    gtk_window_set_title(GTK_WINDOW(window), "Loomora");
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 240);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
#ifdef LOOMORA_GTK4
    /* GTK 4 has no border width: the 12 pixels around the content are margins of the box. */
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    g_signal_connect(window, "realize", G_CALLBACK(on_window_realize), NULL);
    gtk_window_set_child(GTK_WINDOW(window), box);
#else
    gtk_container_set_border_width(GTK_CONTAINER(window), 12);
    g_signal_connect_after(window, "draw", G_CALLBACK(on_first_draw), NULL);
    gtk_container_add(GTK_CONTAINER(window), box);
#endif

    if (kernel_version(&kernel_major, &kernel_minor) &&
        loomora_is_eol(LOOMORA_FAMILY_LINUX, kernel_major, kernel_minor) == 1) {
        GtkWidget *bar = create_eol_bar();
        eol = TRUE;
#ifdef LOOMORA_GTK4
        gtk_box_append(GTK_BOX(box), bar);
#else
        gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
#endif
    }

    entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry), MAX_INPUT_CHARS);
    loomora_entry_set_text(GTK_ENTRY(entry), probe.input != NULL ? probe.input : "Coucou");
#ifdef LOOMORA_GTK4
    gtk_box_append(GTK_BOX(box), entry);
#else
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
#endif

    greeting = create_greeting_label();
    scroller = create_scroller(greeting);
#ifdef LOOMORA_GTK4
    gtk_box_append(GTK_BOX(box), scroller);
#else
    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);
#endif

    g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), greeting);
    show_greeting(GTK_ENTRY(entry), GTK_LABEL(greeting));

#ifdef LOOMORA_GTK4
    gtk_window_present(GTK_WINDOW(window));
#else
    gtk_widget_show_all(window);
#endif
    if (probe.enabled)
        probe_schedule_exit(GTK_ENTRY(entry), GTK_LABEL(greeting), eol, &exit_status);
    loomora_main_loop_run();
    return exit_status;
}
