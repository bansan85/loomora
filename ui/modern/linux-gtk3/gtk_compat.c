/* Main loop of the Linux Modern UI: see gtk_compat.h. */
#include "gtk_compat.h"

#ifdef LOOMORA_GTK4

static GMainLoop *g_loop;

void loomora_main_loop_run(void) {
    g_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(g_loop);
    g_main_loop_unref(g_loop);
    g_loop = NULL;
}

void loomora_main_loop_quit(void) {
    if (g_loop != NULL)
        g_main_loop_quit(g_loop);
}

#else

void loomora_main_loop_run(void) { gtk_main(); }

void loomora_main_loop_quit(void) { gtk_main_quit(); }

#endif
