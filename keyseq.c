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
 *     C-u                 one key with Ctrl (also written Ctrl+u)
 *     C-Space c           two keys, one after the other (a tmux command)
 *     M-b                 Alt, sends Esc first (also written Alt+b)
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
    const gchar *shown; /** For keyseq_describe() */
} NamedKey;

/** Names are matched without regard to case */
static const NamedKey named_keys[] = {
    { "Enter", "\r", "回车" }, { "Return", "\r", "回车" }, { "Tab", "\t", "Tab" },
    { "Esc", "\x1b", "Esc" }, { "Escape", "\x1b", "Esc" },
    { "Space", " ", "空格" }, { "BSpace", "\x7f", "退格" }, { "Backspace", "\x7f", "退格" },
    { "Up", "\x1b[A", "↑" }, { "Down", "\x1b[B", "↓" }, { "Right", "\x1b[C", "→" }, { "Left", "\x1b[D", "←" },
    { "Home", "\x1b[H", "Home" }, { "End", "\x1b[F", "End" }, { "PgUp", "\x1b[5~", "PgUp" }, { "PgDn", "\x1b[6~", "PgDn" },
    { "Insert", "\x1b[2~", "Insert" }, { "Delete", "\x1b[3~", "Delete" }, { "Del", "\x1b[3~", "Delete" },
    { "F1", "\x1bOP", "F1" }, { "F2", "\x1bOQ", "F2" }, { "F3", "\x1bOR", "F3" }, { "F4", "\x1bOS", "F4" },
    { "F5", "\x1b[15~", "F5" }, { "F6", "\x1b[17~", "F6" }, { "F7", "\x1b[18~", "F7" }, { "F8", "\x1b[19~", "F8" },
    { "F9", "\x1b[20~", "F9" }, { "F10", "\x1b[21~", "F10" }, { "F11", "\x1b[23~", "F11" }, { "F12", "\x1b[24~", "F12" },
};

/**
 * @param name Key name
 * @return The named key, NULL if there is no such name
 */
static const NamedKey * named_key(const gchar *name) {
    for (gsize i = 0; i < G_N_ELEMENTS(named_keys); i++) {
        if (!g_ascii_strcasecmp(name, named_keys[i].name)) {
            return &named_keys[i];
        }
    }
    return NULL;
}

/**
 * @param key Key as written
 * @param short_form "C-" or "M-"
 * @param long_form "Ctrl+" or "Alt+"
 * @return The key after the modifier, or NULL if the key does not start
 *  with this modifier (or nothing follows it)
 */
static const gchar * after_modifier(const gchar *key, const gchar *short_form, const gchar *long_form) {
    const gchar *forms[] = { short_form, long_form };
    for (guint i = 0; i < G_N_ELEMENTS(forms); i++) {
        const gsize length = strlen(forms[i]);
        if (!g_ascii_strncasecmp(key, forms[i], length) && key[length]) {
            return key + length;
        }
    }
    return NULL;
}

/**
 * Append what one key without modifiers sends
 * @param out Bytes so far
 * @param shown Description so far, or NULL
 * @param key Named key or a single character (UTF-8)
 * @return FALSE if it is neither
 */
static gboolean append_plain(GByteArray *out, GString *shown, const gchar *key) {
    const NamedKey *named = named_key(key);
    if (named) {
        g_byte_array_append(out, (const guint8 *) named->bytes, (guint) strlen(named->bytes));
        if (shown) {
            g_string_append(shown, named->shown);
        }
        return TRUE;
    }
    if (*key && g_utf8_validate(key, -1, NULL) && g_utf8_strlen(key, -1) == 1) {
        g_byte_array_append(out, (const guint8 *) key, (guint) strlen(key));
        if (shown) {
            g_string_append(shown, key);
            // case matters for these, say which one it is
            if (g_ascii_islower(key[0])) {
                g_string_append(shown, "（小写）");
            } else if (g_ascii_isupper(key[0])) {
                g_string_append(shown, "（大写）");
            }
        }
        return TRUE;
    }
    return FALSE;
}

