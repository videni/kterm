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

/*
 * Material icons (Google, Apache License 2.0, fonts.google.com/icons):
 * their SVG paths on a grid of 24 by 24. Drawn, not taken from a font: the
 * fonts that have such signs differ from one system to the next.
 */
/** "menu": the panel */
#define ICON_MENU "M3 18h18v-2H3v2zm0-5h18v-2H3v2zm0-7v2h18V6H3z"
/** "keyboard_hide": with the keyboard out, a tap folds it away */
#define ICON_KEYBOARD_HIDE "M20 3H4c-1.1 0-1.99.9-1.99 2L2 15c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2z" \
    "m-9 3h2v2h-2V6zm0 3h2v2h-2V9zM8 6h2v2H8V6zm0 3h2v2H8V9zm-1 2H5V9h2v2zm0-3H5V6h2v2zm9 7H8v-2h8v2z" \
    "m0-4h-2V9h2v2zm0-3h-2V6h2v2zm3 3h-2V9h2v2zm0-3h-2V6h2v2zm-7 15l4-4H8l4 4z"
/** "keyboard": while previewing, a tap brings it back */
#define ICON_KEYBOARD "M20 5H4c-1.1 0-1.99.9-1.99 2L2 17c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V7c0-1.1-.9-2-2-2z" \
    "m-9 3h2v2h-2V8zm0 3h2v2h-2v-2zM8 8h2v2H8V8zm0 3h2v2H8v-2zm-1 2H5v-2h2v2zm0-3H5V8h2v2zm9 7H8v-2h8v2z" \
    "m0-4h-2v-2h2v2zm0-3h-2V8h2v2zm3 3h-2v-2h2v2zm0-3h-2V8h2v2z"

/** "link": the panel's connections */
#define ICON_LINK "M3.9 12c0-1.71 1.39-3.1 3.1-3.1h4V7H7c-2.76 0-5 2.24-5 5s2.24 5 5 5h4v-1.9H7c-1.71 0-3.1-1.39-3.1-3.1z" \
    "M8 13h8v-2H8v2zm9-6h-4v1.9h4c1.71 0 3.1 1.39 3.1 3.1s-1.39 3.1-3.1 3.1h-4V17h4c2.76 0 5-2.24 5-5s-2.24-5-5-5z"
/** "link_off": disconnecting */
#define ICON_LINK_OFF "M17 7h-4v1.9h4c1.71 0 3.1 1.39 3.1 3.1 0 1.43-.98 2.63-2.31 2.98l1.46 1.46" \
    "C20.88 15.61 22 13.95 22 12c0-2.76-2.24-5-5-5zm-1 4h-2.19l2 2H16zM2 4.27l3.11 3.11" \
    "C3.29 8.12 2 9.91 2 12c0 2.76 2.24 5 5 5h4v-1.9H7c-1.71 0-3.1-1.39-3.1-3.1 0-1.59 1.21-2.9 2.76-3.07" \
    "L8.73 11H8v2h2.73L13 15.27V17h1.73l4.01 4L20 19.74 3.27 3 2 4.27z"
/** "settings" */
#define ICON_SETTINGS "M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61" \
    "l-1.92-3.32c-.12-.22-.37-.29-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41-.48-.41" \
    "h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22" \
    "L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58" \
    "c-.18.14-.23.41-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54" \
    "c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96" \
    "c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58z" \
    "M12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"
/** "logout": leaving kterm */
#define ICON_LOGOUT "M17 7l-1.41 1.41L18.17 11H8v2h10.17l-2.58 2.58L17 17l5-5z" \
    "M4 5h8V3H4c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h8v-2H4V5z"

/** Material "content_paste" */
#define ICON_CONTENT_PASTE "M19 2h-4.18C14.4.84 13.3 0 12 0c-1.3 0-2.4.84-2.82 2H5c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h14c1.1 0 2-.9 2-2V4c0-1.1-.9-2-2-2zm-7 0c.55 0 1 .45 1 1s-.45 1-1 1-1-.45-1-1 .45-1 1-1zm7 18H5V4h2v3h10V4h2v16z"
/*
 * Outlined icons, drawn as lines (icon_new_outline()): those of the phone
 * page (kindle/web/page.html), Lucide (ISC License), with their small arcs
 * left out, which the round joins of the lines make up for.
 */
/** Lucide "send": put in */
#define OUTLINE_SEND "M15 21.7L22 2.3L2.3 9L10.8 13.2Z M21.9 2.1L10.9 13.1"
/** Lucide "play": put in and Enter */
#define OUTLINE_PLAY "M6 3L20 12L6 21Z"
/** Lucide "x": close, and throw away what it closes */
#define OUTLINE_X "M18 6L6 18 M6 6L18 18"

void icon_draw(cairo_t *cr, const gchar *path, gdouble x, gdouble y, gdouble size);
GtkWidget * icon_new(const gchar *path, gint size);
GtkWidget * icon_new_outline(const gchar *path, gint size);

#endif /* icons_h */
