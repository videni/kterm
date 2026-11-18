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
 * The panel that slides over the bottom of the terminal. It opens on a row
 * of actions; two of them lead to pages of their own, each with a way back:
 * the saved ssh connections (with the form to add or edit one) and the
 * settings.
 *
 * It is drawn inside the main window, over the terminal, rather than as a
 * window of its own: the terminal keeps its size, so the program running in
 * it is not disturbed, and the on-screen keyboard below stays usable for the
 * connection form.
 */

#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <gtk/gtk.h>
#include "config.h"
#include "panel.h"

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

static GtkWidget *root = NULL;
static GtkWidget *terminal_widget = NULL;
static GtkWidget *home_page = NULL;
/** Row of actions on the first page */
static GtkWidget *home_row = NULL;
static GtkWidget *settings_page = NULL;
static GtkWidget *settings_grid = NULL;
static gint settings_count = 0;
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
 * @return True while the panel is on screen
 */
gboolean panel_is_open(void) {
    return root && gtk_widget_get_visible(root);
}

/**
 * @return True while the keyboard is typing into the panel's form, so that
 *         nothing typed should reach the terminal
 */
gboolean panel_wants_keys(void) {
    return panel_is_open() && form_page && gtk_widget_get_visible(form_page);
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
 * @param page Page widget
 */
static void show_page(GtkWidget *page) {
    GtkWidget *pages[] = { home_page, list_page, form_page, settings_page };
    for (guint i = 0; i < G_N_ELEMENTS(pages); i++) {
        if (pages[i] && pages[i] != page) {
            gtk_widget_hide(pages[i]);
        }
    }
    gtk_widget_show(page);
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
    focus_terminal();
}

/**
 * Show the panel, on its first page
 */
void panel_open(void) {
    if (!root || panel_is_open()) {
        return;
    }
    show_page(home_page);
    focus_terminal();
    gtk_widget_show(root);
}

/**
 * Back to the first page
 */
static void show_home(void) {
    show_page(home_page);
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

/**
 * Connection row callback: run ssh in the terminal.
 * The command is typed into the shell that is already there, so the
 * session behaves exactly as if the user had typed it, and ends back at
 * that shell.
 * @param button Calling widget
 * @param data Index of the connection
 */
/** Command to type once the session being left has ended */
static gchar *switch_command = NULL;
static guint switch_tries = 0;

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
 * Timer callback: type the pending ssh command as soon as the shell has
 * the terminal back. Given up after three seconds; nothing is typed then.
 * @param data Not used
 * @return FALSE when done
 */
static gboolean switch_when_idle(gpointer data) {
    UNUSED(data);
    if (kterm_terminal_busy() && ++switch_tries < 30) {
        return TRUE;
    }
    if (!kterm_terminal_busy() && switch_command && terminal_widget) {
        vte_terminal_feed_child(VTE_TERMINAL(terminal_widget), switch_command, -1);
    }
    g_free(switch_command);
    switch_command = NULL;
    return FALSE;
}

static void connect_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    const guint index = GPOINTER_TO_UINT(data);
    if (!hosts || index >= hosts->len || !terminal_widget) {
        return;
    }
    const SshHost *host = g_ptr_array_index(hosts, index);
    // Typed while something else has the terminal, the command would go to
    // that program: inside an ssh session it would run on the far machine,
    // which does not know the names saved here.
    const pid_t foreground = kterm_terminal_foreground();
    if (foreground && is_ssh(foreground)) {
        // Another connection is open: picking one switches to it. The open
        // session is hung up, as when its window is closed, and the new
        // command is typed once the shell is back.
        kill(-foreground, SIGHUP);
        g_free(switch_command);
        switch_command = g_strdup_printf("\x15ssh %s\r", host->name);
        switch_tries = 0;
        g_timeout_add(100, switch_when_idle, NULL);
        panel_close();
        return;
    }
    if (foreground) {
        // some other program (an editor, a pager): not ours to end
        gtk_label_set_text(GTK_LABEL(empty_label), "终端里有程序在运行，先退出它再连接。");
        gtk_widget_show(empty_label);
        return;
    }
    // ^U first: anything half typed on the command line would otherwise be
    // run together with the command. Names are validated to be plain words.
    gchar *command = g_strdup_printf("\x15ssh %s\r", host->name);
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
    show_page(form_page);
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
    show_page(list_page);
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
 * @return Header row, for more buttons on the right
 */
static GtkWidget * add_header(GtkWidget *page, const gchar *title) {
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING);
    // an arrow, as on the Kindle's own pages
    GtkWidget *back = panel_button_new("←");
    gtk_widget_set_name(back, PANEL_BACK_NAME);
    g_signal_connect(back, "clicked", G_CALLBACK(show_home), NULL);
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
 * on the terminal puts the panel away.
 * @return Page widget
 */
static GtkWidget * build_home_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    home_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_name(home_row, "ktermPanelActions");
    gtk_box_set_homogeneous(GTK_BOX(home_row), TRUE);
    GtkWidget *connections = panel_button_new("连接");
    GtkWidget *settings = panel_button_new("设置");
    g_signal_connect(connections, "clicked", G_CALLBACK(connections_cb), NULL);
    g_signal_connect_swapped(settings, "clicked", G_CALLBACK(show_page), settings_page);
    gtk_box_pack_start(GTK_BOX(home_row), connections, TRUE, TRUE, 0);
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
    add_header(page, "设置");
    settings_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(settings_grid), PANEL_SPACING);
    gtk_grid_set_column_spacing(GTK_GRID(settings_grid), PANEL_SPACING * 2);
    gtk_box_pack_start(GTK_BOX(page), settings_grid, FALSE, FALSE, 0);
    return page;
}

