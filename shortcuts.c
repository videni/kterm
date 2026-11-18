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

#include <stdlib.h>
#include <string.h>
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
 * The line of the file for a button
 * @param label Text on the button
 * @param spec Keys as written
 * @return Line without newline, free with g_free()
 */
static gchar * format_line(const gchar *label, const gchar *spec) {
    if (!strcmp(label, spec)) {
        return g_strdup(spec);
    }
    return g_strdup_printf("%s = %s", label, spec);
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
    if (!*label) {
        g_free(label);
        label = g_strdup(keys_text);
    }
    gchar *line = format_line(label, keys_text);
    Shortcut *item = NULL;
    if (strcmp(label, keys_text) && strchr(label, '=')) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "名字里不能有 = 号");
    } else if (*line == '#') {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    strcmp(label, keys_text) ? "名字不能以 # 开头" : "以 # 开头的按键请起个名字");
    } else {
        // what is saved must come back the same from the file
        item = parse_line(line, error);
        if (item && (strcmp(item->label, label) || strcmp(item->spec, keys_text))) {
            shortcut_free(item);
            item = NULL;
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                        "这样写存进文件后读回来会变样：名字里不要有 = 号或单个引号，按键以 = 开头或结尾时请起个名字");
        }
    }
    g_free(line);
    g_free(label);
    g_free(keys_text);
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
    gchar *found = g_string_free(report, report->len == 0);
    if (problems) {
        *problems = found;
    } else {
        g_free(found);
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
#define SHORTCUTS_NOTE "# kterm 快捷栏：一行一个按钮，写法见面板里的“设置 → 快捷键”页\n\n"

/**
 * The text of the file for a list of buttons
 * @param old_text The file as it was, or NULL. The comments on its top
 *  stay there, the buttons follow, and the other lines that were no
 *  buttons (comments further down, lines that could not be read, buttons
 *  past the limit) are kept after them: nothing written by hand is lost.
 * @param list Buttons, in order
 * @return New contents, free with g_free()
 */
gchar * shortcuts_format(const gchar *old_text, GPtrArray *list) {
    GString *head = g_string_new(old_text ? NULL : SHORTCUTS_NOTE);
    GString *kept = g_string_new(NULL);
    gchar **lines = g_strsplit(old_text ? old_text : "", "\n", -1);
    gboolean in_head = TRUE;
    guint buttons = 0;
    for (guint i = 0; lines[i]; i++) {
        if (!lines[i + 1] && !*lines[i]) {
            break; // after the last newline
        }
        gchar *line = g_strstrip(g_strdup(lines[i]));
        const gboolean note = !*line || *line == '#';
        if (in_head && note) {
            g_string_append_printf(head, "%s\n", lines[i]);
        } else if (*line) {
            in_head = FALSE;
            Shortcut *item = note ? NULL : parse_line(line, NULL);
            if (item && buttons < SHORTCUTS_MAX) {
                buttons++; // one of the list's, written from the list
            } else {
                g_string_append_printf(kept, "%s\n", lines[i]);
            }
            shortcut_free(item);
        }
        g_free(line);
    }
    g_strfreev(lines);
    for (guint i = 0; i < list->len; i++) {
        const Shortcut *item = g_ptr_array_index(list, i);
        gchar *line = format_line(item->label, item->spec);
        g_string_append_printf(head, "%s\n", line);
        g_free(line);
    }
    if (kept->len) {
        g_string_append_printf(head, "\n%s", kept->str);
    }
    g_string_free(kept, TRUE);
    return g_string_free(head, FALSE);
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
    GError *read_error = NULL;
    if (!g_file_get_contents(path, &old_text, NULL, &read_error)
        && !g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        // never replace a file that is there but could not be read
        g_propagate_error(error, read_error);
        return FALSE;
    }
    g_clear_error(&read_error);
    gchar *text = shortcuts_format(old_text, list);
    // through a symbolic link: the file it points to is the one replaced
    char *real = realpath(path, NULL);
    const gboolean saved = g_file_set_contents(real ? real : path, text, -1, error);
    free(real);
    g_free(text);
    g_free(old_text);
    return saved;
}
