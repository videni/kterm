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

#define ICON_MENU IMAGE_RESOURCES "icons/menu.svg"
#define ICON_KEYBOARD_HIDE IMAGE_RESOURCES "icons/keyboard-hide.svg"
#define ICON_KEYBOARD IMAGE_RESOURCES "icons/keyboard.svg"
#define ICON_PASTE IMAGE_RESOURCES "icons/paste.svg"
#define ICON_SEND IMAGE_RESOURCES "icons/send.svg"
#define ICON_PLAY IMAGE_RESOURCES "icons/play.svg"
#define ICON_COPY IMAGE_RESOURCES "icons/copy.svg"
#define ICON_CLOSE IMAGE_RESOURCES "icons/close.svg"
#define ICON_BELL IMAGE_RESOURCES "icons/bell.svg"
#define ICON_GO IMAGE_RESOURCES "icons/go.svg"
#define ICON_NEW_TAB IMAGE_RESOURCES "icons/new-tab.svg"
#define ICON_POWER IMAGE_RESOURCES "icons/power.svg"
#define ICON_LINK IMAGE_RESOURCES "icons/link.svg"
#define ICON_SETTINGS IMAGE_RESOURCES "icons/settings.svg"
#define ICON_SHORTCUT IMAGE_RESOURCES "icons/shortcut.svg"

void icon_draw(cairo_t *cr, const gchar *resource, gdouble x, gdouble y, gdouble size);
GtkWidget * icon_new(const gchar *resource, gint size);

#endif /* icons_h */
