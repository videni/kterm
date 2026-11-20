/* notice.h
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

#ifndef notice_h
#define notice_h

#include <gtk/gtk.h>

/** Told of a tap on a notice (not on a button), or of its close button */
typedef void (*NoticeTapped)(void);

void notices_init(GtkWidget *overlay);
void notices_set_top(gint top);
GtkWidget * notice_new(gint lines, NoticeTapped tapped, NoticeTapped closed);
void notice_set_text(GtkWidget *notice, const gchar *text);
void notice_lead(GtkWidget *notice, GtkWidget *icon);
GtkWidget * notice_action(GtkWidget *notice, const gchar *icon, const gchar *words);
void notice_show(GtkWidget *notice);

#endif /* notice_h */
