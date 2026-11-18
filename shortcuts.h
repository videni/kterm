/* shortcuts.h
 *
 * This file is part of kterm
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef shortcuts_h
#define shortcuts_h

#include <glib.h>

/** Name of the file, looked for where kterm.conf is */
#define SHORTCUTS_FILE "shortcuts.conf"
/** Buttons kept; past one row they are reached by swiping the bar */
#define SHORTCUTS_MAX 48
/** Used when there is no file */
#define SHORTCUTS_DEFAULT "C-a\nC-e\nC-u\nC-y\nC-w\nC-c\nTX = C-Space\n"

/** One button of the shortcut bar */
typedef struct {
    gchar *label; /** Text on the button */
    gchar *spec; /** Keys as written, see keyseq.c */
    GBytes *keys; /** What a tap sends to the terminal */
} Shortcut;

Shortcut * shortcut_new(const gchar *name, const gchar *spec, GError **error);
void shortcut_free(gpointer item);
GPtrArray * shortcuts_parse(const gchar *text, gchar **problems);
GPtrArray * shortcuts_load(const gchar *path, gchar **problems);
gchar * shortcuts_format(const gchar *old_text, GPtrArray *list);
gboolean shortcuts_save(const gchar *path, GPtrArray *list, GError **error);

#endif /* shortcuts_h */
