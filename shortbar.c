/* shortbar.c
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
 * The shortcut bar: a row of buttons above the keyboard, each typing a
 * whole key sequence with one tap (C-u, or the tmux prefix). The buttons
 * come from shortcuts.conf, see shortcuts.c. The bar is on screen while a
 * keyboard is, the built-in one or the system one.
 */

#include <string.h>
#include <vte/vte.h>
#include "config.h"
#include "keyboard.h"
#include "panel.h"
#include "shortcuts.h"
#include "shortbar.h"

/** More buttons than this go on two rows */
#define SHORTBAR_ROW_MAX 8

static void button_clicked_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    const Shortcut *item = data;
    GtkWidget *terminal = kterm_get_terminal();
    // not while a form of the panel is being filled in, as for the keys
    if (!terminal || panel_wants_keys()) {
        return;
    }
    gsize size = 0;
    const gchar *keys = g_bytes_get_data(item->keys, &size);
    vte_terminal_feed_child(VTE_TERMINAL(terminal), keys, (gssize) size);
}

/**
 * Build the bar
 * @param path shortcuts.conf; the built-in buttons are used when it does
 *  not exist
 * @return Bar widget, or NULL when there is nothing to show
 */
GtkWidget * shortbar_new(const gchar *path) {
    gchar *problems = NULL;
    GPtrArray *list = shortcuts_load(path, &problems);
    if (list->len == 0 && !problems) {
        g_ptr_array_unref(list);
        return NULL;
    }
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(bar, SHORTBAR_NAME);
    // the buttons point into the list: it lives as long as the bar
    g_object_set_data_full(G_OBJECT(bar), "shortcuts", list, (GDestroyNotify) g_ptr_array_unref);

    if (problems) {
        // the first mistake is enough to find the file and the line
        gchar *end = strchr(problems, '\n');
        if (end) {
            *end = '\0';
        }
        gchar *text = g_strdup_printf("%s %s", path ? path : SHORTCUTS_FILE, problems);
        GtkWidget *label = gtk_label_new(text);
        gtk_widget_set_name(label, SHORTBAR_PROBLEM_NAME);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_START);
        gtk_box_pack_start(GTK_BOX(bar), label, FALSE, FALSE, 0);
        g_free(text);
        g_free(problems);
    }

    const guint rows = (list->len + SHORTBAR_ROW_MAX - 1) / SHORTBAR_ROW_MAX;
    const guint per_row = rows ? (list->len + rows - 1) / rows : 0;
    GtkWidget *row = NULL;
    for (guint i = 0; i < list->len; i++) {
        if (i % per_row == 0) {
            row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
            gtk_box_pack_start(GTK_BOX(bar), row, FALSE, FALSE, 0);
        }
        Shortcut *item = g_ptr_array_index(list, i);
        GtkWidget *button = gtk_button_new_with_label(item->label);
        gtk_widget_set_name(button, SHORTBAR_BUTTON_NAME);
        // the terminal keeps the keyboard focus
        gtk_widget_set_can_focus(button, FALSE);
        g_signal_connect(button, "clicked", G_CALLBACK(button_clicked_cb), item);
        gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    }
    g_object_set_data(G_OBJECT(bar), "rows", GUINT_TO_POINTER(rows));
    return bar;
}

/** The two widgets whose being on screen decides the bar's */
typedef struct {
    GtkWidget *bar;
    GtkWidget *keyboard_box;
    GtkWidget *other;
    guint keyboard_rows;
} Follow;

static void follow_visibility_cb(GtkWidget *widget, gpointer data) {
    UNUSED(widget);
    const Follow *follow = data;
    const gboolean shown = gtk_widget_get_visible(follow->keyboard_box)
        || (follow->other && gtk_widget_get_visible(follow->other));
    gtk_widget_set_visible(follow->bar, shown);
}

/**
 * Buttons as tall as the keys: the keyboard sets its height once it knows
 * the screen.
 */
static void follow_height_cb(GObject *object, GParamSpec *spec, gpointer data) {
    UNUSED(spec);
    const Follow *follow = data;
    gint height = -1;
    gtk_widget_get_size_request(GTK_WIDGET(object), NULL, &height);
    if (height <= 0 || follow->keyboard_rows == 0) {
        return;
    }
    const gint key_height = height / (gint) follow->keyboard_rows;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(follow->bar));
    for (GList *l = rows; l; l = l->next) {
        if (GTK_IS_BOX(l->data)) {
            gtk_widget_set_size_request(GTK_WIDGET(l->data), -1, key_height);
        }
    }
    g_list_free(rows);
}

/**
 * Show the bar while a keyboard is shown, with buttons as tall as its keys
 * @param bar Bar widget
 * @param keyboard_box Built-in keyboard
 * @param keyboard_rows Rows of keys in it
 * @param other Shown instead of the keyboard while the system keyboard is
 *  up, or NULL
 */
void shortbar_follow(GtkWidget *bar, GtkWidget *keyboard_box, guint keyboard_rows, GtkWidget *other) {
    Follow *follow = g_new0(Follow, 1);
    follow->bar = bar;
    follow->keyboard_box = keyboard_box;
    follow->other = other;
    follow->keyboard_rows = keyboard_rows;
    g_object_set_data_full(G_OBJECT(bar), "follow", follow, g_free);
    g_signal_connect_after(keyboard_box, "show", G_CALLBACK(follow_visibility_cb), follow);
    g_signal_connect_after(keyboard_box, "hide", G_CALLBACK(follow_visibility_cb), follow);
    if (other) {
        g_signal_connect_after(other, "show", G_CALLBACK(follow_visibility_cb), follow);
        g_signal_connect_after(other, "hide", G_CALLBACK(follow_visibility_cb), follow);
    }
    g_signal_connect(keyboard_box, "notify::height-request", G_CALLBACK(follow_height_cb), follow);
    follow_height_cb(G_OBJECT(keyboard_box), NULL, follow);
    follow_visibility_cb(NULL, follow);
}
