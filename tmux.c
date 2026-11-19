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
 * - whether it is scrolled back (copy mode), asked of tmux itself when
 *   needed, not watched: the same ssh connection runs one more command
 *   (ControlMaster, about 0.1 s on the Kindle), as RootShell does. The
 *   tmux client of this terminal is the one on the login the Kindle's
 *   address has there (who). Needs a key login: ssh cannot ask for a
 *   password in the background.
 *
 * The paste tool learns whether the program waits for input another way,
 * from a mark tmux is told to put in the window title (kterm.c).
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vte/vte.h>
#include "config.h"
#include "kterm.h"
#include "tmux.h"

extern KTconf *conf;

/** tmux not answering this long counts as no answer, ms */
#define PROBE_TIMEOUT_MS 6000
/** The connection to the far machine is kept this long after the last
 *  question, s */
#define CONTROL_PERSIST "600"

/*
 * Run on the machine tmux is on: finds this terminal's tmux client and
 * prints its pane and whether that pane is in a mode (copy mode: scrolled
 * back), "none" without tmux clients, "unknown" if there are clients but
 * none is this terminal's. With "cancel", a pane in a mode leaves it.
 */
static const gchar PROBE_SCRIPT[] =
    "PATH=\"$PATH:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin\"\n"
    "command -v tmux >/dev/null 2>&1 || { echo none; exit 0; }\n"
    "clients=$(tmux list-clients -F '#{client_activity} #{client_tty} #{pane_id} #{pane_in_mode}' 2>/dev/null)\n"
    "[ -n \"$clients\" ] || { echo none; exit 0; }\n"
    "if [ -n \"$KTERM_TTY\" ]; then ttys=$KTERM_TTY\n"
    "else ip=${SSH_CONNECTION%% *}; ttys=$(who | awk -v h=\"($ip)\" '$NF == h { print \"/dev/\" $2 }'); fi\n"
    "best=$(for t in $ttys; do printf '%s\\n' \"$clients\" | awk -v t=\"$t\" '$2 == t'; done | sort -rn | head -n 1)\n"
    "[ -n \"$best\" ] || { echo unknown; exit 0; }\n"
    "set -- $best\n"
    "if [ \"$ACTION\" = cancel ] && [ \"$4\" = 1 ]; then tmux send-keys -t \"$3\" -X cancel && echo \"$3 0\"; exit 0; fi\n"
    "echo \"$3 $4\"\n";

/**
 * The ssh command that asks the far machine, built from the client that
 * has the terminal: its destination and the options that say how to get
 * there (port, user, key, jump host, config); not its forwardings, its
 * remote command or its terminal.
 * @param pid ssh client
 * @return Arguments, NULL if they cannot be read
 */
static GPtrArray * probe_command(pid_t pid, const gchar *action) {
    gchar *path = g_strdup_printf("/proc/%d/cmdline", (int) pid);
    gchar *raw = NULL;
    gsize size = 0;
    const gboolean read = g_file_get_contents(path, &raw, &size, NULL);
    g_free(path);
    if (!read || !size) {
        g_free(raw);
        return NULL;
    }
    GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(args, g_strdup("ssh"));
    // first: ssh takes the first value it is given for an option
    static const gchar *own[] = {
        "BatchMode=yes", "RemoteCommand=none", "RequestTTY=no", "ConnectTimeout=5",
        "ControlMaster=auto", "ControlPath=/tmp/kterm-ssh-%C", "ControlPersist=" CONTROL_PERSIST,
        "ClearAllForwardings=yes", "LogLevel=ERROR",
    };
    for (guint i = 0; i < G_N_ELEMENTS(own); i++) {
        g_ptr_array_add(args, g_strdup("-o"));
        g_ptr_array_add(args, g_strdup(own[i]));
    }
    const gchar *destination = NULL;
    const gchar *end = raw + size;
    const gchar *arg = raw + strlen(raw) + 1;
    while (arg < end && !destination) {
        const gchar *next = arg + strlen(arg) + 1;
        if (arg[0] != '-' || !arg[1]) {
            destination = arg;
            break;
        }
        // options that take a value, kept or not
        static const gchar *with_value = "BbcDEeFIiJLlmOopQRSWw";
        static const gchar *kept = "cFiJlmop";
        for (const gchar *c = arg + 1; *c; c++) {
            if (strchr(with_value, *c)) {
                const gchar *value = c[1] ? c + 1 : (next < end ? next : NULL);
                if (strchr(kept, *c) && value) {
                    g_ptr_array_add(args, g_strdup_printf("-%c", *c));
                    g_ptr_array_add(args, g_strdup(value));
                }
                if (!c[1] && next < end) {
                    next += strlen(next) + 1;
                }
                break;
            }
            if (strchr("46C", *c)) {
                g_ptr_array_add(args, g_strdup_printf("-%c", *c));
            }
        }
        arg = next;
    }
    if (!destination) {
        g_ptr_array_unref(args);
        g_free(raw);
        return NULL;
    }
    g_ptr_array_add(args, g_strdup(destination));
    g_ptr_array_add(args, g_strdup_printf("ACTION=%s sh -s", action));
    g_free(raw);
    return args;
}

