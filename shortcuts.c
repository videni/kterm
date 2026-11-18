/* shortcuts.c
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
 * The buttons of the shortcut bar, read from a text file with one button
 * per line, left to right:
 *
 *     C-u                     the button shows the keys as written
 *     TX = C-Space            the button shows the name
 *     拉代码 = "git pull" Enter
 *
 * Keys are written as for keyseq_parse(). Blank lines and lines starting
 * with # are skipped; a line that cannot be read is skipped and reported,
 * the others still make their buttons.
 */

#include <string.h>
#include "keyseq.h"
#include "shortcuts.h"

static void shortcut_free(gpointer data) {
    Shortcut *item = data;
    g_free(item->label);
    g_bytes_unref(item->keys);
    g_free(item);
}

/**
 * @param line Line without surrounding blanks
 * @return Position of the = between a name and its keys, or NULL. An = in
 *  quoted text is text, and one right after a modifier (M-=) is a key.
 */
static const gchar * find_separator(const gchar *line) {
    gboolean quoted = FALSE;
    for (const gchar *p = line; *p; p++) {
        if (quoted && *p == '\\' && p[1]) {
            p++;
        } else if (*p == '"') {
            quoted = !quoted;
        } else if (*p == '=' && !quoted && !(p > line && p[-1] == '-')) {
            return p;
        }
    }
    return NULL;
}

/**
 * Read one line
 * @param line Line without surrounding blanks, not empty
 * @param error Set when the line makes no button
 * @return Button, or NULL
 */
static Shortcut * parse_line(const gchar *line, GError **error) {
    const gchar *separator = find_separator(line);
    gchar *label = NULL;
    gchar *spec = NULL;
    if (separator && separator[1] == '\0' && separator == line) {
        // the = key on its own
        label = g_strdup(line);
        spec = g_strdup(line);
    } else if (separator) {
        label = g_strstrip(g_strndup(line, (gsize) (separator - line)));
        spec = g_strstrip(g_strdup(separator + 1));
        if (!*label) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "等号前面缺少名字");
        } else if (!*spec) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "等号后面缺少按键");
        }
    } else {
        label = g_strdup(line);
        spec = g_strdup(line);
    }
    GBytes *keys = NULL;
    if (!(error && *error)) {
        keys = keyseq_parse(spec, error);
    }
    g_free(spec);
    if (!keys) {
        g_free(label);
        return NULL;
    }
    Shortcut *item = g_new0(Shortcut, 1);
    item->label = label;
    item->keys = keys;
    return item;
}

/**
 * Read the buttons from the text of the file
 * @param text File contents
 * @param problems Set to the lines skipped and why, one per line, or NULL
 *  when there were none. Free with g_free().
 * @return Buttons, in order; free with g_ptr_array_unref()
 */
GPtrArray * shortcuts_parse(const gchar *text, gchar **problems) {
    GPtrArray *list = g_ptr_array_new_with_free_func(shortcut_free);
    GString *report = g_string_new(NULL);
    gchar **lines = g_strsplit(text ? text : "", "\n", -1);
    for (guint i = 0; lines[i]; i++) {
        gchar *line = g_strstrip(lines[i]);
        if (!*line || *line == '#') {
            continue;
        }
        if (list->len == SHORTCUTS_MAX) {
            g_string_append_printf(report, "第 %u 行起：最多 %d 个按钮，后面的没有放上\n", i + 1, SHORTCUTS_MAX);
            break;
        }
        GError *error = NULL;
        Shortcut *item = parse_line(line, &error);
        if (item) {
            g_ptr_array_add(list, item);
        } else {
            g_string_append_printf(report, "第 %u 行：%s\n", i + 1, error ? error->message : "看不懂");
            g_clear_error(&error);
        }
    }
    g_strfreev(lines);
    if (problems) {
        *problems = report->len ? g_string_free(report, FALSE) : (g_string_free(report, TRUE), NULL);
    } else {
        g_string_free(report, TRUE);
    }
    return list;
}

/**
 * Read the buttons from a file
 * @param path File; when it does not exist, the built-in buttons are used
 * @param problems As for shortcuts_parse(), also set when the file cannot
 *  be read (the built-in buttons are used then)
 * @return Buttons, in order; free with g_ptr_array_unref()
 */
GPtrArray * shortcuts_load(const gchar *path, gchar **problems) {
    gchar *text = NULL;
    GError *error = NULL;
    if (!path || !g_file_get_contents(path, &text, NULL, &error)) {
        GPtrArray *list = shortcuts_parse(SHORTCUTS_DEFAULT, problems);
        if (error && !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT) && problems) {
            g_free(*problems);
            *problems = g_strdup_printf("读不了 %s：%s\n", path, error->message);
        }
        g_clear_error(&error);
        return list;
    }
    GPtrArray *list = shortcuts_parse(text, problems);
    g_free(text);
    return list;
}
