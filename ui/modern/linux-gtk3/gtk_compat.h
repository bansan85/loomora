/* GTK version policy of the Linux Modern UI (CLAUDE.md, docs/implementation/adr/0005-gtk-version-policy.md).

   The floor is GTK 3.10: the UI must always build and run with only the 3.10 API. A newer
   feature is used only behind ONE macro of this file, LOOMORA_GTK_HAS_<FEATURE>, computed from
   the version of the headers being built against; without the macro the code takes the 3.10
   path. Every macro below is declared in docs/implementation/validation/fallbacks.json.

   Two build options of meson.build drive this file:
     -Dgtk_api=4     build against GTK 4 (LOOMORA_GTK4); the default is GTK 3.
     -Dgtk_floor=true  pin GDK_VERSION_MAX_ALLOWED to 3.10: every macro that needs a newer GTK is
                       then off (the "&& GDK_VERSION_MAX_ALLOWED >= ..." terms), so the floor
                       build compiles the 3.10 path only, with -Werror=deprecated-declarations
                       making any symbol newer than 3.10 fail the build. */
#ifndef LOOMORA_GTK3_GTK_COMPAT_H
#define LOOMORA_GTK3_GTK_COMPAT_H

#include <gtk/gtk.h>

#if GTK_MAJOR_VERSION >= 4

/* GTK 4 branch: a different window construction and main loop (main.c, gtk_compat.c). The
   features below all exist in GTK 4. */
#define LOOMORA_GTK4 1
#define LOOMORA_GTK_HAS_LABEL_XALIGN 1
#define LOOMORA_GTK_HAS_OVERLAY_SCROLLING 1

#else

/* gtk_label_set_xalign (GTK 3.16) replaces gtk_misc_set_alignment (GtkMisc, deprecated in 3.14,
   removed in GTK 4). */
#if GTK_CHECK_VERSION(3, 16, 0) && GDK_VERSION_MAX_ALLOWED >= GDK_VERSION_3_16
#define LOOMORA_GTK_HAS_LABEL_XALIGN 1
#endif

/* gtk_scrolled_window_set_overlay_scrolling (GTK 3.16): GTK 3.10 has no overlay scrollbars, its
   scrollbar always takes room next to the content. From 3.16 the default is an overlay that
   is hidden until the pointer moves, so nothing shows that a long greeting scrolls and the
   scrollbar covers the text once shown; switching overlay scrolling off gives the 3.10 look. */
#if GTK_CHECK_VERSION(3, 16, 0) && GDK_VERSION_MAX_ALLOWED >= GDK_VERSION_3_16
#define LOOMORA_GTK_HAS_OVERLAY_SCROLLING 1
#endif

#endif

/* Calls that differ between GTK 3 and GTK 4 and are not optional features: the same name for
   both, the GTK 3 one being the 3.10 API. */
#ifdef LOOMORA_GTK4
static inline gboolean loomora_init_check(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    return gtk_init_check();
}
static inline const char *loomora_entry_get_text(GtkEntry *entry) {
    return gtk_editable_get_text(GTK_EDITABLE(entry));
}
static inline void loomora_entry_set_text(GtkEntry *entry, const char *text) {
    gtk_editable_set_text(GTK_EDITABLE(entry), text);
}
#else
static inline gboolean loomora_init_check(int *argc, char ***argv) {
    return gtk_init_check(argc, argv);
}
static inline const char *loomora_entry_get_text(GtkEntry *entry) {
    return gtk_entry_get_text(entry);
}
static inline void loomora_entry_set_text(GtkEntry *entry, const char *text) {
    gtk_entry_set_text(entry, text);
}
#endif

/* Main loop: gtk_main / gtk_main_quit in GTK 3, a GMainLoop in GTK 4 (which has neither). */
void loomora_main_loop_run(void);
void loomora_main_loop_quit(void);

#endif
