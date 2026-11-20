/* refresh.h
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

#ifndef refresh_h
#define refresh_h

#include <gtk/gtk.h>

/** Refreshes a second while the terminal changes on its own, by default */
#define REFRESH_RATE_DEFAULT 2
/** For this long after a touch or a key, the screen follows at once */
#define REFRESH_ACTIVE_MS 3000
/** Minutes untouched after which the screen stops following, by default */
#define REFRESH_IDLE_PAUSE_DEFAULT 5

void refresh_init(GtkWidget *window);
void refresh_activity(void);
void refresh_attach(GtkWidget *terminal);
void refresh_set_rate(guint per_second);

#endif /* refresh_h */
