/* confedit.c
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

/*
 * Write one setting back to kterm.conf, keeping the rest of the file (other
 * settings, comments) as the user left it.
 */

#include <stdlib.h>
#include <string.h>
#include "confedit.h"

/**
 * @param line Line of the file
 * @param key Setting name
 * @return TRUE if the line sets this key (not a comment, not a longer name)
 */
static gboolean line_sets(const gchar *line, const gchar *key) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    const gsize length = strlen(key);
    if (strncmp(line, key, length) != 0) {
        return FALSE;
    }
    line += length;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return *line == '=';
}

/**
 * Set a key in the text of a configuration file
 * @param text File contents, may be NULL
 * @param key Setting name, e.g. "font_size"
 * @param line The whole line to put, e.g. "font_size = 10"
 * @return New contents: the key's first line replaced, its other lines
 *  dropped, or the line added at the end. Free with g_free().
 */
gchar * confedit_set(const gchar *text, const gchar *key, const gchar *line) {
    GString *out = g_string_new(NULL);
    gboolean placed = FALSE;
    gchar **lines = g_strsplit(text ? text : "", "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        // the piece after the last newline is not a line
        if (!lines[i + 1] && !*lines[i]) {
            break;
        }
        if (line_sets(lines[i], key)) {
            if (!placed) {
                g_string_append_printf(out, "%s\n", line);
                placed = TRUE;
            }
            continue;
        }
        g_string_append_printf(out, "%s\n", lines[i]);
    }
    g_strfreev(lines);
    if (!placed) {
        g_string_append_printf(out, "%s\n", line);
    }
    return g_string_free(out, FALSE);
}

/**
 * Set a key in a configuration file, creating it (and its directory) if
 * needed. The file is replaced as a whole, never left half written.
 * @param path File
 * @param key Setting name
 * @param line The whole line to put
 * @param error Set on failure
 * @return TRUE on success
 */
gboolean confedit_save(const gchar *path, const gchar *key, const gchar *line, GError **error) {
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    gchar *text = NULL;
    GError *read_error = NULL;
    if (!g_file_get_contents(path, &text, NULL, &read_error)
        && !g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        // never replace a file that is there but could not be read
        g_propagate_error(error, read_error);
        return FALSE;
    }
    g_clear_error(&read_error);
    gchar *updated = confedit_set(text, key, line);
    // through a symbolic link: the file it points to is the one replaced
    char *real = realpath(path, NULL);
    const gboolean saved = g_file_set_contents(real ? real : path, updated, -1, error);
    free(real);
    g_free(updated);
    g_free(text);
    return saved;
}
