/* icons.h
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

#ifndef icons_h
#define icons_h

#include <gtk/gtk.h>

#define IMAGE_RESOURCES "/net/fabiszewski/kterm/"

#define ICON_MENU IMAGE_RESOURCES "icons/menu.png"
#define ICON_KEYBOARD_HIDE IMAGE_RESOURCES "icons/keyboard-hide.png"
#define ICON_KEYBOARD IMAGE_RESOURCES "icons/keyboard.png"
#define ICON_PASTE IMAGE_RESOURCES "icons/paste.png"
#define ICON_SEND IMAGE_RESOURCES "icons/send.png"
#define ICON_PLAY IMAGE_RESOURCES "icons/play.png"
#define ICON_COPY IMAGE_RESOURCES "icons/copy.png"
#define ICON_CLOSE IMAGE_RESOURCES "icons/close.png"
#define ICON_BELL IMAGE_RESOURCES "icons/bell.png"
#define ICON_GO IMAGE_RESOURCES "icons/go.png"
#define ICON_NEW_TAB IMAGE_RESOURCES "icons/new-tab.png"
#define ICON_POWER IMAGE_RESOURCES "icons/power.png"
#define ICON_LINK IMAGE_RESOURCES "icons/link.png"
#define ICON_SETTINGS IMAGE_RESOURCES "icons/settings.png"
#define ICON_SHORTCUT IMAGE_RESOURCES "icons/shortcut.png"
#define ICON_TAB_LIST IMAGE_RESOURCES "icons/tab-list.png"

void icon_draw(cairo_t *cr, const gchar *resource, gdouble x, gdouble y, gdouble size);
GtkWidget * icon_new(const gchar *resource, gint size);

#endif /* icons_h */
