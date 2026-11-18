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

/** Widget name of the page number at the end of the row */
#define SHORTBAR_PAGES_NAME "ktermShortPages"
/** Widget name of the paste button, there while something was copied */
#define SHORTBAR_PASTE_NAME "ktermShortPaste"
/** Widget name of the bar's two tools: the panel, the keyboard */
#define SHORTBAR_TOOL_NAME "ktermShortTool"

GtkWidget * shortbar_new(const gchar *path);
void shortbar_follow(GtkWidget *keyboard, guint rows, GtkWidget *other);
GPtrArray * shortbar_items(void);
gboolean shortbar_save(GError **error);
void shortbar_offer_paste(const gchar *text);
void shortbar_update(void);

#endif /* shortbar_h */
