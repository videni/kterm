/* keyseq.c
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
 * Key sequences written the way people write them, turned into the bytes a
 * terminal sends for them:
 *
 *     C-u                 one key with Ctrl
 *     C-Space c           two keys, one after the other (a tmux command)
 *     M-b                 Alt (sends Esc first)
 *     Esc : w q Enter     named keys and plain characters
 *     "git pull" Enter    text in quotes is typed as it is
 *
 * Keys are separated by spaces. A word that is neither a named key nor a
 * single character is an error rather than typed: a misspelled key name
 * should not end up as text on someone's command line.
 */

#include <string.h>
#include "keyseq.h"

#define KEYSEQ_ERROR g_quark_from_static_string("kterm-keyseq")

typedef struct {
    const gchar *name;
    const gchar *bytes;
} NamedKey;

/** Names are matched without regard to case */
static const NamedKey named_keys[] = {
    { "Enter", "\r" }, { "Return", "\r" }, { "Tab", "\t" }, { "Esc", "\x1b" }, { "Escape", "\x1b" },
    { "Space", " " }, { "BSpace", "\x7f" }, { "Backspace", "\x7f" },
    { "Up", "\x1b[A" }, { "Down", "\x1b[B" }, { "Right", "\x1b[C" }, { "Left", "\x1b[D" },
    { "Home", "\x1b[H" }, { "End", "\x1b[F" }, { "PgUp", "\x1b[5~" }, { "PgDn", "\x1b[6~" },
    { "Insert", "\x1b[2~" }, { "Delete", "\x1b[3~" }, { "Del", "\x1b[3~" },
    { "F1", "\x1bOP" }, { "F2", "\x1bOQ" }, { "F3", "\x1bOR" }, { "F4", "\x1bOS" },
    { "F5", "\x1b[15~" }, { "F6", "\x1b[17~" }, { "F7", "\x1b[18~" }, { "F8", "\x1b[19~" },
    { "F9", "\x1b[20~" }, { "F10", "\x1b[21~" }, { "F11", "\x1b[23~" }, { "F12", "\x1b[24~" },
};

/**
 * @param name Key name
 * @return Bytes of the named key, NULL if there is no such name
 */
static const gchar * named_key(const gchar *name) {
    for (gsize i = 0; i < G_N_ELEMENTS(named_keys); i++) {
        if (!g_ascii_strcasecmp(name, named_keys[i].name)) {
            return named_keys[i].bytes;
        }
    }
    return NULL;
}

/**
 * Append what one key without modifiers sends
 * @param out Bytes so far
 * @param key Named key or a single character (UTF-8)
 * @return FALSE if it is neither
 */
static gboolean append_plain(GByteArray *out, const gchar *key) {
    const gchar *bytes = named_key(key);
    if (bytes) {
        g_byte_array_append(out, (const guint8 *) bytes, (guint) strlen(bytes));
        return TRUE;
    }
    if (*key && g_utf8_validate(key, -1, NULL) && g_utf8_strlen(key, -1) == 1) {
        g_byte_array_append(out, (const guint8 *) key, (guint) strlen(key));
        return TRUE;
    }
    return FALSE;
}

/**
 * Append what one key sends
 * @param out Bytes so far
 * @param key Key as written: "C-u", "M-b", "Enter", "c"
 * @return FALSE if it is not understood
 */
static gboolean append_key(GByteArray *out, const gchar *key) {
    if ((key[0] == 'C' || key[0] == 'c') && key[1] == '-' && key[2]) {
        const gchar *base = key + 2;
        guint8 code;
        if (!g_ascii_strcasecmp(base, "Space") || !strcmp(base, "@")) {
            code = 0;
        } else if (!strcmp(base, "?")) {
            code = 0x7f;
        } else if (!base[1] && (g_ascii_isalpha(base[0]) || strchr("[\\]^_", base[0]))) {
            code = (guint8) (g_ascii_toupper(base[0]) & 0x1f);
        } else {
            return FALSE;
        }
        g_byte_array_append(out, &code, 1);
        return TRUE;
    }
    if ((key[0] == 'M' || key[0] == 'm') && key[1] == '-' && key[2]) {
        // Alt: the key with Esc in front. What follows may carry Ctrl.
        const guint8 esc = 0x1b;
        g_byte_array_append(out, &esc, 1);
        return append_key(out, key + 2);
    }
    return append_plain(out, key);
}

/**
 * Turn a written key sequence into the bytes to send
 * @param spec Sequence, see the top of this file
 * @param error Location for an error (message in Chinese, shown to the user)
 * @return Bytes, never empty; NULL on error
 */
GBytes * keyseq_parse(const gchar *spec, GError **error) {
    GByteArray *out = g_byte_array_new();
    const gchar *p = spec ? spec : "";
    while (*p) {
        if (g_ascii_isspace(*p)) {
            p++;
            continue;
        }
        if (*p == '"') {
            // text: up to the closing quote; \" and \\ stand for themselves
            p++;
            gboolean closed = FALSE;
            while (*p) {
                if (*p == '"') {
                    closed = TRUE;
                    p++;
                    break;
                }
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                    p++;
                }
                g_byte_array_append(out, (const guint8 *) p, 1);
                p++;
            }
            if (!closed) {
                g_set_error(error, KEYSEQ_ERROR, 1, "引号没有闭合");
                g_byte_array_unref(out);
                return NULL;
            }
            continue;
        }
        const gchar *end = p;
        while (*end && !g_ascii_isspace(*end)) {
            end++;
        }
        gchar *key = g_strndup(p, (gsize) (end - p));
        const gboolean known = append_key(out, key);
        if (!known) {
            g_set_error(error, KEYSEQ_ERROR, 2, "不认识的按键“%s”（文字请加引号）", key);
            g_free(key);
            g_byte_array_unref(out);
            return NULL;
        }
        g_free(key);
        p = end;
    }
    if (out->len == 0) {
        g_set_error(error, KEYSEQ_ERROR, 3, "按键为空");
        g_byte_array_unref(out);
        return NULL;
    }
    return g_byte_array_free_to_bytes(out);
}
