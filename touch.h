/* touch.h
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

#ifndef touch_h
#define touch_h

#include <gtk/gtk.h>

/** A swipe from a screen edge starts within this many pixels of the top.
 *  Wide: a finger coming in from the frame is
 *  first seen some way down the screen, and a narrow band made the swipe
 *  down to the device's menu scroll the terminal instead. */
#define TOUCH_EDGE_TOP_PX 150
/** and travels at least this far */
#define TOUCH_SWIPE_MIN_PX 96

gboolean touch_event(GtkWidget *terminal, GdkEvent *any);
gboolean touch_forwarding(void);
gboolean touch_scroll_event(GtkWidget *terminal, GdkEventScroll *event, gpointer data);

#endif /* touch_h */
