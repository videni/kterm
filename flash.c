/* flash.c
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
 * Full refreshes of the terminal, to clear what earlier pictures left
 * behind on the e-ink screen.
 *
 * The screen is refreshed only where it changed, and quickly: scrolling,
 * switching tmux windows or turning a page leaves a ghost of the text that
 * was there. Only a full refresh (the screen flashing black and white)
 * clears it, and the Kindle's window manager never does one for kterm on
 * its own.
 *
 * kterm has the page itself drawn as a full refresh, as a book's page is
 * turned on the Kindle: when it turns a page (a tap on the top or bottom
 * third while previewing), scrolls for a swipe, or switches tmux windows
 * for a swipe, it first tells the window manager to hold the screen until
 * the terminal is drawn anew, and to show it then with a full refresh.
 * Nothing flashes at other times: not while typing, not for what a program
 * draws on its own, not once the screen stands still.
 *
 * Every such change, with conf->full_refresh FLASH_ALWAYS. Otherwise only
 * once the terminal has been written over at least once since the last
 * full refresh by something else than these changes: what is drawn is
 * added up (a character typed is a sliver of a row, a screenful of output
 * a whole terminal), the pages turned and swipes not counted. Turning
 * pages one after another never flashes; a page turned after a screenful
 * of output does; one turned after a few keys does not.
 *
 * The request is the window manager's lab126_create_trigger client
 * message (see /etc/xdg/awesome/rc.lua and lab126_ligl.lua on the device).
 */

#include <stdio.h>
#include <string.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <vte/vte.h>
#include "config.h"
#include "flash.h"

extern KTconf *conf;

/** Trigger: once the whole rectangle has been drawn */
#define FLASH_TRIGGER_RECT 3
/** Waveform: GC, full grey levels */
#define FLASH_WAVEFORM 9
/** Most time the screen is held for the drawing, ms */
#define FLASH_WAIT_MS 800
/** Held this much longer once the rectangle is drawn, for what follows
 *  (more of the page, coming through ssh), ms */
#define FLASH_AFTER_MS 200
/** The screen still takes taps while held */
#define FLASH_NON_BLOCKING 0x0400

static GtkWidget *window = NULL;
static GtkWidget *terminal = NULL;
/** Drawn since the last full refresh, in terminals */
static gdouble written = 0;
/** Monotonic time until which what is drawn is a page turned or a swipe,
 *  not counted */
static gint64 turning_until = 0;

/**
 * Ask the window manager to show the next drawing of a widget as a full
 * refresh
 * @param area Terminal, or the window
 */
static void flash_send(GtkWidget *area) {
    GdkWindow *gdk_window = window ? gtk_widget_get_window(window) : NULL;
    if (!gdk_window || !GDK_IS_X11_WINDOW(gdk_window) || !gtk_widget_get_mapped(area)) {
        return;
    }
    gint x = 0, y = 0;
    if (!gtk_widget_translate_coordinates(area, window, 0, 0, &x, &y)) {
        return;
    }
    const gint scale = gtk_widget_get_scale_factor(area);
    // type, waveform (8 bits each); wait, wait after, x, y, width, height
    // (16 bits each); flags (32 bits)
    const guint16 values[6] = {
        FLASH_WAIT_MS, FLASH_AFTER_MS,
        (guint16) (x * scale), (guint16) (y * scale),
        (guint16) (gtk_widget_get_allocated_width(area) * scale),
        (guint16) (gtk_widget_get_allocated_height(area) * scale)
    };
    const guint32 flags = FLASH_NON_BLOCKING;
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_window_get_display(gdk_window));
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = GDK_WINDOW_XID(gdk_window);
    event.xclient.message_type = XInternAtom(display, "lab126_create_trigger", False);
    event.xclient.format = 8;
    event.xclient.data.b[0] = FLASH_TRIGGER_RECT;
    event.xclient.data.b[1] = FLASH_WAVEFORM;
    memcpy(event.xclient.data.b + 2, values, sizeof(values));
    memcpy(event.xclient.data.b + 2 + sizeof(values), &flags, sizeof(flags));
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
    D printf("flash: next drawing of %ux%u+%u+%u\n", values[4], values[5], values[2], values[3]);
}

/**
 * The terminal is about to be drawn anew for a change the user made (a
 * page turned, a swipe): shown as a full refresh if conf->full_refresh
 * says so, see above
 */
void flash_next_draw(void) {
    D printf("flash: written %.2f terminals\n", written);
    // what is drawn for it, through ssh too, is not counted
    turning_until = g_get_monotonic_time() + (gint64) (FLASH_WAIT_MS + FLASH_AFTER_MS) * 1000;
    if (conf->full_refresh == FLASH_NEVER
        || (conf->full_refresh != FLASH_ALWAYS && written < 1)) {
        return;
    }
    flash_send(terminal);
    written = 0;
}

/**
 * The whole window is about to be drawn anew (an image shown over it, or
 * taken away): shown as a full refresh unless full refreshes are off
 */
void flash_window_next_draw(void) {
    if (conf->full_refresh != FLASH_NEVER) {
        flash_send(window);
        written = 0;
    }
}

/**
 * The terminal was drawn: add up how much of it
 */
static gboolean drawn_cb(GtkWidget *widget, cairo_t *cr, gpointer data) {
    UNUSED(data);
    if (g_get_monotonic_time() < turning_until) {
        return FALSE;
    }
    // the text, without the margins around it
    VteTerminal *vte = VTE_TERMINAL(widget);
    const gdouble whole = (gdouble) vte_terminal_get_column_count(vte) * vte_terminal_get_char_width(vte)
                          * vte_terminal_get_row_count(vte) * vte_terminal_get_char_height(vte);
    if (whole <= 0) {
        return FALSE;
    }
    gdouble area = 0;
    cairo_rectangle_list_t *list = cairo_copy_clip_rectangle_list(cr);
    if (list->status == CAIRO_STATUS_SUCCESS) {
        for (gint i = 0; i < list->num_rectangles; i++) {
            area += list->rectangles[i].width * list->rectangles[i].height;
        }
    } else {
        GdkRectangle clip;
        if (gdk_cairo_get_clip_rectangle(cr, &clip)) {
            area = (gdouble) clip.width * clip.height;
        }
    }
    cairo_rectangle_list_destroy(list);
    written += MIN(area / whole, 1);
    return FALSE;
}

/**
 * @param toplevel Window
 * @param widget Terminal in it
 */
void flash_init(GtkWidget *toplevel, GtkWidget *widget) {
    window = toplevel;
    terminal = widget;
    g_signal_connect_after(terminal, "draw", G_CALLBACK(drawn_cb), NULL);
}

/**
 * @param when FLASH_NEVER, FLASH_ALWAYS or FLASH_WRITTEN_OVER
 */
void flash_set_when(guint when) {
    conf->full_refresh = when;
}
