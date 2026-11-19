/* tmux.c
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
 * tmux, here or at the other end of the ssh session in the terminal:
 *
 * - whether it has the terminal at all, guessed from what runs in it;
 * - its commands, typed after its prefix (conf->tmux_prefix);
 * - whether it is scrolled back (copy mode), and its sessions, asked of
 *   tmux itself when needed, not watched: a script run over the session's
 *   own ssh connection (remote.c). The tmux client of this terminal is the
 *   one on the login the Kindle's address has there (who).
 *
 * All of it only while "tmux 集成" is on (conf->tmux_on): off, kterm
 * runs nothing on the far machine and knows only its own history.
 *
 * The paste tool learns whether the program waits for input another way,
 * from a mark tmux is told to put in the window title (kterm.c).
 */

#include <string.h>
#include <vte/vte.h>
#include "config.h"
#include "kterm.h"
#include "remote.h"
#include "tmux.h"

extern KTconf *conf;

/** tmux not answering this long counts as no answer, ms */
#define PROBE_TIMEOUT_MS 6000

/*
 * Run on the machine tmux is on. Finds this terminal's tmux client
 * (TMUX_FIND_CLIENT), then by ACTION:
 *   query, cancel: prints its pane and whether that pane is in a mode
 *     (copy mode: scrolled back), "none" without tmux clients, "unknown"
 *     if there are clients but none is this terminal's. With "cancel", a
 *     pane in a mode leaves it.
 *   bell: a line for each window of its session: bell flag, active, last
 *     output (seconds since 1970), index, host, name, the title of its
 *     pane, between colons (a title may have more of them, so last);
 *     "unknown" as above.
 *   sessions: "inside" if the terminal is in tmux already; else
 *     "sessions NOW" and a line for each session: name, windows, clients
 *     attached, last used (seconds since 1970), between colons (which
 *     tmux does not allow in names; a tab it would print as "_").
 */
static const gchar PROBE_SCRIPT[] =
    "PATH=\"$PATH:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin\"\n"
    "command -v tmux >/dev/null 2>&1 || { echo none; exit 0; }\n"
    TMUX_FIND_CLIENT
    "if [ \"$ACTION\" = sessions ]; then\n"
    "  [ -n \"$best\" ] && { echo inside; exit 0; }\n"
    "  echo \"sessions $(date +%s)\"\n"
    "  tmux list-sessions -F '#{session_name}:#{session_windows}:#{session_attached}:#{session_activity}' 2>/dev/null\n"
    "  exit 0\n"
    "fi\n"
    "[ -n \"$clients\" ] || { echo none; exit 0; }\n"
    "[ -n \"$best\" ] || { echo unknown; exit 0; }\n"
    "set -- $best\n"
    "if [ \"$ACTION\" = bell ]; then\n"
    "  tmux list-windows -t \"$3\" -F '#{window_bell_flag}:#{window_active}:#{window_activity}:#{window_index}:#{host}:#{window_name}:#{pane_title}'\n"
    "  exit 0\n"
    "fi\n"
    "if [ \"$ACTION\" = cancel ] && [ \"$4\" = 1 ]; then tmux send-keys -t \"$3\" -X cancel && echo \"$3 0\"; exit 0; fi\n"
    "echo \"$3 $4\"\n";

/**
 * @return TRUE while "tmux 集成" is on
 */
gboolean tmux_enabled(void) {
    return conf->tmux_on != 0;
}

/**
 * Switch "tmux 集成" on or off
 * @param on Non-zero for on
 */
void tmux_set_enabled(guint on) {
    conf->tmux_on = on ? 1 : 0;
}

/** Makes the script's output (NULL if tmux could not be asked) an answer
 *  for done, the caller's callback */
typedef void (*ScriptParse)(gchar *output, GCallback done, gpointer data);

/** A question to tmux under way */
typedef struct {
    ScriptParse parse;
    GCallback done;
    gpointer data;
} Ask;

static void script_done(GBytes *output, gpointer data) {
    Ask *ask = data;
    gchar *text = output ? g_strndup(g_bytes_get_data(output, NULL), g_bytes_get_size(output)) : NULL;
    D printf("tmux: says \"%s\"\n", text ? text : "(nothing)");
    ask->parse(text, ask->done, ask->data);
    g_free(text);
    g_free(ask);
}

/**
 * Run PROBE_SCRIPT where tmux is: over the ssh session that has the
 * terminal, or here if tmux runs here
 * @param action ACTION of the script
 * @param ssh_only Not for a tmux here
 * @param parse Told the output, later; not called if FALSE is returned
 * @param done,data The caller's callback and its data, for parse
 * @return FALSE if there is no tmux to ask: switched off, or neither ssh
 *         nor tmux has the terminal
 */
