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
 * a rounded frame: a line with what it says and, at its right, a close
 * button; under it a line of buttons with words, for what is done with it
 * (none: no line). Text from the phone (voicebar.c) and the bell
 * (bellbar.c) have one each. Several are stacked, the one shown last on
 * top. The panel is drawn over them, an image over everything (kterm.c
 * adds them in that order).
 */

#include "config.h"
#include "icons.h"
#include "notice.h"
#include "panel.h"

#define NOTICE_NAME "ktermNotice"
#define NOTICE_BUTTON_NAME "ktermNoticeButton"
#define NOTICE_ICON_PX 32
/** Its first line and its buttons' (g_object_set_data()) */
#define NOTICE_TOP "notice-top"
#define NOTICE_ACTIONS "notice-actions"

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
 * @param child Icon, or icon and words
 * @return A button of a notice
 */
static GtkWidget * notice_button(GtkWidget *child) {
    GtkWidget *button = gtk_button_new();
    gtk_widget_set_name(button, NOTICE_BUTTON_NAME);
    gtk_widget_set_can_focus(button, FALSE);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(button), child);
    return button;
}

/**
 * A new notice, not shown yet
 * @param content What it says, taking the room the close button leaves
 * @param tapped Told of a tap on it, not on a button; NULL for none
 * @param closed Told of the close button
 * @return The notice
 */
GtkWidget * notice_new(GtkWidget *content, NoticeTapped tapped, NoticeTapped closed) {
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
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_name(frame, NOTICE_NAME);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(top), content, TRUE, TRUE, 0);
    // alone at the right, away from the buttons below
    GtkWidget *close = notice_button(icon_new_outline(OUTLINE_X, NOTICE_ICON_PX));
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(closed), NULL);
    gtk_box_pack_end(GTK_BOX(top), close, FALSE, FALSE, 0);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(frame), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(frame), actions, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(notice), frame);
    gtk_widget_show_all(frame);
    gtk_widget_set_no_show_all(notice, TRUE);
    g_object_set_data(G_OBJECT(notice), NOTICE_TOP, top);
    g_object_set_data(G_OBJECT(notice), NOTICE_ACTIONS, actions);
    gtk_box_pack_start(GTK_BOX(stack), notice, FALSE, FALSE, 0);
    return notice;
}

/**
 * Put an icon before what a notice says
 * @param notice Notice
 * @param icon Icon widget
 */
void notice_lead(GtkWidget *notice, GtkWidget *icon) {
    GtkWidget *top = g_object_get_data(G_OBJECT(notice), NOTICE_TOP);
    gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(top), icon, 0);
    gtk_widget_show(icon);
}

/**
 * Add a button to the line of buttons, after those there
 * @param notice Notice
 * @param icon Icon, OUTLINE_...
 * @param words What it does
 * @return The button
 */
GtkWidget * notice_action(GtkWidget *notice, const gchar *icon, const gchar *words) {
    GtkWidget *inside = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(inside), icon_new_outline(icon, NOTICE_ICON_PX), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(inside), gtk_label_new(words), FALSE, FALSE, 0);
    GtkWidget *button = notice_button(inside);
    GtkWidget *actions = g_object_get_data(G_OBJECT(notice), NOTICE_ACTIONS);
    gtk_box_pack_start(GTK_BOX(actions), button, FALSE, FALSE, 0);
    gtk_widget_show_all(button);
    return button;
}

/**
 * Show a notice: on top of the others if it was not up; its line of
 * buttons only if one of them is shown
 * @param notice Notice
 */
void notice_show(GtkWidget *notice) {
    GtkWidget *actions = g_object_get_data(G_OBJECT(notice), NOTICE_ACTIONS);
    gboolean any = FALSE;
    GList *buttons = gtk_container_get_children(GTK_CONTAINER(actions));
    for (GList *b = buttons; b; b = b->next) {
        any = any || gtk_widget_get_visible(b->data);
    }
    g_list_free(buttons);
    gtk_widget_set_visible(actions, any);
    if (!gtk_widget_get_visible(notice)) {
        gtk_box_reorder_child(GTK_BOX(stack), notice, 0);
    }
    gtk_widget_show(notice);
}
