/* icons.c
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

/*
 * Icons drawn from SVG paths, in the color of the text around them
 */

#include <glib.h>
#include "icons.h"

/**
 * Read the next number of an SVG path
 * @param p Position in the path, moved past the number
 * @return The number, 0 if there is none
 */
static gdouble path_number(const gchar **p) {
    while (**p == ' ' || **p == ',') {
        (*p)++;
    }
    gchar *end = NULL;
    const gdouble value = g_ascii_strtod(*p, &end);
    if (end == *p) {
        // no number: step over whatever is there, so that reading ends
        if (**p) {
            (*p)++;
        }
        return 0;
    }
    *p = end;
    return value;
}

/**
 * Add an SVG path to the cairo path: the commands of the icons drawn here
 * (M L H V C S Z, absolute or relative), no arcs
 * @param cr Cairo context, its matrix set to the grid of the path
 * @param d The path
 */
static void svg_path(cairo_t *cr, const gchar *d) {
    gdouble x = 0, y = 0, start_x = 0, start_y = 0;
    // second control point of the last curve, for S
    gdouble ctrl_x = 0, ctrl_y = 0;
    gchar command = 0, last = 0;
    const gchar *p = d;
    while (*p) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (g_ascii_isalpha(*p)) {
            command = *p++;
        } else if (command == 'M') {
            // more points after a move are lines
            command = 'L';
        } else if (command == 'm') {
            command = 'l';
        }
        const gboolean relative = g_ascii_islower(command);
        const gdouble ox = relative ? x : 0, oy = relative ? y : 0;
        switch (g_ascii_toupper(command)) {
        case 'M':
            x = ox + path_number(&p);
            y = oy + path_number(&p);
            cairo_move_to(cr, x, y);
            start_x = x;
            start_y = y;
            break;
        case 'L':
            x = ox + path_number(&p);
            y = oy + path_number(&p);
            cairo_line_to(cr, x, y);
            break;
        case 'H':
            x = ox + path_number(&p);
            cairo_line_to(cr, x, y);
            break;
        case 'V':
            y = oy + path_number(&p);
            cairo_line_to(cr, x, y);
            break;
        case 'C':
        case 'S': {
            gdouble x1, y1;
            if (g_ascii_toupper(command) == 'C') {
                x1 = ox + path_number(&p);
                y1 = oy + path_number(&p);
            } else if (g_ascii_toupper(last) == 'C' || g_ascii_toupper(last) == 'S') {
                // the first control point mirrors the last one
                x1 = 2 * x - ctrl_x;
                y1 = 2 * y - ctrl_y;
            } else {
                x1 = x;
                y1 = y;
            }
            ctrl_x = ox + path_number(&p);
            ctrl_y = oy + path_number(&p);
            x = ox + path_number(&p);
            y = oy + path_number(&p);
            cairo_curve_to(cr, x1, y1, ctrl_x, ctrl_y, x, y);
            break;
        }
        case 'Z':
            cairo_close_path(cr);
            x = start_x;
            y = start_y;
            break;
        default:
            // not drawn here: stop rather than draw something wrong
            return;
        }
        last = command;
    }
}

/**
 * Draw an icon, filled
 * @param cr Cairo context, its source the color to draw in
 * @param path SVG path on a grid of 24 by 24 (ICON_...)
 * @param x,y Top left corner
 * @param size Width and height
 */
void icon_draw(cairo_t *cr, const gchar *path, gdouble x, gdouble y, gdouble size) {
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, size / 24, size / 24);
    svg_path(cr, path);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_fill(cr);
    cairo_restore(cr);
}

/**
 * @param widget Icon widget
 * @param cr Cairo context
 * @param path SVG path of the icon
 * @return FALSE
 */
static gboolean icon_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer path) {
    // the color of the text: an icon in a button turns white with its label
    GtkStyleContext *style = gtk_widget_get_style_context(widget);
    GdkRGBA color;
    gtk_style_context_get_color(style, gtk_style_context_get_state(style), &color);
    gdk_cairo_set_source_rgba(cr, &color);
    const gdouble w = gtk_widget_get_allocated_width(widget);
    const gdouble h = gtk_widget_get_allocated_height(widget);
    const gdouble size = MIN(w, h);
    if (g_object_get_data(G_OBJECT(widget), "outline")) {
        // lines 2 wide on the grid of 24, ends and corners round
        cairo_save(cr);
        cairo_translate(cr, (w - size) / 2, (h - size) / 2);
        cairo_scale(cr, size / 24, size / 24);
        svg_path(cr, path);
        cairo_set_line_width(cr, 2);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);
        cairo_restore(cr);
    } else {
        icon_draw(cr, path, (w - size) / 2, (h - size) / 2, size);
    }
    return FALSE;
}

/**
 * An icon to put next to a label
 * @param path SVG path (ICON_...), not copied: a constant
 * @param size Width and height in pixels
 * @return Widget
 */
GtkWidget * icon_new(const gchar *path, gint size) {
    GtkWidget *icon = gtk_drawing_area_new();
    gtk_widget_set_size_request(icon, size, size);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    g_signal_connect(icon, "draw", G_CALLBACK(icon_draw_cb), (gpointer) path);
    return icon;
}

/**
 * An outlined icon (OUTLINE_...), drawn as lines, to put next to a label
 * @param path SVG path, not copied: a constant
 * @param size Width and height in pixels
 * @return Widget
 */
GtkWidget * icon_new_outline(const gchar *path, gint size) {
    GtkWidget *icon = icon_new(path, size);
    g_object_set_data(G_OBJECT(icon), "outline", GINT_TO_POINTER(1));
    return icon;
}