static gboolean run_script(const gchar *action, gboolean ssh_only, ScriptParse parse, GCallback done,
                           gpointer data) {
    if (!tmux_enabled()) {
        return FALSE;
    }
    gboolean here = FALSE;
    if (!ssh_only && !remote_is_ssh()) {
        gchar *name = kterm_process_name(kterm_terminal_owner());
        here = name && g_str_has_prefix(name, "tmux");
        g_free(name);
        if (!here) {
            return FALSE;
        }
    }
    gchar *var = g_strconcat("ACTION=", action, NULL);
    const gchar *vars[] = { var, NULL };
    Ask *ask = g_new0(Ask, 1);
    ask->parse = parse;
    ask->done = done;
    ask->data = data;
    const gboolean asked = remote_run(PROBE_SCRIPT, vars, here, PROBE_TIMEOUT_MS, script_done, ask);
    g_free(var);
    if (!asked) {
        g_free(ask);
    }
    return asked;
}

static void where_done(gchar *output, GCallback done, gpointer data) {
    TmuxWhere where = TMUX_UNKNOWN;
    if (output) {
        g_strstrip(output);
        if (!strcmp(output, "none")) {
            // no tmux there: the history is kterm's own
            where = kterm_terminal_scrolled() ? TMUX_SCROLLED : TMUX_AT_INPUT;
        } else if (g_str_has_suffix(output, " 0")) {
            where = TMUX_AT_INPUT;
        } else if (g_str_has_suffix(output, " 1")) {
            where = TMUX_SCROLLED;
        }
    }
    ((TmuxAnswer) done)(where, data);
}

/**
 * Find out whether the terminal is scrolled back
 * @param cancel TRUE to leave tmux's copy mode as well, if it is in it;
 *  kterm's own history is shown to its end
 * @param done Called with the answer
 * @param data For done
 */
void tmux_ask_where(gboolean cancel, TmuxAnswer done, gpointer data) {
    if (!run_script(cancel ? "cancel" : "query", FALSE, where_done, G_CALLBACK(done), data)) {
        // no tmux to ask: kterm's own history is all there is
        if (cancel) {
            kterm_terminal_to_bottom();
        }
        done(cancel || !kterm_terminal_scrolled() ? TMUX_AT_INPUT : TMUX_SCROLLED, data);
    }
}

static gint session_order(gconstpointer a, gconstpointer b) {
    const TmuxSession *x = *(TmuxSession * const *) a;
    const TmuxSession *y = *(TmuxSession * const *) b;
    return x->idle < y->idle ? -1 : x->idle > y->idle;
}

static void session_free(gpointer data) {
    TmuxSession *session = data;
    g_free(session->name);
    g_free(session);
}

static void sessions_done(gchar *output, GCallback done, gpointer data) {
    TmuxAsked asked = TMUX_NOT_ASKED;
    GPtrArray *sessions = g_ptr_array_new_with_free_func(session_free);
    gchar **lines = output ? g_strsplit(output, "\n", -1) : NULL;
    gint64 now = 0;
    if (lines && !strcmp(g_strstrip(lines[0]), "inside")) {
        asked = TMUX_INSIDE;
    } else if (lines && !strcmp(lines[0], "none")) {
        asked = TMUX_LISTED;
    } else if (lines && sscanf(lines[0], "sessions %" G_GINT64_FORMAT, &now) == 1) {
        asked = TMUX_LISTED;
        for (guint i = 1; lines[i]; i++) {
            gchar **fields = g_strsplit(lines[i], ":", 4);
            if (g_strv_length(fields) == 4 && *fields[0]) {
                TmuxSession *session = g_new0(TmuxSession, 1);
                session->name = g_strdup(fields[0]);
                session->windows = (guint) g_ascii_strtoull(fields[1], NULL, 10);
                session->attached = (guint) g_ascii_strtoull(fields[2], NULL, 10);
                session->idle = MAX(0, now - g_ascii_strtoll(fields[3], NULL, 10));
                g_ptr_array_add(sessions, session);
            }
            g_strfreev(fields);
        }
        // the one used last first
        g_ptr_array_sort(sessions, session_order);
    }
    g_strfreev(lines);
    ((TmuxSessionsDone) done)(asked, sessions, data);
    g_ptr_array_unref(sessions);
}

/**
 * Ask the far machine of the ssh session that has the terminal for its
 * tmux sessions
 * @param done Told the answer, later; not called if FALSE is returned
 * @param data For done
 * @return FALSE if there is nothing to ask: switched off, no ssh session
 */
