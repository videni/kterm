/* shortbar.h
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

#ifndef shortbar_h
#define shortbar_h

#include <gtk/gtk.h>

/** Widget name of the bar, for styling */
#define SHORTBAR_NAME "ktermShortBar"
/** Widget name of its buttons */
#define SHORTBAR_BUTTON_NAME "ktermShortButton"
/** Widget name of the line telling about mistakes in the file */
#define SHORTBAR_PROBLEM_NAME "ktermShortProblem"

GtkWidget * shortbar_new(const gchar *path);
void shortbar_follow(GtkWidget *bar, GtkWidget *keyboard_box, guint keyboard_rows, GtkWidget *other);

#endif /* shortbar_h */