/**
 * Build the list page
 * @return Page widget
 */
static GtkWidget * build_list_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    GtkWidget *header = add_header(page, "SSH 连接");
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
 */
void panel_add_action(GtkWidget *button) {
    gtk_box_pack_start(GTK_BOX(home_row), button, TRUE, TRUE, 0);
    gtk_widget_show(button);
}

/**
 * Add a row to the settings page
 * @param title What is set
 * @param control Widget that sets it
 */
void panel_add_setting(const gchar *title, GtkWidget *control) {
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_grid_attach(GTK_GRID(settings_grid), label, 0, settings_count, 1, 1);
    gtk_grid_attach(GTK_GRID(settings_grid), control, 1, settings_count, 1, 1);
    settings_count++;
    gtk_widget_show_all(label);
    gtk_widget_show_all(control);
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
    gtk_widget_set_valign(root, GTK_ALIGN_END);
    gtk_widget_set_halign(root, GTK_ALIGN_FILL);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(content), PANEL_SPACING * 2);
    // the settings page first: the home page's button refers to it
    settings_page = build_settings_page();
    home_page = build_home_page();
    list_page = build_list_page();
    form_page = build_form_page();
    gtk_box_pack_start(GTK_BOX(content), home_page, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), list_page, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), form_page, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), settings_page, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(root), content);

    gtk_widget_show_all(content);
    show_page(home_page);
    // stays hidden through gtk_widget_show_all() on the window
    gtk_widget_set_no_show_all(root, TRUE);
    return root;
}

#else /* GTK+ 2: no overlay to draw the panel in */

GtkWidget * panel_new(GtkWidget *terminal) {
    UNUSED(terminal);
    return NULL;
}
void panel_add_action(GtkWidget *button) { UNUSED(button); }
void panel_add_setting(const gchar *title, GtkWidget *control) { UNUSED(title); UNUSED(control); }
GtkWidget * panel_button_new(const gchar *label) { return gtk_button_new_with_label(label); }
void panel_open(void) {}
void panel_close(void) {}
gboolean panel_is_open(void) { return FALSE; }
gboolean panel_wants_keys(void) { return FALSE; }

#endif