/** A question to tmux under way */
typedef struct {
    TmuxAnswer done;
    gpointer data;
    guint timeout;
    GSubprocess *process;
} Probe;

static gboolean probe_timeout(gpointer data) {
    Probe *probe = data;
    probe->timeout = 0;
    g_subprocess_force_exit(probe->process);
    return G_SOURCE_REMOVE;
}

static void probe_finished(GObject *source, GAsyncResult *result, gpointer data) {
    Probe *probe = data;
    if (probe->timeout) {
        g_source_remove(probe->timeout);
    }
    gchar *output = NULL;
    gchar *errors = NULL;
    TmuxWhere where = TMUX_UNKNOWN;
    // only the output tells: kterm leaves its children to the kernel
    // (SIGCHLD ignored), so their exit status is never known
    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, &errors, NULL) && output) {
        g_strstrip(output);
        D printf("voicebar: tmux says \"%s\"\n", output);
        if (!strcmp(output, "none")) {
            // no tmux there: the history is kterm's own
            where = kterm_terminal_scrolled() ? TMUX_SCROLLED : TMUX_AT_INPUT;
        } else if (g_str_has_suffix(output, " 0")) {
            where = TMUX_AT_INPUT;
        } else if (g_str_has_suffix(output, " 1")) {
            where = TMUX_SCROLLED;
        }
    } else {
        D printf("voicebar: tmux not asked: %s\n", errors ? errors : "");
    }
    g_free(output);
    g_free(errors);
    g_object_unref(probe->process);
    probe->done(where, probe->data);
    g_free(probe);
}

/**
 * Find out whether the terminal is scrolled back
 * @param cancel TRUE to leave tmux's copy mode as well, if it is in it
 * @param done Called with the answer
 * @param data For done
 */
void tmux_ask_where(gboolean cancel, TmuxAnswer done, gpointer data) {
    // what has the terminal: the shell, or what was started from it, or
    // the program kterm runs in place of a shell
    VtePty *pty = vte_terminal_get_pty(VTE_TERMINAL(kterm_get_terminal()));
    const pid_t pid = kterm_terminal_owner();
    gchar *name = kterm_process_name(pid);
    const gchar *action = cancel ? "cancel" : "query";
    GPtrArray *args = NULL;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE
                                                              | G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                                              | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    if (name && !strcmp(name, "ssh")) {
        args = probe_command(pid, action);
    } else if (name && g_str_has_prefix(name, "tmux")) {
        // tmux here: its client is on this terminal's tty
        const char *tty = pty ? ptsname(vte_pty_get_fd(pty)) : NULL;
        if (tty) {
            g_subprocess_launcher_setenv(launcher, "KTERM_TTY", tty, TRUE);
            g_subprocess_launcher_setenv(launcher, "ACTION", action, TRUE);
            args = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(args, g_strdup("sh"));
            g_ptr_array_add(args, g_strdup("-s"));
        }
    }
    g_free(name);
    if (!args) {
        g_object_unref(launcher);
        if (cancel) {
            kterm_terminal_to_bottom();
        }
        done(cancel || !kterm_terminal_scrolled() ? TMUX_AT_INPUT : TMUX_SCROLLED, data);
        return;
    }
    g_ptr_array_add(args, NULL);
    GError *error = NULL;
    Probe *probe = g_new0(Probe, 1);
    probe->done = done;
    probe->data = data;
    probe->process = g_subprocess_launcher_spawnv(launcher, (const gchar * const *) args->pdata, &error);
    g_object_unref(launcher);
    g_ptr_array_unref(args);
    if (!probe->process) {
        D printf("voicebar: %s\n", error->message);
        g_error_free(error);
        g_free(probe);
        done(TMUX_UNKNOWN, data);
        return;
    }
    probe->timeout = g_timeout_add(PROBE_TIMEOUT_MS, probe_timeout, probe);
    g_subprocess_communicate_utf8_async(probe->process, PROBE_SCRIPT, NULL, probe_finished, probe);
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
