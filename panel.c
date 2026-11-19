/* panel.c
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
 * The panel opened from the shortcut bar's ☰ button. It opens on a row of
 * actions, the ones used often: the saved ssh connections and hanging up
 * the session (connpage.c), the settings page and quitting. The settings page holds what is
 * changed rarely: the font size, the screen orientation and the shortcuts
 * (shortpage.c, a page below it). Each page has a way back.
 *
 * It is drawn inside the main window rather than as a window of its own:
 * the terminal keeps its size, so the program running in it is not
 * disturbed. It sits at the bottom of the window, over the last rows of the
 * keyboard, which stays in view above it; a form sits above the keyboard,
 * which types into it.
 */

#include <gtk/gtk.h>
#include "config.h"
#include "panel.h"
#include "icons.h"

#if GTK_CHECK_VERSION(3,2,0)

#include "kterm.h"

/** Size of the icons on the buttons, a little above the text */
#define PANEL_ICON_PX 54

static GtkWidget *root = NULL;
static GtkWidget *terminal_widget = NULL;
/** Holds the pages, one shown at a time */
static GtkWidget *content = NULL;
static GPtrArray *pages = NULL;
static GtkWidget *current_page = NULL;
static GtkWidget *home_page = NULL;
/** Row of actions on the first page */
static GtkWidget *home_row = NULL;
static GtkWidget *settings_page = NULL;
/** Rows of the settings page */
static GtkWidget *settings_box = NULL;

/**
 * Create a button in the panel's style.
 * Panel buttons never take the keyboard focus: keys must keep going to the
 * terminal, or to the field being edited.
 * @param label Button text
 * @return Button widget
 */
GtkWidget * panel_button_new(const gchar *label) {
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_name(button, PANEL_BUTTON_NAME);
    gtk_widget_set_can_focus(button, FALSE);
    return button;
}

static void destroy_cb(GtkWidget *widget, gpointer data) {
    UNUSED(data);
    gtk_widget_destroy(widget);
}

/**
 * Take everything out of a box, to fill it again
 * @param box Container
 */
void panel_clear_box(GtkWidget *box) {
    gtk_container_foreach(GTK_CONTAINER(box), destroy_cb, NULL);
}

/**
 * A button of the panel with an icon before its label
 * @param label Text
 * @param icon SVG path of the icon (ICON_...)
 * @return Button
 */
GtkWidget * panel_button_with_icon(const gchar *label, const gchar *icon) {
    GtkWidget *button = gtk_button_new();
    gtk_widget_set_name(button, PANEL_BUTTON_NAME);
    gtk_widget_set_can_focus(button, FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING * 2);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), icon_new(icon, PANEL_ICON_PX), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(label), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(button), box);
    gtk_widget_show_all(box);
    return button;
}

/**
 * @return True while the panel is on screen
 */
gboolean panel_is_open(void) {
    return root && gtk_widget_get_visible(root);
}

/**
 * @return True while the page shown is a form the keyboard types into
 */
static gboolean page_is_typing(void) {
    return current_page && g_object_get_data(G_OBJECT(current_page), "typing") != NULL;
}

/**
 * @return True while the keyboard is typing into the panel's form, so that
 *         nothing typed should reach the terminal
 */
gboolean panel_wants_keys(void) {
    return panel_is_open() && page_is_typing();
}

/**
 * Give the keyboard back to the terminal
 */
static void focus_terminal(void) {
    if (terminal_widget) {
        gtk_widget_grab_focus(terminal_widget);
    }
}

/**
 * Show one page of the panel, the others hidden
 * @param page Page widget, added with panel_add_page() or one of the panel's
 */
void panel_show_page(GtkWidget *page) {
    for (guint i = 0; pages && i < pages->len; i++) {
        GtkWidget *other = g_ptr_array_index(pages, i);
        if (other != page) {
            gtk_widget_hide(other);
        }
    }
    current_page = page;
    gtk_widget_show(page);
    // While a form is shown, keys such as Tab must not move the focus to
    // the terminal, where the rest of what is typed would run.
    if (terminal_widget) {
        gtk_widget_set_can_focus(terminal_widget, !page_is_typing());
    }
}

/**
 * Add a page
 * @param page Page widget, complete: what it shows is shown now, it is
 *  hidden until panel_show_page()
 * @param typing TRUE for a form: the keyboard types into it
 */
void panel_add_page(GtkWidget *page, gboolean typing) {
    if (typing) {
        g_object_set_data(G_OBJECT(page), "typing", GINT_TO_POINTER(1));
    }
    g_ptr_array_add(pages, page);
    gtk_box_pack_start(GTK_BOX(content), page, FALSE, FALSE, 0);
    gtk_widget_show_all(page);
    gtk_widget_hide(page);
}

/**
 * Hide the panel
 */
void panel_close(void) {
    if (!panel_is_open()) {
        return;
    }
    D printf("panel: close\n");
    gtk_widget_hide(root);
    if (terminal_widget) {
        gtk_widget_set_can_focus(terminal_widget, TRUE);
    }
    focus_terminal();
}

