/* imageview.h
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

#ifndef imageview_h
#define imageview_h

#include <gtk/gtk.h>

void imageview_init(GtkWidget *overlay);
void imageview_attach(GtkWidget *terminal);
void imageview_hide(void);
gboolean imageview_tap(GtkWidget *terminal, GdkEvent *event);

#endif /* imageview_h */
