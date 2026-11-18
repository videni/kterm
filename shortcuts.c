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
#include <glib/gstdio.h>
#include "keyseq.h"
#include "shortcuts.h"

/**
 * Free a button
 * @param data Shortcut
 */
void shortcut_free(gpointer data) {
    Shortcut *item = data;
    if (!item) {
        return;
    }
    g_free(item->label);
    g_free(item->spec);
    g_bytes_unref(item->keys);
    g_free(item);
}

/**
 * Make a button, as typed into the panel's form
 * @param name Text for the button; empty: the keys are shown
 * @param spec Keys as written, see keyseq.c
 * @param error Set when the button could not be written to the file and
 *  read back the same (message in Chinese, shown to the user)
 * @return Button, or NULL
 */
Shortcut * shortcut_new(const gchar *name, const gchar *spec, GError **error) {
    gchar *label = g_strstrip(g_strdup(name ? name : ""));
    gchar *keys_text = g_strstrip(g_strdup(spec ? spec : ""));
    if (*label && (strchr(label, '=') || *label == '#')) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "名字里不能有 = 号，也不能以 # 开头");
    } else if (!*label && *keys_text == '#') {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "以 # 开头的按键请起个名字");
    }
    GBytes *keys = NULL;
    if (!(error && *error)) {
        keys = keyseq_parse(keys_text, error);
    }
    if (!keys) {
        g_free(label);
        g_free(keys_text);
        return NULL;
    }
    Shortcut *item = g_new0(Shortcut, 1);
    if (*label) {
        item->label = label;
    } else {
        g_free(label);
        item->label = g_strdup(keys_text);
    }
    item->spec = keys_text;
    item->keys = keys;
    return item;
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
    if (!keys) {
        g_free(spec);
        g_free(label);
        return NULL;
    }
    Shortcut *item = g_new0(Shortcut, 1);
    item->label = label;
    item->spec = spec;
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

/** On top of a file written from the panel when there was none */
#define SHORTCUTS_NOTE "# kterm 快捷栏：一行一个按钮，写法见面板里的“快捷键”页\n\n"

/**
 * The text of the file for a list of buttons
 * @param old_text The file as it was, or NULL; the comments on its top are
 *  kept, everything after them is replaced
 * @param list Buttons, in order
 * @return New contents, free with g_free()
 */
gchar * shortcuts_format(const gchar *old_text, GPtrArray *list) {
    GString *out = g_string_new(NULL);
    if (old_text) {
        const gchar *p = old_text;
        while (*p) {
            const gchar *end = strchr(p, '\n');
            const gchar *next = end ? end + 1 : p + strlen(p);
            gchar *line = g_strstrip(g_strndup(p, (gsize) (next - p)));
            const gboolean note = !*line || *line == '#';
            g_free(line);
            if (!note) {
                break;
            }
            g_string_append_len(out, p, next - p);
            if (!end) {
                g_string_append_c(out, '\n');
            }
            p = next;
        }
    } else {
        g_string_append(out, SHORTCUTS_NOTE);
    }
    for (guint i = 0; i < list->len; i++) {
        const Shortcut *item = g_ptr_array_index(list, i);
        if (!strcmp(item->label, item->spec)) {
            g_string_append_printf(out, "%s\n", item->spec);
        } else {
            g_string_append_printf(out, "%s = %s\n", item->label, item->spec);
        }
    }
    return g_string_free(out, FALSE);
}

/**
 * Write the buttons to the file, keeping the comments on its top
 * @param path File; created, with its directory, if needed
 * @param list Buttons, in order
 * @param error Set on failure
 * @return TRUE on success
 */
gboolean shortcuts_save(const gchar *path, GPtrArray *list, GError **error) {
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    gchar *old_text = NULL;
    g_file_get_contents(path, &old_text, NULL, NULL);
    gchar *text = shortcuts_format(old_text, list);
    const gboolean saved = g_file_set_contents(path, text, -1, error);
    g_free(text);
    g_free(old_text);
    return saved;
}
