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

/** The connection to a far machine shared by kterm's questions to tmux
 *  and, opened from the panel, the session itself (ssh -o ControlPath=) */
#define TMUX_CONTROL_PATH "/tmp/kterm-ssh-%C"
/** It is kept this long after the last use, s */
#define TMUX_CONTROL_PERSIST "600"

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

gboolean tmux_enabled(void);
void tmux_set_enabled(guint on);
gboolean tmux_may_have_terminal(void);
gboolean tmux_list_sessions(TmuxSessionsDone done, gpointer data);
gboolean tmux_send_command(const gchar *key);
void tmux_ask_where(gboolean cancel, TmuxAnswer done, gpointer data);

#endif /* tmux_h */
