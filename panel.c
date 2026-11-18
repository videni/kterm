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
 * The panel opened by swiping up. It opens on a row of actions, the ones
 * used often: the saved ssh connections (a page of their own, with the form
 * to add or edit one), hanging up the ssh session in the terminal, reading
 * mode, the settings page and quitting. The settings page holds what is
 * changed rarely: the font size, the screen orientation and the shortcuts
 * (shortpage.c, a page below it). Each page has a way back.
 *
 * It is drawn inside the main window rather than as a window of its own:
 * the terminal keeps its size, so the program running in it is not
 * disturbed. It sits at the bottom of the window, over the last rows of the
 * keyboard, which stays in view above it; a form sits above the keyboard,
 * which types into it.
 */

#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <gtk/gtk.h>
#include "config.h"
#include "panel.h"
#include "icons.h"

#if GTK_CHECK_VERSION(3,2,0)

#include <vte/vte.h>
#include "sshconf.h"
#include "keyboard.h"

/** Connections shown at once; more are reached with the page buttons.
 *  Dragging a list with a finger needs touch events the Kindle's X server
 *  does not deliver. */
#define ROWS_PER_PAGE 4
/** Space around and between the panel's parts, in pixels */
#define PANEL_SPACING 12
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
/** On the first page while an ssh session has the terminal */
static GtkWidget *disconnect_button = NULL;
static GtkWidget *settings_page = NULL;
/** Rows of the settings page */
static GtkWidget *settings_box = NULL;
static GtkWidget *list_page = NULL;
static GtkWidget *form_page = NULL;
static GtkWidget *rows_box = NULL;
static GtkWidget *empty_label = NULL;
static GtkWidget *page_label = NULL;
static GtkWidget *prev_button = NULL;
static GtkWidget *next_button = NULL;
static GtkWidget *form_title = NULL;
static GtkWidget *entry_name = NULL;
static GtkWidget *entry_host = NULL;
static GtkWidget *entry_user = NULL;
static GtkWidget *entry_port = NULL;
static GtkWidget *form_message = NULL;
static GtkWidget *delete_button = NULL;
/** Connections as last read from the configuration */
static GPtrArray *hosts = NULL;
/** First connection of the page shown */
static guint page_start = 0;
/** Name of the connection in the form, NULL for a new one */
static gchar *editing_name = NULL;
/** Delete was pressed once and waits for the second press */
static gboolean delete_armed = FALSE;

static void show_list(void);
static pid_t ssh_session(void);

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
    if (page == home_page && disconnect_button) {
        gtk_widget_set_visible(disconnect_button, ssh_session() != 0);
    }
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
 * Describe where a connection goes: user@address:port
 * @param host Connection
 * @return Text, to be freed
 */
static gchar * describe(const SshHost *host) {
    GString *text = g_string_new(NULL);
    if (*host->user) {
        g_string_append_printf(text, "%s@", host->user);
    }
    g_string_append(text, host->host);
    if (*host->port && strcmp(host->port, "22")) {
        g_string_append_printf(text, ":%s", host->port);
    }
    return g_string_free(text, FALSE);
}

/** Command to type once the session being left has ended */
static gchar *switch_command = NULL;
static guint switch_tries = 0;
static guint switch_source = 0;
/** What is typed to start a connection. ^E ^U first: anything half typed
 *  on the command line would otherwise run together with the command.
 *  Names are validated to be plain words. */
#define SSH_COMMAND "\x05\x15ssh %s\r"

/**
 * @param pgid Process group
 * @return TRUE if its leader is the ssh client
 */
static gboolean is_ssh(pid_t pgid) {
    gchar *path = g_strdup_printf("/proc/%d/comm", (int) pgid);
    gchar *name = NULL;
    const gboolean ssh = g_file_get_contents(path, &name, NULL, NULL) && !strcmp(g_strstrip(name), "ssh");
    g_free(name);
    g_free(path);
    return ssh;
}

/**
 * @return Process group of the ssh client that has the terminal, 0 if
 *         there is none
 */
static pid_t ssh_session(void) {
    const pid_t foreground = kterm_terminal_foreground();
    return (foreground && is_ssh(foreground)) ? foreground : 0;
}

/**
 * Disconnect button callback: hang up the ssh session in the terminal.
 * Only the client here ends, as when its window is closed, and the shell
 * it was started from is back; the far machine only sees the connection
 * go (programs left running in tmux there keep running).
 */
static void disconnect_cb(void) {
    const pid_t session = ssh_session();
    if (session) {
        kill(-session, SIGHUP);
    }
    panel_close();
}

/**
 * Timer callback: type the pending ssh command as soon as the shell has
 * the terminal back. Given up after three seconds; nothing is typed then.
 * @param data Not used
 * @return FALSE when done
 */
