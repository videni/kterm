/* remote.h
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

#ifndef remote_h
#define remote_h

#include <gtk/gtk.h>

/** The connection to a far machine shared by kterm's scripts and, opened
 *  from the panel, the session itself (ssh -o ControlPath=) */
#define REMOTE_CONTROL_PATH "/tmp/kterm-ssh-%C"
/** It is kept this long after the last use, s */
#define REMOTE_CONTROL_PERSIST "600"

/**
 * @param output What the script printed, NULL if it could not be run or
 *        did not finish in time
 * @param data The caller's
 */
typedef void (*RemoteDone)(GBytes *output, gpointer data);

gboolean remote_is_ssh(void);
gboolean remote_run(const gchar *script, const gchar * const *vars, gboolean here, guint timeout_ms,
                    RemoteDone done, gpointer data);

#endif /* remote_h */
