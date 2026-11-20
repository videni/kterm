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

#include <string.h>
#include "icons.h"

typedef struct {
    const guint8 *data;
    gsize remaining;
} IconPng;

static cairo_status_t read_icon_png(void *closure, unsigned char *buffer, unsigned int length) {
    IconPng *png = closure;
    if (length > png->remaining) {
        return CAIRO_STATUS_READ_ERROR;
    }
    memcpy(buffer, png->data, length);
    png->data += length;
    png->remaining -= length;
    return CAIRO_STATUS_SUCCESS;
}

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
    GBytes *bytes = g_resources_lookup_data(resource, G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    cairo_surface_t *surface = NULL;
    if (bytes) {
        // These PNGs are embedded at build time. Drawing must not wait on
        // an external image loader.
        IconPng png;
        png.data = g_bytes_get_data(bytes, &png.remaining);
        surface = cairo_image_surface_create_from_png_stream(read_icon_png, &png);
        const cairo_status_t status = cairo_surface_status(surface);
        if (status != CAIRO_STATUS_SUCCESS) {
            g_warning("Cannot decode icon %s: %s", resource, cairo_status_to_string(status));
            cairo_surface_destroy(surface);
            surface = NULL;
        }
        g_bytes_unref(bytes);
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
