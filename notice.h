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

/** A tap on a notice, not on one of its buttons */
typedef void (*NoticeTapped)(void);

void notices_init(GtkWidget *overlay);
GtkWidget * notice_new(GtkWidget *content, NoticeTapped tapped);
GtkWidget * notice_button(const gchar *icon);
void notice_add(GtkWidget *notice, GtkWidget *widget, gboolean first);
void notice_show(GtkWidget *notice);

#endif /* notice_h */
