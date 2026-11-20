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
 * Notices: boxes over the top of the terminal, in kterm's own window, each
 * a rounded frame: what it says and, at its top right, a round close
 * button; under it a line of buttons with words, for what is done with it
 * (none: no line). As wide as what it says, up to NOTICE_MAX_PX, centred;
 * longer, it goes on over a few lines, then loses its middle. Text from
 * the phone (voicebar.c) and the bell (bellbar.c) have one each. Several
 * are stacked, the one shown last on top. The panel is drawn over them,
 * an image over everything (kterm.c adds them in that order).
 */

#include "config.h"
#include "icons.h"
#include "notice.h"
#include "panel.h"

#define NOTICE_NAME "ktermNotice"
#define NOTICE_BUTTON_NAME "ktermNoticeButton"
#define NOTICE_ICON_PX 32
/** Widest a notice is, frame and all, px: some 20 characters of Chinese */
#define NOTICE_MAX_PX 720
/** Room between the text, the icons and buttons, px */
#define NOTICE_GAP 8
/** Its parts (g_object_set_data()): first line, text, line of buttons,
 *  most lines of text */
#define NOTICE_TOP "notice-top"
#define NOTICE_TEXT "notice-text"
#define NOTICE_ACTIONS "notice-actions"
#define NOTICE_LINES "notice-lines"

/** The notices, one under the other */
static GtkWidget *stack = NULL;

void notices_set_top(gint top) {
    gtk_widget_set_margin_top(stack, top + PANEL_SPACING);
}

/**
 * @param overlay Overlay of the window: the notices are drawn over the
 *        terminal's top. Before the panel is added, which covers them.
 */
void notices_init(GtkWidget *overlay) {
    stack = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING / 2);
    gtk_widget_set_valign(stack, GTK_ALIGN_START);
    gtk_widget_set_margin_start(stack, PANEL_SPACING);
    gtk_widget_set_margin_end(stack, PANEL_SPACING);
    notices_set_top(0);
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
 * @param lines Most lines of text: more loses its middle
 * @param tapped Told of a tap on it, not on a button; NULL for none
 * @param closed Told of the close button
 * @return The notice
 */
GtkWidget * notice_new(gint lines, NoticeTapped tapped, NoticeTapped closed) {
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
    // as wide as it needs, in the middle
    gtk_widget_set_halign(notice, GTK_ALIGN_CENTER);
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_name(frame, NOTICE_NAME);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, NOTICE_GAP);
    // as wide as notice_set_text() makes it, wrapped there
    GtkWidget *text = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(text), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(text), 1);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(top), text, TRUE, TRUE, 0);
    // round, alone at the top right, away from the buttons below
    GtkWidget *close = notice_button(icon_new_outline(OUTLINE_X, NOTICE_ICON_PX));
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "round");
    gtk_widget_set_valign(close, GTK_ALIGN_START);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(closed), NULL);
    gtk_box_pack_end(GTK_BOX(top), close, FALSE, FALSE, 0);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, NOTICE_GAP);
    gtk_box_pack_start(GTK_BOX(frame), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(frame), actions, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(notice), frame);
    gtk_widget_show_all(frame);
    gtk_widget_set_no_show_all(notice, TRUE);
    g_object_set_data(G_OBJECT(notice), NOTICE_TOP, top);
    g_object_set_data(G_OBJECT(notice), NOTICE_TEXT, text);
    g_object_set_data(G_OBJECT(notice), NOTICE_ACTIONS, actions);
    g_object_set_data(G_OBJECT(notice), NOTICE_LINES, GINT_TO_POINTER(lines));
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
 * @param layout Layout, wrapped at the room there is
 * @param text Text
 * @param lines Most lines
 * @return TRUE if the text takes no more lines
 */
static gboolean fits(PangoLayout *layout, const gchar *text, gint lines) {
    pango_layout_set_text(layout, text, -1);
    return pango_layout_get_line_count(layout) <= lines;
}

/**
 * What of a text is shown in a few lines: all of it, or most of how it
 * starts and some of how it ends, "…" for its middle
 * @param layout Layout, wrapped at the room there is
 * @param text Text
 * @param lines Most lines
 * @return Text to show
 */
static gchar * fit_text(PangoLayout *layout, const gchar *text, gint lines) {
    if (fits(layout, text, lines)) {
        return g_strdup(text);
    }
    // the most characters kept, three fifths from the start, that fit
    const glong chars = g_utf8_strlen(text, -1);
    gchar *best = g_strdup("…");
    glong lo = 0, hi = chars;
    while (lo < hi) {
        const glong keep = (lo + hi + 1) / 2;
        const glong head = keep * 3 / 5;
        gchar *start = g_strndup(text, (gsize) (g_utf8_offset_to_pointer(text, head) - text));
        gchar *shown = g_strconcat(start, "…", g_utf8_offset_to_pointer(text, chars - (keep - head)), NULL);
        g_free(start);
        if (fits(layout, shown, lines)) {
            g_free(best);
            best = shown;
            lo = keep;
        } else {
            g_free(shown);
            hi = keep - 1;
        }
    }
    return best;
}

/**
 * Set what a notice says, and its width to fit it: as wide as the text on
 * one line, else as wide as a notice is, over its lines
 * @param notice Notice
 * @param text Text, line breaks kept
 */
void notice_set_text(GtkWidget *notice, const gchar *text) {
    GtkWidget *label = g_object_get_data(G_OBJECT(notice), NOTICE_TEXT);
    GtkWidget *frame = gtk_bin_get_child(GTK_BIN(notice));
    const gint lines = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(notice), NOTICE_LINES));
    // the room for the text: the notice less its frame, the close button
    // and the icon before it, if any; less if the screen is narrower
    gint room = NOTICE_MAX_PX;
    const gint screen = gtk_widget_get_allocated_width(stack);
    if (screen > 1) {
        room = MIN(room, screen);
    }
    GtkStyleContext *style = gtk_widget_get_style_context(frame);
    GtkBorder padding, border;
    gtk_style_context_get_padding(style, gtk_style_context_get_state(style), &padding);
    gtk_style_context_get_border(style, gtk_style_context_get_state(style), &border);
    room -= padding.left + padding.right + border.left + border.right;
    GList *parts = gtk_container_get_children(GTK_CONTAINER(g_object_get_data(G_OBJECT(notice), NOTICE_TOP)));
    for (GList *part = parts; part; part = part->next) {
        if (part->data != label) {
            gint width = 0;
            gtk_widget_get_preferred_width(part->data, NULL, &width);
            room -= width + NOTICE_GAP;
        }
    }
    g_list_free(parts);
    room = MAX(room, 1);

    gchar *clean = g_strdup(text);
    g_strdelimit(clean, "\r\t", ' ');
    PangoLayout *layout = gtk_widget_create_pango_layout(label, NULL);
    pango_layout_set_width(layout, room * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    gchar *shown = fit_text(layout, clean, lines);
    pango_layout_set_text(layout, shown, -1);
    gint width = room;
    if (pango_layout_get_line_count(layout) == 1) {
        pango_layout_get_pixel_size(layout, &width, NULL);
        // not wrapped for a rounding
        width = MIN(width + 1, room);
    }
    g_object_unref(layout);
    gtk_label_set_text(GTK_LABEL(label), shown);
    gtk_widget_set_size_request(label, width, -1);
    g_free(shown);
    g_free(clean);
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