static gboolean switch_when_idle(gpointer data) {
    UNUSED(data);
    const gboolean busy = kterm_terminal_foreground() != 0;
    if (busy && ++switch_tries < 30) {
        return TRUE;
    }
    if (!busy && switch_command && terminal_widget) {
        vte_terminal_feed_child(VTE_TERMINAL(terminal_widget), switch_command, -1);
    }
    g_free(switch_command);
    switch_command = NULL;
    switch_source = 0;
    return FALSE;
}

/**
 * Connection row callback: run ssh in the terminal.
 * The command is typed into the shell that is already there, so the
 * session behaves exactly as if the user had typed it, and ends back at
 * that shell.
 * @param button Calling widget
 * @param data Index of the connection
 */
static void connect_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    const guint index = GPOINTER_TO_UINT(data);
    if (!hosts || index >= hosts->len || !terminal_widget) {
        return;
    }
    const SshHost *host = g_ptr_array_index(hosts, index);
    if (!kterm_terminal_has_shell()) {
        // kterm runs a program of its own (kterm -e tmux): there is no
        // shell of ours to type the command into
        gtk_label_set_text(GTK_LABEL(empty_label), "终端里运行的不是 shell，无法在这里连接。");
        gtk_widget_show(empty_label);
        return;
    }
    // Typed while something else has the terminal, the command would go to
    // that program: inside an ssh session it would run on the far machine,
    // which does not know the names saved here.
    const pid_t session = ssh_session();
    if (session) {
        // Another connection is open: picking one switches to it. The open
        // session is hung up, as when its window is closed, and the new
        // command is typed once the shell is back.
        kill(-session, SIGHUP);
        g_free(switch_command);
        switch_command = g_strdup_printf(SSH_COMMAND, host->name);
        switch_tries = 0;
        if (!switch_source) {
            switch_source = g_timeout_add(100, switch_when_idle, NULL);
        }
        panel_close();
        return;
    }
    if (kterm_terminal_foreground()) {
        // some other program (an editor, a pager): not ours to end
        gtk_label_set_text(GTK_LABEL(empty_label), "终端里有程序在运行，先退出它再连接。");
        gtk_widget_show(empty_label);
        return;
    }
    gchar *command = g_strdup_printf(SSH_COMMAND, host->name);
    vte_terminal_feed_child(VTE_TERMINAL(terminal_widget), command, -1);
    g_free(command);
    panel_close();
}

/**
 * Show the form
 * @param host Connection to edit, NULL for a new one
 */
static void show_form(const SshHost *host) {
    // typed with the built-in keyboard, wherever it is at the moment
    kterm_use_builtin_keyboard();
    g_free(editing_name);
    editing_name = host ? g_strdup(host->name) : NULL;
    delete_armed = FALSE;
    gtk_label_set_text(GTK_LABEL(form_title), host ? "编辑连接" : "新建连接");
    gtk_entry_set_text(GTK_ENTRY(entry_name), host ? host->name : "");
    gtk_entry_set_text(GTK_ENTRY(entry_host), host ? host->host : "");
    gtk_entry_set_text(GTK_ENTRY(entry_user), host ? host->user : "");
    gtk_entry_set_text(GTK_ENTRY(entry_port), host ? host->port : "");
    gtk_label_set_text(GTK_LABEL(form_message), "");
    gtk_button_set_label(GTK_BUTTON(delete_button), "删除");
    gtk_widget_set_visible(delete_button, host != NULL);
    panel_show_page(form_page);
    gtk_widget_grab_focus(entry_name);
}

/**
 * Edit button callback
 * @param button Calling widget
 * @param data Index of the connection
 */
static void edit_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    const guint index = GPOINTER_TO_UINT(data);
    if (hosts && index < hosts->len) {
        show_form(g_ptr_array_index(hosts, index));
    }
}

/**
 * New connection button callback
 */
static void new_cb(void) {
    show_form(NULL);
}

/**
 * Build one row of the list: the connection itself, and its edit button
 * @param index Index of the connection
 * @return Row widget
 */
static GtkWidget * build_row(guint index) {
    const SshHost *host = g_ptr_array_index(hosts, index);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING);
    gchar *where = describe(host);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>   %s", host->name, where);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    g_free(markup);
    g_free(where);
    GtkWidget *connect = gtk_button_new();
    gtk_widget_set_name(connect, PANEL_BUTTON_NAME);
    gtk_widget_set_can_focus(connect, FALSE);
    gtk_container_add(GTK_CONTAINER(connect), label);
    g_signal_connect(connect, "clicked", G_CALLBACK(connect_cb), GUINT_TO_POINTER(index));
    GtkWidget *edit = panel_button_new("编辑");
    g_signal_connect(edit, "clicked", G_CALLBACK(edit_cb), GUINT_TO_POINTER(index));
    gtk_box_pack_start(GTK_BOX(row), connect, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), edit, FALSE, FALSE, 0);
    return row;
}

