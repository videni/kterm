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
/** Widget name of the paste button, there while something was copied and
 *  the program waits for input */
#define SHORTBAR_PASTE_NAME "ktermShortPaste"
/** Widget name of the bar's two tools: the panel, the keyboard */
#define SHORTBAR_TOOL_NAME "ktermShortTool"
/** Widget name of the row of shortcuts between the tools */
#define SHORTBAR_SLOTS_NAME "ktermShortSlots"
/** Gap around a tool, as around the keys of the shortcuts */
#define SHORTBAR_TOOL_MARGIN 3
/** Side of the triangle in the corner of a prefix shortcut */
#define SHORTBAR_PREFIX_MARK_PX 26

GtkWidget * shortbar_new(const gchar *path);
void shortbar_follow(GtkWidget *keyboard, guint rows, GtkWidget *other);
GPtrArray * shortbar_items(void);
gboolean shortbar_save(GError **error);
void shortbar_offer_paste(const gchar *text);
void shortbar_paste_wanted(gboolean wanted);
void shortbar_update(void);

#endif /* shortbar_h */