/**
 * Append what one key sends
 * @param out Bytes so far
 * @param shown Description so far, or NULL
 * @param key Key as written: "C-u", "Ctrl+u", "M-b", "Enter", "c"
 * @return FALSE if it is not understood
 */
static gboolean append_key(GByteArray *out, GString *shown, const gchar *key) {
    const gchar *base = after_modifier(key, "C-", "Ctrl+");
    if (base) {
        guint8 code;
        const gchar *name = NULL;
        if (!g_ascii_strcasecmp(base, "Space") || !strcmp(base, "@")) {
            code = 0;
            name = !strcmp(base, "@") ? "@" : "空格";
        } else if (!strcmp(base, "?")) {
            code = 0x7f;
            name = "?";
        } else if (!base[1] && (g_ascii_isalpha(base[0]) || strchr("[\\]^_", base[0]))) {
            code = (guint8) (g_ascii_toupper(base[0]) & 0x1f);
        } else {
            return FALSE;
        }
        g_byte_array_append(out, &code, 1);
        if (shown) {
            // Ctrl with a letter is the same key whatever its case
            g_string_append(shown, "Ctrl+");
            if (name) {
                g_string_append(shown, name);
            } else {
                g_string_append_c(shown, g_ascii_toupper(base[0]));
            }
        }
        return TRUE;
    }
    base = after_modifier(key, "M-", "Alt+");
    if (base) {
        // Alt: the key with Esc in front. What follows may carry Ctrl.
        const guint8 esc = 0x1b;
        g_byte_array_append(out, &esc, 1);
        if (shown) {
            g_string_append(shown, "Alt+");
        }
        return append_key(out, shown, base);
    }
    return append_plain(out, shown, key);
}

/**
 * Read a written key sequence
 * @param spec Sequence, see the top of this file
 * @param shown Set to the description in words when not NULL
 * @param error Location for an error (message in Chinese, shown to the user)
 * @return Bytes, never empty; NULL on error
 */
static GBytes * walk(const gchar *spec, GString *shown, GError **error) {
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
            GString *text = g_string_new(NULL);
            while (*p) {
                if (*p == '"') {
                    closed = TRUE;
                    p++;
                    break;
                }
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                    p++;
                }
                g_string_append_c(text, *p);
                p++;
            }
            if (!closed) {
                g_set_error(error, KEYSEQ_ERROR, 1, "引号没有闭合");
                g_string_free(text, TRUE);
                g_byte_array_unref(out);
                return NULL;
            }
            g_byte_array_append(out, (const guint8 *) text->str, (guint) text->len);
            if (shown && text->len) {
                g_string_append_printf(shown, "%s输入“%s”", shown->len ? "，然后" : "", text->str);
            }
            g_string_free(text, TRUE);
            continue;
        }
        const gchar *end = p;
        while (*end && !g_ascii_isspace(*end)) {
            end++;
        }
        gchar *key = g_strndup(p, (gsize) (end - p));
        if (shown) {
            g_string_append_printf(shown, "%s按 ", shown->len ? "，然后" : "");
        }
        const gboolean known = append_key(out, shown, key);
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

/**
 * Turn a written key sequence into the bytes to send
 * @param spec Sequence, see the top of this file
 * @param error Location for an error (message in Chinese, shown to the user)
 * @return Bytes, never empty; NULL on error
 */
GBytes * keyseq_parse(const gchar *spec, GError **error) {
    return walk(spec, NULL, error);
}

/**
 * Say in words what a written key sequence does, for whoever writes one
 * @param spec Sequence, see the top of this file
 * @param error Set as by keyseq_parse()
 * @return "按 Ctrl+空格，然后按 c（小写）"; NULL on error. Free with g_free().
 */
gchar * keyseq_describe(const gchar *spec, GError **error) {
    GString *shown = g_string_new(NULL);
    GBytes *bytes = walk(spec, shown, error);
    if (!bytes) {
        g_string_free(shown, TRUE);
        return NULL;
    }
    g_bytes_unref(bytes);
    return g_string_free(shown, FALSE);
}
