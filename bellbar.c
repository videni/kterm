/* bellbar.c
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
 * The bell (BEL) of the terminal: a program asking for attention, Claude
 * Code done or waiting, a tmux window's bell passed on. Shown as a bar at
 * the top of the screen, over whatever is shown there, kterm or a book: a
 * window of its own the window manager leaves alone, so kterm is not
 * brought to the front. It says what rang: the tmux window and its pane's
 * title (asked of tmux, tmux.c), or the terminal's title. A tap takes it
 * away; a bell while it is up only changes what it says.
 */

#include <math.h>
#include <string.h>
#include <vte/vte.h>
#include "config.h"
#include "bellbar.h"
#include "icons.h"
#include "kterm.h"
#include "panel.h"
#include "tmux.h"

/** Corners of the bar, px; as in kindle.c's styles */
#define BELL_RADIUS 24
/** Most characters of what rang */
#define BELL_LABEL_CHARS 60

static GtkWidget *bar = NULL;
static GtkWidget *message = NULL;
/** tmux is being asked which window rang: more bells wait for it */
static gboolean asking = FALSE;

/**
 * Cut the corners off the bar's window, round as its frame: what is under
 * them shows
 */
static void round_corners(GtkWidget *widget, GdkRectangle *allocation, gpointer data) {
    UNUSED(data);
    const gint width = allocation->width;
    const gint height = allocation->height;
    const gint radius = MIN(BELL_RADIUS, MIN(width, height) / 2);
    cairo_region_t *region = cairo_region_create();
    for (gint y = 0; y < height; y++) {
        // rows of the corners start in from the side
        const gint from_edge = y < radius ? radius - 1 - y : (y >= height - radius ? y - (height - radius) : -1);
        gint inset = 0;
        if (from_edge >= 0) {
            const gdouble dy = from_edge + 0.5;
            inset = radius - (gint) floor(sqrt((gdouble) radius * radius - dy * dy));
        }
        const cairo_rectangle_int_t row = { inset, y, width - 2 * inset, 1 };
        cairo_region_union_rectangle(region, &row);
    }
    gtk_widget_shape_combine_region(widget, region);
    cairo_region_destroy(region);
}

static gboolean bar_event_cb(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    UNUSED(widget);
    UNUSED(data);
    if (event->type == GDK_BUTTON_RELEASE) {
        gtk_widget_hide(bar);
    }
    return TRUE;
}

/**
 * Show the bar, across the top of the screen as it is turned now
 * @param text What rang
 */
static void show_bar(const gchar *text) {
    D printf("bell: %s\n", text);
    gtk_label_set_text(GTK_LABEL(message), text);
    GdkScreen *screen = gtk_widget_get_screen(bar);
    const gint width = gdk_screen_get_width(screen) - 2 * PANEL_SPACING;
    gtk_widget_set_size_request(bar, width, -1);
    gtk_window_move(GTK_WINDOW(bar), PANEL_SPACING, PANEL_SPACING);
    gtk_widget_show(bar);
}

/**
 * What rang without tmux: the terminal's title, if it has one
 */
static void show_own(void) {
    gchar *title = kterm_terminal_title();
    show_bar(title ? title : "终端响铃");
    g_free(title);
}

static void bell_done(gint index, const gchar *label, gpointer data) {
    UNUSED(data);
    asking = FALSE;
    if (index < 0) {
        show_own();
        return;
    }
    gchar *text = label && *label ? g_strdup_printf("窗口 %d · %s", index, label)
                                  : g_strdup_printf("窗口 %d", index);
    show_bar(text);
    g_free(text);
}

static void bell_cb(VteTerminal *terminal, gpointer data) {
    UNUSED(terminal);
    UNUSED(data);
    if (asking) {
        return;
    }
    asking = tmux_ask_bell(bell_done, NULL);
    if (!asking) {
        show_own();
    }
}

/**
 * @param window kterm's window: the bar is drawn the way it is
 * @param terminal Terminal widget, whose bell is shown
 */
void bellbar_init(GtkWidget *window, GtkWidget *terminal) {
    // no beep: the Kindle has no speaker
    vte_terminal_set_audible_bell(VTE_TERMINAL(terminal), FALSE);
    g_signal_connect(terminal, "bell", G_CALLBACK(bell_cb), NULL);

    // a window the window manager leaves alone: over everything, kterm's
    // own window not raised for it
    bar = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_widget_set_visual(bar, gtk_widget_get_visual(window));
    gtk_widget_add_events(bar, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(bar, "button-press-event", G_CALLBACK(bar_event_cb), NULL);
    g_signal_connect(bar, "button-release-event", G_CALLBACK(bar_event_cb), NULL);
    g_signal_connect(bar, "size-allocate", G_CALLBACK(round_corners), NULL);

    // the frame: the box's, the window's corners cut to it
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING);
    gtk_widget_set_name(row, "ktermBell");
    GtkWidget *bell = icon_new_outline(OUTLINE_BELL, 32);
    message = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(message), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(message), BELL_LABEL_CHARS);
    gtk_label_set_xalign(GTK_LABEL(message), 0);
    // a tap anywhere closes, the ✕ says so
    GtkWidget *x = icon_new_outline(OUTLINE_X, 32);
    gtk_box_pack_start(GTK_BOX(row), bell, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), message, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row), x, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(bar), row);
    gtk_widget_show_all(row);
}
