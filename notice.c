/* notice.c
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
 * Notices: bars over the top of the terminal, in kterm's own window, each
 * a rounded frame with a text and buttons of icons. Text from the phone
 * (voicebar.c) and the bell (bellbar.c) have one each. Several are
 * stacked, the one shown last on top. The panel is drawn over them, an
 * image over everything (kterm.c adds them in that order).
 */

#include "config.h"
#include "icons.h"
#include "notice.h"
#include "panel.h"

#define NOTICE_NAME "ktermNotice"
#define NOTICE_BUTTON_NAME "ktermNoticeButton"
#define NOTICE_ICON_PX 32

/** The notices, one under the other */
static GtkWidget *stack = NULL;

/**
 * @param overlay Overlay of the window: the notices are drawn over the
 *        terminal's top. Before the panel is added, which covers them.
 */
void notices_init(GtkWidget *overlay) {
    stack = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING / 2);
    gtk_widget_set_valign(stack, GTK_ALIGN_START);
    gtk_widget_set_margin_start(stack, PANEL_SPACING);
    gtk_widget_set_margin_end(stack, PANEL_SPACING);
    gtk_widget_set_margin_top(stack, PANEL_SPACING);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), stack);
    gtk_widget_show(stack);
}

/** Who is told of a tap on a notice */
typedef struct {
    NoticeTapped tapped;
} Tap;

/**
 * A touch on a notice: taken, so that it does not reach the terminal
 * below; its buttons act, a tap elsewhere is told
 */
static gboolean notice_event(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    UNUSED(widget);
    const Tap *tap = data;
    if (event->type == GDK_BUTTON_RELEASE && tap->tapped) {
        tap->tapped();
    }
    return TRUE;
}

/**
 * A new notice, not shown yet
 * @param content What it says, taking the room the buttons leave
 * @param tapped Told of a tap on it, NULL for none
 * @return The notice
 */
GtkWidget * notice_new(GtkWidget *content, NoticeTapped tapped) {
    // takes the touches; drawn by the box inside it, rounded, the
    // terminal showing at its corners
    GtkWidget *notice = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(notice), FALSE);
    gtk_widget_add_events(notice, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    // notices stay for as long as kterm runs
    Tap *tap = g_new0(Tap, 1);
    tap->tapped = tapped;
    g_signal_connect(notice, "button-press-event", G_CALLBACK(notice_event), tap);
    g_signal_connect(notice, "button-release-event", G_CALLBACK(notice_event), tap);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_name(row, NOTICE_NAME);
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row), content, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(notice), row);
    gtk_widget_show_all(row);
    gtk_widget_set_no_show_all(notice, TRUE);
    gtk_box_pack_start(GTK_BOX(stack), notice, FALSE, FALSE, 0);
    return notice;
}

/**
 * @param icon Icon, OUTLINE_...: the sign alone
 * @return A button for a notice
 */
GtkWidget * notice_button(const gchar *icon) {
    GtkWidget *button = gtk_button_new();
    gtk_widget_set_name(button, NOTICE_BUTTON_NAME);
    gtk_widget_set_can_focus(button, FALSE);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(button), icon_new_outline(icon, NOTICE_ICON_PX));
    return button;
}

/**
 * Add a button, or an icon, to a notice
 * @param notice Notice
 * @param widget What is added
 * @param first At the far left, before the text; else after what is there
 */
void notice_add(GtkWidget *notice, GtkWidget *widget, gboolean first) {
    GtkWidget *row = gtk_bin_get_child(GTK_BIN(notice));
    gtk_box_pack_start(GTK_BOX(row), widget, FALSE, FALSE, 0);
    if (first) {
        gtk_box_reorder_child(GTK_BOX(row), widget, 0);
    }
    gtk_widget_show_all(widget);
}

/**
 * Show a notice: on top of the others if it was not up
 * @param notice Notice
 */
void notice_show(GtkWidget *notice) {
    if (!gtk_widget_get_visible(notice)) {
        gtk_box_reorder_child(GTK_BOX(stack), notice, 0);
    }
    gtk_widget_show(notice);
}
