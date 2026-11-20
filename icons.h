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
/** Lucide "arrow-right": go there */
#define OUTLINE_GO "M5 12L19 12 M12 5L19 12L12 19"
/** A bell, after Lucide "bell" (its arcs as curves): the terminal rang */
#define OUTLINE_BELL "M6 16V10C6 6.7 8.7 4 12 4C15.3 4 18 6.7 18 10V16L20 18H4L6 16Z" \
    " M10 21C10.6 21.7 11.3 22 12 22C12.7 22 13.4 21.7 14 21 M12 2V4"

void icon_draw(cairo_t *cr, const gchar *path, gdouble x, gdouble y, gdouble size);
GtkWidget * icon_new(const gchar *path, gint size);
GtkWidget * icon_new_outline(const gchar *path, gint size);

#endif /* icons_h */