gboolean tmux_list_sessions(TmuxSessionsDone done, gpointer data) {
    return run_script("sessions", TRUE, sessions_done, G_CALLBACK(done), data);
}

/** Windows flagged for a bell at the last bell, " index:" each: one
 *  flagged since is the one that rang */
static gchar *flagged_before = NULL;

static void bell_done(gchar *output, GCallback done, gpointer data) {
    gchar **lines = output ? g_strsplit(output, "\n", -1) : NULL;
    // A window that rang is flagged until it is shown; one shown is not
    // flagged. The window flagged since the last bell rang (of several,
    // the one written to last); with none, the one shown did.
    gchar **rang = NULL;
    gchar **shown = NULL;
    gint64 latest = -1;
    GString *flagged = g_string_new(NULL);
    for (guint i = 0; lines && lines[i]; i++) {
        gchar **fields = g_strsplit(lines[i], ":", 7);
        // not every line is a window's: the empty one after the last
        if (g_strv_length(fields) != 7) {
            g_strfreev(fields);
            continue;
        }
        gchar *key = g_strconcat(" ", fields[3], ":", NULL);
        const gint64 written = g_ascii_strtoll(fields[2], NULL, 10);
        const gboolean is_flagged = !strcmp(fields[0], "1");
        const gboolean is_new = is_flagged && !(flagged_before && strstr(flagged_before, key));
        if (is_flagged) {
            g_string_append(flagged, key);
        }
        g_free(key);
        if (is_new && written > latest) {
            g_strfreev(rang);
            rang = fields;
            latest = written;
        } else if (!shown && !strcmp(fields[1], "1")) {
            shown = fields;
        } else {
            g_strfreev(fields);
        }
    }
    g_free(flagged_before);
    flagged_before = g_string_free(flagged, FALSE);
    if (!rang) {
        rang = shown;
        shown = NULL;
    }
    gint index = -1;
    const gchar *label = NULL;
    if (rang) {
        index = (gint) g_ascii_strtoll(rang[3], NULL, 10);
        // a pane without a title of its own has the host's name
        const gchar *host = rang[4];
        const gchar *title = rang[6];
        const gsize short_host = strcspn(host, ".");
        const gboolean own = *title && strcmp(title, host)
                             && !(strlen(title) == short_host && !strncmp(title, host, short_host));
        label = own ? title : rang[5];
    }
    ((TmuxBellDone) done)(index, label, data);
    g_strfreev(rang);
    g_strfreev(shown);
    g_strfreev(lines);
}

/**
 * Ask tmux, here or over the ssh session, which of its windows rang the
 * bell the terminal got
 * @param done Told the window, later; not called if FALSE is returned
 * @param data For done
 * @return FALSE if there is no tmux to ask
 */
gboolean tmux_ask_bell(TmuxBellDone done, gpointer data) {
    return run_script("bell", FALSE, bell_done, G_CALLBACK(done), data);
}

/**
 * Type a tmux command key: the prefix from the configuration (tmux_prefix,
 * "C-b" unless set), then the key.
 * @param key Key after the prefix, e.g. "n"
 * @return FALSE if the configured prefix is not understood
 */
gboolean tmux_send_command(const gchar *key) {
    const gchar *prefix = conf->tmux_prefix;
    gchar code;
    if (g_ascii_strncasecmp(prefix, "C-", 2) != 0) {
        return FALSE;
    }
    if (!g_ascii_strcasecmp(prefix + 2, "Space")) {
        code = '\0';
    } else if (prefix[2] && !prefix[3] && g_ascii_isalpha(prefix[2])) {
        code = (gchar) (g_ascii_toupper(prefix[2]) & 0x1f);
    } else {
        return FALSE;
    }
    D printf("tmux: prefix %s + %s\n", prefix, key);
    VteTerminal *terminal = VTE_TERMINAL(kterm_get_terminal());
    vte_terminal_feed_child(terminal, &code, 1);
    vte_terminal_feed_child(terminal, key, -1);
    return TRUE;
}

/**
 * @return TRUE if tmux has the terminal, here or, as far as can be told,
 *         at the other end of an ssh session (a full screen there)
 */
gboolean tmux_may_have_terminal(void) {
    gchar *name = kterm_process_name(kterm_terminal_owner());
    const gboolean tmux = name && (g_str_has_prefix(name, "tmux")
                                   || (g_str_has_prefix(name, "ssh") && !kterm_terminal_has_history()));
    g_free(name);
    return tmux;
}