/**
 * Read the connections again and show the list page
 */
static void show_list(void) {
    if (hosts) {
        g_ptr_array_unref(hosts);
    }
    gchar *path = sshconf_default_path();
    GError *error = NULL;
    hosts = sshconf_list(path, &error);
    g_free(path);

    GList *old_rows = gtk_container_get_children(GTK_CONTAINER(rows_box));
    for (GList *cur = old_rows; cur != NULL; cur = cur->next) {
        gtk_widget_destroy(GTK_WIDGET(cur->data));
    }
    g_list_free(old_rows);

    const guint count = hosts ? hosts->len : 0;
    if (page_start >= count) {
        // the last connection of the last page was removed
        page_start = count ? ((count - 1) / ROWS_PER_PAGE) * ROWS_PER_PAGE : 0;
    }
    guint shown = 0;
    for (guint i = page_start; i < count && i < page_start + ROWS_PER_PAGE; i++, shown++) {
        gtk_box_pack_start(GTK_BOX(rows_box), build_row(i), FALSE, FALSE, 0);
    }
    // A short last page is padded to full height: the page buttons must not
    // move under the finger between one press and the next.
    for (; count > ROWS_PER_PAGE && shown < ROWS_PER_PAGE; shown++) {
        GtkWidget *filler = panel_button_new(" ");
        gtk_widget_set_sensitive(filler, FALSE);
        gtk_widget_set_opacity(filler, 0);
        gtk_box_pack_start(GTK_BOX(rows_box), filler, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(rows_box);

    if (error) {
        gtk_label_set_text(GTK_LABEL(empty_label), error->message);
        g_error_free(error);
    } else {
        gtk_label_set_text(GTK_LABEL(empty_label), "还没有保存的连接，点“新建”添加");
    }
    gtk_widget_set_visible(empty_label, count == 0);
    const gboolean paged = count > ROWS_PER_PAGE;
    gtk_widget_set_visible(prev_button, paged);
    gtk_widget_set_visible(next_button, paged);
    gtk_widget_set_visible(page_label, paged);
    if (paged) {
        gchar *pages = g_strdup_printf("%u/%u", page_start / ROWS_PER_PAGE + 1,
                                       (count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
        gtk_label_set_text(GTK_LABEL(page_label), pages);
        g_free(pages);
        gtk_widget_set_sensitive(prev_button, page_start > 0);
        gtk_widget_set_sensitive(next_button, page_start + ROWS_PER_PAGE < count);
    }
    panel_show_page(list_page);
    focus_terminal();
}

/**
 * The first page's connections button: the list, from its first page
 */
static void connections_cb(void) {
    page_start = 0;
    show_list();
}

/**
 * Page button callback
 * @param button Calling widget
 * @param data Direction: -1 or 1
 */
static void page_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    const gint direction = GPOINTER_TO_INT(data);
    if (direction < 0) {
        page_start = (page_start >= ROWS_PER_PAGE) ? page_start - ROWS_PER_PAGE : 0;
    } else {
        page_start += ROWS_PER_PAGE;
    }
    show_list();
}

/**
 * @param entry Entry widget
 * @return The entry's text without surrounding blanks, to be freed
 */
static gchar * entry_value(GtkWidget *entry) {
    gchar *text = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    return g_strstrip(text);
}

/**
 * Save button callback: write the form to the ssh configuration
 */
static void save_cb(void) {
    gchar *name = entry_value(entry_name);
    gchar *address = entry_value(entry_host);
    gchar *user = entry_value(entry_user);
    gchar *port = entry_value(entry_port);
    SshHost *host = ssh_host_new(name, address, user, port);
    gchar *path = sshconf_default_path();
    GError *error = NULL;
    if (sshconf_save(path, editing_name, host, &error)) {
        D printf("panel: saved %s\n", host->name);
        show_list();
    } else {
        gtk_label_set_text(GTK_LABEL(form_message), error->message);
        g_error_free(error);
    }
    g_free(path);
    ssh_host_free(host);
    g_free(name);
    g_free(address);
    g_free(user);
    g_free(port);
}

/**
 * Delete button callback. Asks once more on the button itself: a second
 * press is quicker on this screen than a confirmation window.
 */
static void delete_cb(void) {
    if (!editing_name) {
        return;
    }
    if (!delete_armed) {
        delete_armed = TRUE;
        gtk_button_set_label(GTK_BUTTON(delete_button), "确认删除");
        return;
    }
    gchar *path = sshconf_default_path();
    GError *error = NULL;
    if (sshconf_delete(path, editing_name, &error)) {
        show_list();
    } else {
        gtk_label_set_text(GTK_LABEL(form_message), error->message);
        g_error_free(error);
    }
    g_free(path);
}

/**
 * Enter in a field: go on to the next one, save from the last
 * @param entry Calling widget
 * @param next Next entry, NULL for the last
 */
static void entry_activate_cb(GtkWidget *entry, gpointer next) {
    UNUSED(entry);
    if (next) {
        gtk_widget_grab_focus(GTK_WIDGET(next));
    } else {
        save_cb();
    }
}

/**
 * Add a labelled field to the form
 * @param grid Form grid
 * @param row Grid row
 * @param title Field label
 * @param hint Shown in the empty field
 * @return Entry widget
 */
static GtkWidget * add_field(GtkWidget *grid, gint row, const gchar *title, const gchar *hint) {
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), hint);
    gtk_widget_set_hexpand(entry, TRUE);
    // no capital letter suggestions, spelling or emoji for a host name
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_URL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
    return entry;
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
    GtkWidget *connections = panel_button_with_icon("连接", ICON_LINK);
    disconnect_button = panel_button_with_icon("断开", ICON_LINK_OFF);
    GtkWidget *settings = panel_button_with_icon("设置", ICON_SETTINGS);
    g_signal_connect(connections, "clicked", G_CALLBACK(connections_cb), NULL);
    g_signal_connect(disconnect_button, "clicked", G_CALLBACK(disconnect_cb), NULL);
    g_signal_connect_swapped(settings, "clicked", G_CALLBACK(panel_show_page), settings_page);
    gtk_box_pack_start(GTK_BOX(home_row), connections, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(home_row), disconnect_button, TRUE, TRUE, 0);
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
 * Build the list page
 * @return Page widget
 */
static GtkWidget * build_list_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    GtkWidget *header = panel_page_header(page, "SSH 连接", NULL);
    prev_button = panel_button_new("‹");
    next_button = panel_button_new("›");
    page_label = gtk_label_new(NULL);
    GtkWidget *add = panel_button_new("新建");
    g_signal_connect(prev_button, "clicked", G_CALLBACK(page_cb), GINT_TO_POINTER(-1));
    g_signal_connect(next_button, "clicked", G_CALLBACK(page_cb), GINT_TO_POINTER(1));
    g_signal_connect(add, "clicked", G_CALLBACK(new_cb), NULL);
    gtk_box_pack_start(GTK_BOX(header), prev_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), page_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), next_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), add, FALSE, FALSE, 0);

    rows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    empty_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(empty_label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(empty_label), TRUE);

    gtk_box_pack_start(GTK_BOX(page), rows_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), empty_label, FALSE, FALSE, 0);
    return page;
}

