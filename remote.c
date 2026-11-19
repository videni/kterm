/* remote.c
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
 * A shell script run where the terminal's program is: on the far machine
 * of the ssh session that has the terminal, or here. Asking tmux (tmux.c)
 * and fetching an image (imageview.c) go through it.
 *
 * Over ssh the session's own connection runs it (ControlMaster, about
 * 0.1 s on the Kindle), as RootShell does. A connection opened from the
 * panel is itself the shared one (connpage.c), so a password login works
 * too; one typed by hand needs a key login, ssh cannot ask for a password
 * in the background.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vte/vte.h>
#include "config.h"
#include "kterm.h"
#include "remote.h"

/**
 * @return TRUE if the ssh client has the terminal
 */
gboolean remote_is_ssh(void) {
    return kterm_process_is(kterm_terminal_owner(), "ssh");
}

/**
 * The ssh command that runs the script on the far machine, built from the
 * client that has the terminal: its destination and the options that say
 * how to get there (port, user, key, jump host, config); not its
 * forwardings, its remote command or its terminal.
 * @param pid ssh client
 * @param command What the far machine runs
 * @return Arguments, NULL if they cannot be read
 */
static GPtrArray * ssh_command(pid_t pid, const gchar *command) {
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
        "ControlMaster=auto", "ControlPath=" REMOTE_CONTROL_PATH, "ControlPersist=" REMOTE_CONTROL_PERSIST,
        "ClearAllForwardings=yes", "LogLevel=ERROR",
    };
    for (guint i = 0; i < G_N_ELEMENTS(own); i++) {
        g_ptr_array_add(args, g_strdup("-o"));
        g_ptr_array_add(args, g_strdup(own[i]));
    }
    const gchar *destination = NULL;
    const gchar *end = raw + size;
    // the arguments start after the program's name: the first word, or
    // the words naming ssh, with an interpreter before them (qemu gives
    // its own name, the program's path, then the name it was run by)
    const gchar *arg = raw + strlen(raw) + 1;
    gboolean named = FALSE;
    for (const gchar *word = raw; word < end; word += strlen(word) + 1) {
        gchar *base = g_path_get_basename(word);
        const gboolean is_ssh = !strcmp(base, "ssh");
        g_free(base);
        if (is_ssh) {
            arg = word + strlen(word) + 1;
            named = TRUE;
        } else if (named) {
            break;
        }
    }
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
    g_ptr_array_add(args, g_strdup(command));
    g_free(raw);
    return args;
}

/** A script under way */
typedef struct {
    RemoteDone done;
    gpointer data;
    GSubprocess *process;
    guint timeout;
    /** Stopped for taking too long: what it printed is not all */
    gboolean late;
} Run;

static gboolean run_timeout(gpointer data) {
    Run *run = data;
    run->timeout = 0;
    run->late = TRUE;
    g_subprocess_force_exit(run->process);
    return G_SOURCE_REMOVE;
}

static void run_finished(GObject *source, GAsyncResult *result, gpointer data) {
    Run *run = data;
    if (run->timeout) {
        g_source_remove(run->timeout);
    }
    GBytes *output = NULL;
    GBytes *errors = NULL;
    // the output tells, not the exit status: ssh's own failures and the
    // script's answers would have to be told apart
    if (!g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &output, &errors, NULL)
        || run->late || !output || !g_bytes_get_size(output)) {
        D printf("remote: no output%s: %.*s\n", run->late ? " in time" : "",
                 errors ? (int) g_bytes_get_size(errors) : 0,
                 errors ? (const gchar *) g_bytes_get_data(errors, NULL) : "");
        g_clear_pointer(&output, g_bytes_unref);
    }
    if (errors) {
        g_bytes_unref(errors);
    }
    g_object_unref(run->process);
    run->done(output, run->data);
    if (output) {
        g_bytes_unref(output);
    }
    g_free(run);
}

/**
 * Run a script where the terminal's program is.
 *
 * Here it is told the terminal's tty as KTERM_TTY and the directory of
 * the terminal's program as KTERM_DIR; over ssh it starts in the home
 * directory.
 * @param script Shell script
 * @param vars "NAME=value" for the script, NULL-terminated; NULL for none
 * @param here Run it here too if no ssh session has the terminal
 * @param timeout_ms Stopped if it takes longer
 * @param done Told the output, later; not called if FALSE is returned
 * @param data For done
 * @return FALSE if there is nowhere to run it
 */
gboolean remote_run(const gchar *script, const gchar * const *vars, gboolean here, guint timeout_ms,
                    RemoteDone done, gpointer data) {
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE
                                                              | G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                                              | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    GPtrArray *args = NULL;
    if (remote_is_ssh()) {
        // NAME='value' ... sh -s
        GString *command = g_string_new(NULL);
        for (const gchar * const *var = vars; var && *var; var++) {
            const gchar *equals = strchr(*var, '=');
            gchar *value = g_shell_quote(equals + 1);
            g_string_append_printf(command, "%.*s%s ", (int) (equals + 1 - *var), *var, value);
            g_free(value);
        }
        g_string_append(command, "sh -s");
        args = ssh_command(kterm_terminal_owner(), command->str);
        g_string_free(command, TRUE);
    } else if (here) {
        VtePty *pty = vte_terminal_get_pty(VTE_TERMINAL(kterm_get_terminal()));
        const char *tty = pty ? ptsname(vte_pty_get_fd(pty)) : NULL;
        if (tty) {
            g_subprocess_launcher_setenv(launcher, "KTERM_TTY", tty, TRUE);
        }
        gchar *cwd = g_strdup_printf("/proc/%d/cwd", (int) kterm_terminal_owner());
        gchar *dir = g_file_read_link(cwd, NULL);
        if (dir) {
            g_subprocess_launcher_setenv(launcher, "KTERM_DIR", dir, TRUE);
        }
        g_free(dir);
        g_free(cwd);
        for (const gchar * const *var = vars; var && *var; var++) {
            gchar **pair = g_strsplit(*var, "=", 2);
            g_subprocess_launcher_setenv(launcher, pair[0], pair[1], TRUE);
            g_strfreev(pair);
        }
        args = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(args, g_strdup("sh"));
        g_ptr_array_add(args, g_strdup("-s"));
    }
    if (!args) {
        g_object_unref(launcher);
        return FALSE;
    }
    g_ptr_array_add(args, NULL);
    GError *error = NULL;
    GSubprocess *process = g_subprocess_launcher_spawnv(launcher, (const gchar * const *) args->pdata, &error);
    g_object_unref(launcher);
    g_ptr_array_unref(args);
    if (!process) {
        D printf("remote: %s\n", error->message);
        g_error_free(error);
        done(NULL, data);
        return TRUE;
    }
    Run *run = g_new0(Run, 1);
    run->done = done;
    run->data = data;
    run->process = process;
    run->timeout = g_timeout_add(timeout_ms, run_timeout, run);
    GBytes *input = g_bytes_new_static(script, strlen(script));
    g_subprocess_communicate_async(process, input, NULL, run_finished, run);
    g_bytes_unref(input);
    return TRUE;
}
