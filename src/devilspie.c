/*
 * Copyright (C) 2005 Ross Burton <ross@burtonini.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <libwnck/libwnck.h>
#include "e-sexp.h"
#include "devilspie.h"
#include "parser.h"

/* Global state */

/**
 * The list of s-expressions to evaluate.
 */
GList *sexps = NULL;

/**
 * If we are in debug mode.
 */
gboolean debug = FALSE;

/**
 * Matching context
 */
Context context = {NULL,};

/**
 * If "if" expression is ever satisfied for one window.
 */
gboolean if_satisified = FALSE;

/* Private state */

/**
 * If we apply to existing windows or not.
 */
static gboolean apply_to_existing = FALSE;

/**
 * If we apply to existing windows and immediately quit the program.
 */
static gboolean apply_to_existing_and_quit = FALSE;

/**
 * Apply and quit the program after the first if-matching window is encountered
 */
static gboolean apply_to_first_match_and_quit = FALSE;

/**
 * List of configuration files to open, or NULL if to open the default files.
 */
static char **files = NULL;

/**
 * The main loop.
 */
GMainLoop *loop = NULL;

/**
 * Evaluate a s-expression.
 */
static void run_sexp(ESExp * sexp, WnckWindow *window) {
  if (!(if_satisified && apply_to_first_match_and_quit)) {
    context.window = window;
    /* We really don't care what the top-level s-expressions evaluate to */
    e_sexp_result_free (sexp, e_sexp_eval(sexp));
  }
  // else {
  //   g_printerr("if_satisified already. skipping...\n");
  // }
}

/**
 * This callback is called whenever a window is opened on a screen.
 */
static void window_opened_cb(WnckScreen *screen, WnckWindow *window) {
  g_list_foreach(sexps, (GFunc)run_sexp, window);
  return;
}

/**
 * Connect to every screen on this display and watch for new windows.
 */
static void init_screens(void) {
  int i;
  const int num_screens = gdk_display_get_n_screens (gdk_display_get_default());
  for (i = 0 ; i < num_screens; ++i) {
    WnckScreen *screen = wnck_screen_get (i);
    /* Connect a callback to the window opened event in libwnck */
    g_signal_connect (screen, "window_opened", (GCallback)window_opened_cb, NULL);
    if (apply_to_existing)    // either 'a' or 'q' option is specified.
      wnck_screen_force_update (screen);
  }
}

/*
 * Dedicated to Vicky.
 */
int main(int argc, char **argv) {
  static const GOptionEntry options[] = {
    { "apply-to-existing", 'a', 0, G_OPTION_ARG_NONE, &apply_to_existing, N_("Apply to all existing windows instead of just new windows."), NULL },
    { "quit", 'q', 0, G_OPTION_ARG_NONE, &apply_to_existing_and_quit, N_("Apply to all existing windows and immediately quit the program."), NULL },
    { "quit1", 'Q', 0, G_OPTION_ARG_NONE, &apply_to_first_match_and_quit, N_("Apply and quit the program after the first if-matching window is encountered."), NULL },
    { "debug", 'd', 0, G_OPTION_ARG_NONE, &debug, N_("Output debug information"), NULL },
    { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_FILENAME_ARRAY, &files, N_("Configuration files to use"), NULL },
    { NULL }
  };

  GError *error = NULL;
  GOptionContext *context;

  /* Initialise i18n */
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  /* Initialise GDK */
  gdk_init(&argc, &argv);

  /* Parse the arguments */
  context = g_option_context_new ("- Devil's Pie " VERSION);
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);

  if (apply_to_existing_and_quit || apply_to_first_match_and_quit)
    apply_to_existing = TRUE;

  if (debug) g_printerr(_("Devil's Pie %s starting...\n"), VERSION);

  /* If there were files specified use those, otherwise load the default configuration */
  if (files) {
    while (*files) {
      GList *s;
      s = load_configuration_file (*files++);
      if (s) sexps = g_list_concat (sexps, s);
    }
  } else {
    load_configuration ();
  }

  if (debug) g_printerr("%d s-expressions loaded.\n", g_list_length(sexps));

  if (g_list_length (sexps) == 0) {
    g_printerr(_("No s-expressions loaded, quitting\n"));
    return 1;
  }

  /* Connect to every screen */
  init_screens ();

  if (!apply_to_existing_and_quit && !apply_to_first_match_and_quit) {
    /* Go go go! */
    loop = g_main_loop_new (NULL, TRUE);
    g_main_loop_run (loop);
  }
  return 0;
}
