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

#include "icons.h"

static cairo_surface_t *icon_mask(const gchar *resource) {
    static GHashTable *masks = NULL;
    if (!masks) {
        masks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) cairo_surface_destroy);
    }
    gpointer cached = NULL;
    if (g_hash_table_lookup_extended(masks, resource, NULL, &cached)) {
        return cached;
    }

    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_resource(resource, &error);
    cairo_surface_t *surface = NULL;
    if (pixbuf) {
        surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, 1, NULL);
        g_object_unref(pixbuf);
    } else {
        g_warning("Cannot load icon %s: %s", resource, error->message);
        g_error_free(error);
    }
    g_hash_table_insert(masks, g_strdup(resource), surface);
    return surface;
}

void icon_draw(cairo_t *cr, const gchar *resource, gdouble x, gdouble y, gdouble size) {
    cairo_surface_t *mask = icon_mask(resource);
    if (!mask || size <= 0) {
        return;
    }
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, size / cairo_image_surface_get_width(mask), size / cairo_image_surface_get_height(mask));
    cairo_mask_surface(cr, mask, 0, 0);
    cairo_restore(cr);
}

static gboolean icon_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer resource) {
    GtkStyleContext *style = gtk_widget_get_style_context(widget);
    GdkRGBA color;
    gtk_style_context_get_color(style, gtk_style_context_get_state(style), &color);
    gdk_cairo_set_source_rgba(cr, &color);
    const gdouble width = gtk_widget_get_allocated_width(widget);
    const gdouble height = gtk_widget_get_allocated_height(widget);
    const gdouble size = MIN(width, height);
    icon_draw(cr, resource, (width - size) / 2, (height - size) / 2, size);
    return FALSE;
}

/** Show an icon resource in the surrounding text color. */
GtkWidget * icon_new(const gchar *resource, gint size) {
    GtkWidget *icon = gtk_drawing_area_new();
    gtk_widget_set_size_request(icon, size, size);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    g_signal_connect(icon, "draw", G_CALLBACK(icon_draw_cb), (gpointer) resource);
    return icon;
}
