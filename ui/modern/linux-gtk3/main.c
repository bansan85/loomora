/* Loomora Modern UI for Linux: C and GTK 3, 3.10 API subset only (GtkWindow, GtkBox, GtkEntry,
   GtkLabel, GtkInfoBar). No application logic: the greeting, the end-of-life test and its
   warning text and the probe line all come from the backend through loomora.h. */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#include "loomora.h"
#include "probe.h"

#define MAX_INPUT_CHARS 1000
/* Largest greeting: 1,000 characters of at most 4 bytes each, plus "Bonjour ". */
#define GREETING_CAP (4096 + 64)

static void show_greeting(GtkEntry *entry, GtkLabel *label) {
    const char *text = gtk_entry_get_text(entry);
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

/* Major and minor kernel version, the version the backend's Linux end-of-life rule is
   expressed in. Returns FALSE when it cannot be read. */
static gboolean kernel_version(int *major, int *minor) {
    struct utsname un;
    if (uname(&un) != 0)
        return FALSE;
    return sscanf(un.release, "%d.%d", major, minor) == 2;
}

static GtkWidget *create_eol_bar(void) {
    GtkWidget *bar = gtk_info_bar_new();
    GtkWidget *text = gtk_label_new(loomora_eol_warning());
    gtk_info_bar_set_message_type(GTK_INFO_BAR(bar), GTK_MESSAGE_WARNING);
    gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
    gtk_widget_set_halign(text, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(bar))), text);
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(bar), TRUE);
    g_signal_connect(bar, "response", G_CALLBACK(on_info_bar_response), NULL);
    return bar;
}

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
    PangoAttrList *attrs;

    probe_start();

    if (loomora_abi_version() != LOOMORA_ABI_VERSION) {
        fprintf(stderr, "loomora: backend ABI version %u, expected %d\n",
                (unsigned)loomora_abi_version(), LOOMORA_ABI_VERSION);
        return 1;
    }
    if (!gtk_init_check(&argc, &argv)) {
        fprintf(stderr, "loomora: cannot open the display\n");
        return 1;
    }
    probe_parse_args(argc, argv, &probe);
    if (probe.bad_usage || (probe.input != NULL && !g_utf8_validate(probe.input, -1, NULL))) {
        fprintf(stderr, "usage: %s [--probe [--probe-input <utf8>]]\n", argv[0]);
        return 2;
    }

    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Loomora");
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 240);
    gtk_container_set_border_width(GTK_CONTAINER(window), 12);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect_after(window, "draw", G_CALLBACK(on_first_draw), NULL);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_add(GTK_CONTAINER(window), box);

    if (kernel_version(&kernel_major, &kernel_minor) &&
        loomora_is_eol(LOOMORA_FAMILY_LINUX, kernel_major, kernel_minor) == 1) {
        eol = TRUE;
        gtk_box_pack_start(GTK_BOX(box), create_eol_bar(), FALSE, FALSE, 0);
    }

    entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(entry), MAX_INPUT_CHARS);
    gtk_entry_set_text(GTK_ENTRY(entry), probe.input != NULL ? probe.input : "Coucou");
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);

    greeting = gtk_label_new("");
    gtk_label_set_line_wrap(GTK_LABEL(greeting), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(greeting), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_justify(GTK_LABEL(greeting), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(greeting, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(greeting, GTK_ALIGN_CENTER);
    attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.8)); /* colours come from the theme */
    gtk_label_set_attributes(GTK_LABEL(greeting), attrs);
    pango_attr_list_unref(attrs);
    /* A greeting of up to 1,000 characters wraps to many lines: without a scrolled window the
       label overflows the window, is clipped (blank at 250 characters) and the window
       grows, instead of scrolling. */
    scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroller), greeting);
    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);

    g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), greeting);
    show_greeting(GTK_ENTRY(entry), GTK_LABEL(greeting));

    gtk_widget_show_all(window);
    if (probe.enabled)
        probe_schedule_exit(GTK_ENTRY(entry), GTK_LABEL(greeting), eol, &exit_status);
    gtk_main();
    return exit_status;
}
