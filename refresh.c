/* refresh.c
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
 * Fewer screen refreshes, for the e-ink screen and the battery.
 *
 * Every change drawn costs a refresh of the e-ink screen, the most power
 * hungry thing the device does here. A program animating (Claude Code's
 * spinner and timers, a progress bar, a tmux status line with a spinning
 * window name) changed the screen several times a second, for hours, and
 * drained the battery in less than a day.
 *
 * While the terminal changes on its own, the window's drawing is frozen and
 * let go at most conf->refresh_rate times a second: GTK then draws in one go
 * what changed meanwhile. For a moment after a touch or a key nothing is
 * held back, typing is drawn at once. Untouched for conf->idle_pause
 * minutes, the screen stays as it is (an e-ink screen keeps its picture
 * without power) until it is touched again.
 */

#include <stdio.h>
#include <vte/vte.h>
#include "config.h"
#include "refresh.h"
#include "kterm.h"

extern KTconf *conf;

static GtkWidget *window = NULL;
/** The window's drawing is frozen */
static gboolean frozen = FALSE;
/** Timer letting the drawing go */
static guint thaw_source = 0;
/** Monotonic time of the last touch or key */
static gint64 last_input = 0;

/**
 * Let the window draw what changed
 */
static void thaw(void) {
    if (thaw_source) {
        g_source_remove(thaw_source);
        thaw_source = 0;
    }
    if (frozen && window && gtk_widget_get_window(window)) {
        gdk_window_thaw_updates(gtk_widget_get_window(window));
    }
    frozen = FALSE;
}

void refresh_activity(void) {
    last_input = g_get_monotonic_time();
    thaw();
}

static gboolean thaw_cb(gpointer data) {
    UNUSED(data);
    thaw_source = 0;
    thaw();
    return G_SOURCE_REMOVE;
}

/**
 * Events from the X server: a touch or a key lets everything be drawn at
 * once, for a while
 */
static void event_handler(GdkEvent *event, gpointer data) {
    UNUSED(data);
    switch (event->type) {
    case GDK_BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
    case GDK_KEY_PRESS:
    case GDK_KEY_RELEASE:
    case GDK_TOUCH_BEGIN:
    case GDK_TOUCH_END:
        refresh_activity();
        break;
    default:
        break;
    }
    gtk_main_do_event(event);
}

/**
 * The terminal changed: drawn now, or held back until the next refresh
 */
static void contents_changed_cb(VteTerminal *terminal, gpointer data) {
    UNUSED(data);
    if (GTK_WIDGET(terminal) != kterm_get_terminal()) {
        return;
    }
    const gint64 now = g_get_monotonic_time();
    if (frozen || !window || !gtk_widget_get_window(window)
        || now - last_input < (gint64) REFRESH_ACTIVE_MS * 1000) {
        return;
    }
    const gboolean idle = conf->idle_pause && now - last_input >= (gint64) conf->idle_pause * 60 * G_USEC_PER_SEC;
    if (!idle && !conf->refresh_rate) {
        return;
    }
    gdk_window_freeze_updates(gtk_widget_get_window(window));
    frozen = TRUE;
    if (!idle) {
        thaw_source = g_timeout_add(1000 / conf->refresh_rate, thaw_cb, NULL);
    }
    // idle: stays frozen until a touch
}

/**
 * Debug output: the terminal drawn, to count the refreshes (kterm -d)
 */
static gboolean drawn_cb(GtkWidget *terminal, cairo_t *cr, gpointer data) {
    UNUSED(terminal);
    UNUSED(cr);
    UNUSED(data);
    printf("refresh: drawn %" G_GINT64_FORMAT "\n", g_get_monotonic_time() / 1000);
    fflush(stdout);
    return FALSE;
}

/**
 * Hold back the refreshes of a window while its terminal changes on its own
 * @param toplevel Window
 */
void refresh_init(GtkWidget *toplevel) {
    window = toplevel;
    refresh_activity();
    gdk_event_handler_set(event_handler, NULL, NULL);
}

void refresh_attach(GtkWidget *terminal) {
    D g_signal_connect_after(terminal, "draw", G_CALLBACK(drawn_cb), NULL);
    g_signal_connect(terminal, "contents-changed", G_CALLBACK(contents_changed_cb), NULL);
}

/**
 * @param per_second Refreshes a second while the terminal changes on its
 *  own, 0 for as many as it changes
 */
void refresh_set_rate(guint per_second) {
    conf->refresh_rate = per_second;
    thaw();
}
