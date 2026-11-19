/* tmux.h
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

#ifndef tmux_h
#define tmux_h

#include <gtk/gtk.h>

/**
 * Shell lines finding this terminal's tmux client, tmux being there: sets
 * $clients (one line for each: activity, tty, pane, in a mode) and $best,
 * the line of this terminal's client, empty if none is. Its tty is
 * KTERM_TTY here (remote.c), over ssh one the Kindle's address logged in on.
 */
#define TMUX_FIND_CLIENT \
    "clients=$(tmux list-clients -F '#{client_activity} #{client_tty} #{pane_id} #{pane_in_mode}' 2>/dev/null)\n" \
    "best=\n" \
    "if [ -n \"$clients\" ]; then\n" \
    "  if [ -n \"$KTERM_TTY\" ]; then ttys=$KTERM_TTY\n" \
    "  else ip=${SSH_CONNECTION%% *}; ttys=$(who | awk -v h=\"($ip)\" '$NF == h { print \"/dev/\" $2 }'); fi\n" \
    "  best=$(for t in $ttys; do printf '%s\\n' \"$clients\" | awk -v t=\"$t\" '$2 == t'; done | sort -rn | head -n 1)\n" \
    "fi\n"

/** Where the terminal is: at its input line or scrolled back */
typedef enum {
    /** showing its input line */
    TMUX_AT_INPUT,
    /** scrolled back */
    TMUX_SCROLLED,
    /** not known: tmux could not be asked */
    TMUX_UNKNOWN,
} TmuxWhere;

typedef void (*TmuxAnswer)(TmuxWhere where, gpointer data);

/** A tmux session on the far machine */
typedef struct {
    gchar *name;
    guint windows;
    /** Clients showing it, elsewhere */
    guint attached;
    /** Seconds since it was last used */
    gint64 idle;
} TmuxSession;

/** What tmux_list_sessions() found out */
typedef enum {
    /** tmux could not be asked: no answer over ssh (yet) */
    TMUX_NOT_ASKED,
    /** the terminal is in tmux already */
    TMUX_INSIDE,
    /** the sessions there are, none if tmux is not there */
    TMUX_LISTED,
} TmuxAsked;

/** @param sessions TmuxSession, the one used last first; not to be kept */
typedef void (*TmuxSessionsDone)(TmuxAsked asked, GPtrArray *sessions, gpointer data);

/**
 * @param index Window that rang, -1 if not known
 * @param label Its pane's title, or the window's name if the pane has no
 *        title of its own; NULL if not known
 * @param data The caller's
 */
typedef void (*TmuxBellDone)(gint index, const gchar *label, gpointer data);

gboolean tmux_enabled(void);
gboolean tmux_ask_bell(TmuxBellDone done, gpointer data);
void tmux_set_enabled(guint on);
gboolean tmux_may_have_terminal(void);
gboolean tmux_list_sessions(TmuxSessionsDone done, gpointer data);
gboolean tmux_send_command(const gchar *key);
void tmux_ask_where(gboolean cancel, TmuxAnswer done, gpointer data);

#endif /* tmux_h */
