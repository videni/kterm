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

gboolean tmux_may_have_terminal(void);
gboolean tmux_send_command(const gchar *key);
void tmux_ask_where(gboolean cancel, TmuxAnswer done, gpointer data);

#endif /* tmux_h */