/**
 * Build the form page
 * @return Page widget
 */
static GtkWidget * build_form_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    form_title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(form_title), 0);
    PangoAttrList *bold = pango_attr_list_new();
    pango_attr_list_insert(bold, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(form_title), bold);
    pango_attr_list_unref(bold);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), PANEL_SPACING);
    gtk_grid_set_column_spacing(GTK_GRID(grid), PANEL_SPACING * 2);
    entry_name = add_field(grid, 0, "名称", "例如 mac");
    entry_host = add_field(grid, 1, "地址", "主机名或 IP");
    entry_user = add_field(grid, 2, "用户", "可不填");
    entry_port = add_field(grid, 3, "端口", "22");
    g_signal_connect(entry_name, "activate", G_CALLBACK(entry_activate_cb), entry_host);
    g_signal_connect(entry_host, "activate", G_CALLBACK(entry_activate_cb), entry_user);
    g_signal_connect(entry_user, "activate", G_CALLBACK(entry_activate_cb), entry_port);
    g_signal_connect(entry_port, "activate", G_CALLBACK(entry_activate_cb), NULL);

    form_message = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(form_message), 0);
    gtk_label_set_line_wrap(GTK_LABEL(form_message), TRUE);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING);
    gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
    GtkWidget *save = panel_button_new("保存");
    delete_button = panel_button_new("删除");
    GtkWidget *cancel = panel_button_new("取消");
    g_signal_connect(save, "clicked", G_CALLBACK(save_cb), NULL);
    g_signal_connect(delete_button, "clicked", G_CALLBACK(delete_cb), NULL);
    g_signal_connect(cancel, "clicked", G_CALLBACK(show_list), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), save, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), delete_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), cancel, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(page), form_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), form_message, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    return page;
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
    list_page = build_list_page();
    form_page = build_form_page();
    panel_add_page(home_page, FALSE);
    panel_add_page(list_page, FALSE);
    panel_add_page(form_page, TRUE);
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