/**
 * Show the panel, on its first page
 */
void panel_open(void) {
    if (!root || panel_is_open()) {
        return;
    }
    panel_show_page(home_page);
    focus_terminal();
    gtk_widget_show(root);
}

/**
 * Back button callback
 * @param button Calling widget
 * @param parent Page to go back to, NULL for the first page
 */
static void back_cb(GtkWidget *button, gpointer parent) {
    UNUSED(button);
    panel_show_page(parent ? GTK_WIDGET(parent) : home_page);
    focus_terminal();
}

/**
 * Start a page with its back button and title
 * @param page Page widget
 * @param title Title text
 * @param parent Page the back button goes to, NULL for the first page
 * @return Header row, for more buttons on the right
 */
GtkWidget * panel_page_header(GtkWidget *page, const gchar *title, GtkWidget *parent) {
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING);
    // an arrow, as on the Kindle's own pages
    GtkWidget *back = panel_button_new("←");
    gtk_widget_set_name(back, PANEL_BACK_NAME);
    g_signal_connect(back, "clicked", G_CALLBACK(back_cb), parent);
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(header), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);
    return header;
}

/**
 * Build the first page: a row of actions. There is no close button: a touch
 * on the terminal puts the panel away. Quitting, added last, ends at the
 * far right, away from disconnecting.
 * @return Page widget
 */
static GtkWidget * build_home_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    home_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_name(home_row, "ktermPanelActions");
    gtk_box_set_homogeneous(GTK_BOX(home_row), TRUE);
    GtkWidget *settings = panel_button_with_icon("设置", ICON_SETTINGS);
    g_signal_connect_swapped(settings, "clicked", G_CALLBACK(panel_show_page), settings_page);
    gtk_box_pack_start(GTK_BOX(home_row), settings, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), home_row, FALSE, FALSE, 0);
    return page;
}

/**
 * Build the settings page; its rows are added with panel_add_setting()
 * @return Page widget
 */
static GtkWidget * build_settings_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    panel_page_header(page, "设置", NULL);
    settings_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    gtk_widget_set_name(settings_box, PANEL_SETTINGS_NAME);
    gtk_box_pack_start(GTK_BOX(page), settings_box, FALSE, FALSE, 0);
    return page;
}

/**
 * @return The first page, the row of actions
 */
GtkWidget * panel_home_page(void) {
    return home_page;
}

/**
 * Add a button to the first page's row
 * @param button Button, see panel_button_new()
 * @param position Place in the row counted from 0, -1 for the end
 */
void panel_add_action(GtkWidget *button, gint position) {
    gtk_box_pack_start(GTK_BOX(home_row), button, TRUE, TRUE, 0);
    if (position >= 0) {
        gtk_box_reorder_child(GTK_BOX(home_row), button, position);
    }
    gtk_widget_show(button);
}

/**
 * @return The settings page, for pages opened from it to go back to
 */
GtkWidget * panel_settings_page(void) {
    return settings_page;
}

/**
 * Add a row to the settings page: what is set on the left, the widget
 * setting it at the right edge, a line above
 * @param title What is set
 * @param control Widget that sets it, kept at its own width
 */
void panel_add_setting(const gchar *title, GtkWidget *control) {
    GtkWidget *line = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_name(line, PANEL_LINE_NAME);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING * 2);
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_halign(control, GTK_ALIGN_END);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings_box), line, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings_box), row, FALSE, FALSE, 0);
    gtk_widget_show_all(line);
    gtk_widget_show_all(row);
}

/**
 * Build the panel
 * @param terminal Terminal widget the connections are opened in
 * @return Panel widget, hidden; meant to be added to a GtkOverlay
 */
GtkWidget * panel_new(GtkWidget *terminal) {
    terminal_widget = terminal;
    // an event box has a window of its own: it paints a background over the
    // terminal and keeps touches on the panel from reaching it
    root = gtk_event_box_new();
    gtk_widget_set_name(root, PANEL_NAME);
    // at the top, as the Kindle's own menu bar when reading: the keyboard
    // and the newest output, at the foot of the terminal, stay in view
    gtk_widget_set_valign(root, GTK_ALIGN_START);
    gtk_widget_set_halign(root, GTK_ALIGN_FILL);

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(content), PANEL_SPACING * 2);
    gtk_container_add(GTK_CONTAINER(root), content);
    pages = g_ptr_array_new();
    // the settings page first: the home page's button refers to it
    settings_page = build_settings_page();
    home_page = build_home_page();
    panel_add_page(home_page, FALSE);
    panel_add_page(settings_page, FALSE);
    gtk_widget_show(content);
    panel_show_page(home_page);
    // stays hidden through gtk_widget_show_all() on the window
    gtk_widget_set_no_show_all(root, TRUE);
    return root;
}

#else /* GTK+ 2: no overlay to draw the panel in */

void panel_open(void) {}
void panel_close(void) {}
gboolean panel_is_open(void) { return FALSE; }
gboolean panel_wants_keys(void) { return FALSE; }

#endif
