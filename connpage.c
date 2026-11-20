/* connpage.c
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
 * The panel's ssh connections: the 连接 page lists the saved ones (Host
 * blocks of ~/.ssh/config, sshconf.c), a tap types "ssh NAME" into the
 * terminal; a form adds or edits one; 查找 lists the machines of the local
 * network (kterm-mdns) to add. 断开 in the connections tab, while a remote session
 * has the terminal, hangs it up.
 */

#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <vte/vte.h>
#include "config.h"
#include "connpage.h"
#include "kterm.h"
#include "panel.h"
#include "sshconf.h"
#ifdef KINDLE
#include "remote.h"
#endif

#if GTK_CHECK_VERSION(3,2,0)

/** Connections shown at once; more are reached with the page buttons.
 *  Dragging a list with a finger needs touch events the Kindle's X server
 *  does not deliver. */
#define ROWS_PER_PAGE 4
/** Shown in the connection list while a remote session owns the terminal. */
static GtkWidget *disconnect_button = NULL;
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
/** The form's key choices: one button for each key, 不用, 生成密钥 */
static GtkWidget *key_box = NULL;
/** Key chosen in the form ("~/.ssh/NAME"), "" for none */
static gchar *chosen_key = NULL;
/** Connections as last read from the configuration */
static GPtrArray *hosts = NULL;
/** First connection of the page shown */
static guint page_start = 0;
static guint rendered_page_start = G_MAXUINT;
static gboolean rendered_with_mosh = FALSE;
/** Name of the connection in the form, NULL for a new one */
static gchar *editing_name = NULL;
/** Delete was pressed once and waits for the second press */
static gboolean delete_armed = FALSE;
/** Page of the machines found in the local network */
static GtkWidget *found_page = NULL;
static GtkWidget *found_rows = NULL;
static GtkWidget *found_label = NULL;
/** A search is under way */
static gboolean searching = FALSE;

static void show_list(void);
static pid_t connection_session(void);

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

typedef struct {
    gchar *command;
    gint64 deadline;
    guint source;
} ConnectionSwitch;

static void connection_switch_free(gpointer data) {
    ConnectionSwitch *pending = data;
    if (pending->source) {
        g_source_remove(pending->source);
    }
    g_free(pending->command);
    g_free(pending);
}
/**
 * @param name Saved connection, a plain word (validated)
 * @return The command starting it, newly allocated. The session is also
 *         the connection kterm's own scripts go over (remote.c: questions
 *         to tmux, images): they need no login of their own, a password
 *         one works too.
 */
static gchar * connection_command(const gchar *name, gboolean mosh) {
#ifdef KINDLE
    const gchar *ssh = "ssh -o ControlMaster=auto -o ControlPath=" REMOTE_CONTROL_PATH
                       " -o ControlPersist=" REMOTE_CONTROL_PERSIST;
#else
    const gchar *ssh = "ssh";
#endif
    if (!mosh) {
        return g_strconcat(ssh, " ", name, NULL);
    }
    gchar *session = g_uuid_string_random();
    gchar *bootstrap = g_shell_quote(ssh);
    gchar *command = g_strdup_printf("kterm-mosh %s %s %s", name, session, bootstrap);
    g_free(bootstrap);
    g_free(session);
    return command;
}

static gboolean mosh_available(void) {
    gchar *launcher = g_find_program_in_path("kterm-mosh");
    gchar *wrapper = g_find_program_in_path("mosh");
    gchar *client = g_find_program_in_path("mosh-client");
    const gboolean available = launcher && wrapper && client;
    g_free(launcher);
    g_free(wrapper);
    g_free(client);
    return available;
}

/**
 * @return Process group of the remote client that has the terminal, 0 if
 *         there is none
 */
static pid_t connection_session(void) {
    const pid_t foreground = kterm_terminal_foreground(kterm_get_terminal());
    return remote_process_is_connection(foreground) ? foreground : 0;
}

/**
 * Disconnect button callback: hang up the remote session in the terminal.
 * Only the client here ends, as when its window is closed, and the shell
 * it was started from is back; the far machine only sees the connection
 * go (programs left running in tmux there keep running).
 */
static void disconnect_cb(void) {
    const pid_t session = connection_session();
    if (session) {
        kill(-session, SIGHUP);
    }
    panel_close();
}

/**
 * Timer callback: type the pending ssh command as soon as the shell has
 * the terminal back. Give up at the deadline if it is still busy.
 * @param data Terminal where the switch was requested
 */
static gboolean switch_when_idle(gpointer data) {
    GtkWidget *terminal = data;
    ConnectionSwitch *pending = g_object_get_data(G_OBJECT(terminal), "kterm-connection-switch");
    const gboolean busy = kterm_terminal_foreground(terminal) != 0;
    if (busy && g_get_monotonic_time() < pending->deadline) {
        return G_SOURCE_CONTINUE;
    }
    if (!busy && vte_terminal_get_pty(VTE_TERMINAL(terminal))) {
        kterm_type_command(terminal, pending->command);
    }
    pending->source = 0;
    g_object_set_data(G_OBJECT(terminal), "kterm-connection-switch", NULL);
    return G_SOURCE_REMOVE;
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
    const guint index = GPOINTER_TO_UINT(data);
    GtkWidget *terminal = kterm_get_terminal();
    if (!hosts || index >= hosts->len || !terminal) {
        return;
    }
    const SshHost *host = g_ptr_array_index(hosts, index);
    const gboolean mosh = g_object_get_data(G_OBJECT(button), "mosh") != NULL;
    if (mosh && !mosh_available()) {
        gtk_label_set_text(GTK_LABEL(empty_label), "Kindle 未安装 Mosh 客户端。可点主机名走 SSH。");
        gtk_widget_show(empty_label);
        return;
    }
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
    const pid_t session = connection_session();
    if (session) {
        // Another connection is open: picking one switches to it. The open
        // session is hung up, as when its window is closed, and the new
        // command is typed once the shell is back.
        // Mosh can wait up to ten seconds for its UDP shutdown to be
        // acknowledged when the far machine is unreachable.
        const gint64 seconds = kterm_process_is(session, "mosh-client") ? 12 : 3;
        kill(-session, SIGHUP);
        ConnectionSwitch *pending = g_new0(ConnectionSwitch, 1);
        pending->command = connection_command(host->name, mosh);
        pending->deadline = g_get_monotonic_time() + seconds * G_USEC_PER_SEC;
        pending->source = g_timeout_add(100, switch_when_idle, terminal);
        g_object_set_data_full(G_OBJECT(terminal), "kterm-connection-switch", pending, connection_switch_free);
        panel_close();
        return;
    }
    if (kterm_terminal_foreground(kterm_get_terminal())) {
        // some other program (an editor, a pager): not ours to end
        gtk_label_set_text(GTK_LABEL(empty_label), "终端里有程序在运行，先退出它再连接。");
        gtk_widget_show(empty_label);
        return;
    }
    gchar *command = connection_command(host->name, mosh);
    kterm_type_command(kterm_get_terminal(), command);
    g_free(command);
    panel_close();
}

static void show_keys(void);

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
    g_free(chosen_key);
    if (host) {
        chosen_key = g_strdup(host->key);
    } else {
        // a new one: the key there is, if there is just one
        GPtrArray *keys = sshconf_keys();
        chosen_key = g_strdup(keys->len == 1 ? (const gchar *) g_ptr_array_index(keys, 0) : "");
        g_ptr_array_unref(keys);
    }
    show_keys();
    gtk_label_set_text(GTK_LABEL(form_message), "");
    gtk_button_set_label(GTK_BUTTON(delete_button), "删除");
    gtk_widget_set_visible(delete_button, host != NULL);
    panel_show_page(form_page);
    gtk_widget_grab_focus(entry_name);
}

/**
 * A machine found in the local network was picked: the form, filled in.
 * It is reached by its name there (vidy-macbookpro.local), whatever
 * address the router gives it; sshconf.c has kterm-mdns find it. One
 * already saved is edited instead.
 * @param button Calling widget, holding "host" and "port"
 */
static void found_cb(GtkWidget *button) {
    const gchar *address = g_object_get_data(G_OBJECT(button), "host");
    const gchar *port = g_object_get_data(G_OBJECT(button), "port");
    for (guint i = 0; hosts && i < hosts->len; i++) {
        const SshHost *saved = g_ptr_array_index(hosts, i);
        if (!g_ascii_strcasecmp(saved->host, address)) {
            show_form(saved);
            return;
        }
    }
    // its first label is the name to type: ssh vidy-macbookpro
    gchar *name = g_strndup(address, strcspn(address, "."));
    show_form(NULL);
    gtk_entry_set_text(GTK_ENTRY(entry_name), name);
    gtk_entry_set_text(GTK_ENTRY(entry_host), address);
    gtk_entry_set_text(GTK_ENTRY(entry_port), strcmp(port, "22") ? port : "");
    // the user name on that machine is the one thing not found
    gtk_widget_grab_focus(entry_user);
    g_free(name);
}

/**
 * The search is over: a row for each machine found
 * @param source kterm-mdns
 * @param result Its output: instance, host, port, address, tab separated
 * @param data Not used
 */
static void search_done(GObject *source, GAsyncResult *result, gpointer data) {
    UNUSED(data);
    searching = FALSE;
    gchar *output = NULL;
    GError *error = NULL;
    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, NULL, &error)) {
        gtk_label_set_text(GTK_LABEL(found_label), error->message);
        g_error_free(error);
        return;
    }
    gchar **lines = g_strsplit(output ? output : "", "\n", -1);
    guint count = 0;
    for (gchar **line = lines; *line; line++) {
        gchar **fields = g_strsplit(*line, "\t", 4);
        if (g_strv_length(fields) == 4 && count < ROWS_PER_PAGE) {
            gboolean saved = FALSE;
            for (guint i = 0; hosts && i < hosts->len && !saved; i++) {
                saved = !g_ascii_strcasecmp(((SshHost *) g_ptr_array_index(hosts, i))->host, fields[1]);
            }
            gchar *markup = g_markup_printf_escaped("<b>%s</b>   %s%s", fields[1], fields[0],
                                                    saved ? "   （已保存）" : "");
            GtkWidget *label = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(label), markup);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            g_free(markup);
            GtkWidget *row = gtk_button_new();
            gtk_widget_set_name(row, PANEL_BUTTON_NAME);
            gtk_widget_set_can_focus(row, FALSE);
            gtk_container_add(GTK_CONTAINER(row), label);
            g_object_set_data_full(G_OBJECT(row), "host", g_strdup(fields[1]), g_free);
            g_object_set_data_full(G_OBJECT(row), "port", g_strdup(fields[2]), g_free);
            g_signal_connect(row, "clicked", G_CALLBACK(found_cb), NULL);
            gtk_box_pack_start(GTK_BOX(found_rows), row, FALSE, FALSE, 0);
            count++;
        }
        g_strfreev(fields);
    }
    g_strfreev(lines);
    g_free(output);
    gtk_widget_show_all(found_rows);
    gtk_label_set_text(GTK_LABEL(found_label), count
                       ? "点一台机器，填上用户名保存。换了 IP 也能连上。"
                       : "没找到。Mac 要在 系统设置 → 通用 → 共享 里打开“远程登录”，并和 Kindle 连着同一个 Wi-Fi。");
}

/**
 * Search button callback: the machines of the local network that take ssh
 * connections (the _ssh._tcp service, which a Mac with remote login on
 * offers), asked by kterm-mdns
 */
static void search_cb(void) {
    panel_clear_box(found_rows);
    panel_show_page(found_page);
    if (searching) {
        return;
    }
    GError *error = NULL;
    GSubprocess *search = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                           &error, "kterm-mdns", "browse", "_ssh._tcp", NULL);
    if (!search) {
        gtk_label_set_text(GTK_LABEL(found_label), error->message);
        g_error_free(error);
        return;
    }
    searching = TRUE;
    gtk_label_set_text(GTK_LABEL(found_label), "正在查找局域网里的机器…");
    g_subprocess_communicate_utf8_async(search, NULL, NULL, search_done, NULL);
    g_object_unref(search);
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
 * @param with_mosh Whether to include the Mosh button
 * @return Row widget
 */
static GtkWidget * build_row(guint index, gboolean with_mosh) {
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
    if (with_mosh) {
        GtkWidget *mosh = panel_button_new("Mosh");
        g_object_set_data(G_OBJECT(mosh), "mosh", GINT_TO_POINTER(TRUE));
        g_signal_connect(mosh, "clicked", G_CALLBACK(connect_cb), GUINT_TO_POINTER(index));
        gtk_box_pack_start(GTK_BOX(row), mosh, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(row), edit, FALSE, FALSE, 0);
    return row;
}

/** Check whether the existing rows still describe the requested page. */
static gboolean rows_match(const GPtrArray *previous, guint count, gboolean with_mosh) {
    const guint previous_count = previous ? previous->len : 0;
    if (rendered_page_start != page_start || rendered_with_mosh != with_mosh || previous_count != count) {
        return FALSE;
    }
    for (guint i = page_start; i < count && i < page_start + ROWS_PER_PAGE; i++) {
        const SshHost *old = g_ptr_array_index(previous, i);
        const SshHost *current = g_ptr_array_index(hosts, i);
        if (g_strcmp0(old->name, current->name) || g_strcmp0(old->host, current->host)
            || g_strcmp0(old->user, current->user) || g_strcmp0(old->port, current->port)) {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * Read the saved connections and refresh the list before display.
 */
static void refresh_list(void) {
    gtk_widget_set_visible(disconnect_button, connection_session() != 0);
    GPtrArray *previous = hosts;
    gchar *path = sshconf_default_path();
    GError *error = NULL;
    hosts = sshconf_list(path, &error);
    g_free(path);

    const guint count = hosts ? hosts->len : 0;
    if (page_start >= count) {
        // the last connection of the last page was removed
        page_start = count ? ((count - 1) / ROWS_PER_PAGE) * ROWS_PER_PAGE : 0;
    }
    const gboolean with_mosh = count > 0 && mosh_available();
    if (!rows_match(previous, count, with_mosh)) {
        panel_clear_box(rows_box);
        guint shown = 0;
        for (guint i = page_start; i < count && i < page_start + ROWS_PER_PAGE; i++, shown++) {
            gtk_box_pack_start(GTK_BOX(rows_box), build_row(i, with_mosh), FALSE, FALSE, 0);
        }
        // Pad a short last page to keep the paging buttons in place.
        for (; count > ROWS_PER_PAGE && shown < ROWS_PER_PAGE; shown++) {
            GtkWidget *filler = panel_button_new(" ");
            gtk_widget_set_sensitive(filler, FALSE);
            gtk_widget_set_opacity(filler, 0);
            gtk_box_pack_start(GTK_BOX(rows_box), filler, FALSE, FALSE, 0);
        }
        gtk_widget_show_all(rows_box);
        rendered_page_start = page_start;
        rendered_with_mosh = with_mosh;
    }
    g_clear_pointer(&previous, g_ptr_array_unref);

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
}

static void show_list(void) {
    panel_show_page(list_page);
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
 * Write the form to the ssh configuration; a problem is shown in the form
 * @return The connection saved, NULL if it was not
 */
static SshHost * save_form(void) {
    gchar *name = entry_value(entry_name);
    gchar *address = entry_value(entry_host);
    gchar *user = entry_value(entry_user);
    gchar *port = entry_value(entry_port);
    SshHost *host = ssh_host_new(name, address, user, port, chosen_key);
    gchar *path = sshconf_default_path();
    GError *error = NULL;
    if (sshconf_save(path, editing_name, host, &error)) {
        D printf("panel: saved %s\n", host->name);
        // saved under this name now, should more be done in the form
        g_free(editing_name);
        editing_name = g_strdup(host->name);
    } else {
        gtk_label_set_text(GTK_LABEL(form_message), error->message);
        g_error_free(error);
        ssh_host_free(host);
        host = NULL;
    }
    g_free(path);
    g_free(name);
    g_free(address);
    g_free(user);
    g_free(port);
    return host;
}

/**
 * Save button callback: write the form to the ssh configuration
 */
static void save_cb(void) {
    SshHost *host = save_form();
    if (host) {
        ssh_host_free(host);
        show_list();
    }
}

/**
 * A key button: log in with it ("" for none, a password)
 * @param button Calling widget, holding "key"
 */
static void key_cb(GtkWidget *button) {
    g_free(chosen_key);
    chosen_key = g_strdup(g_object_get_data(G_OBJECT(button), "key"));
    show_keys();
}

/**
 * 生成密钥: a key for this Kindle, chosen at once
 */
static void new_key_cb(void) {
    GError *error = NULL;
    gchar *key = sshconf_new_key(&error);
    if (!key) {
        gtk_label_set_text(GTK_LABEL(form_message), error->message);
        g_error_free(error);
        return;
    }
    g_free(chosen_key);
    chosen_key = key;
    show_keys();
    gtk_label_set_text(GTK_LABEL(form_message), "密钥生成好了。点“装到电脑”，输一次那台电脑的密码，以后就不用了。");
}

/**
 * 装到电脑: save, then put the key's public half into authorized_keys on
 * that machine, typed into the terminal: its password is asked there once,
 * the key logs in after that. Its RemoteCommand (tmux, say) is left out.
 */
static void install_cb(void) {
    gchar *public_key = *chosen_key ? sshconf_public_key(chosen_key) : NULL;
    if (!public_key) {
        gtk_label_set_text(GTK_LABEL(form_message), "这个密钥旁边没有 .pub 公钥文件，装不了。");
        return;
    }
    if (connection_session() || kterm_terminal_foreground(kterm_get_terminal())) {
        gtk_label_set_text(GTK_LABEL(form_message), "终端里有程序在运行（或连着 SSH），先退出它再装。");
        g_free(public_key);
        return;
    }
    SshHost *host = save_form();
    if (!host) {
        g_free(public_key);
        return;
    }
    gchar *remote = g_strdup_printf("umask 077; mkdir -p ~/.ssh; k='%s'; "
                                    "grep -qxF \"$k\" ~/.ssh/authorized_keys 2>/dev/null"
                                    " || echo \"$k\" >> ~/.ssh/authorized_keys", public_key);
    gchar *quoted = g_shell_quote(remote);
    gchar *command = g_strdup_printf("ssh -o RemoteCommand=none -o RequestTTY=no %s %s"
                                     " && echo '公钥装好了：以后连 %s 不用密码'", host->name, quoted, host->name);
    kterm_type_command(kterm_get_terminal(), command);
    g_free(command);
    g_free(quoted);
    g_free(remote);
    g_free(public_key);
    ssh_host_free(host);
    panel_close();
}

/**
 * @param label Text
 * @param key Key it chooses, NULL for another kind of button
 * @return A button of the key row, drawn black when its key is chosen
 */
static GtkWidget * key_button(const gchar *label, const gchar *key) {
    GtkWidget *button = panel_button_new(label);
    if (key) {
        g_object_set_data_full(G_OBJECT(button), "key", g_strdup(key), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(key_cb), NULL);
        if (!strcmp(key, chosen_key)) {
            gtk_style_context_add_class(gtk_widget_get_style_context(button), "selected");
        }
    }
    gtk_box_pack_start(GTK_BOX(key_box), button, FALSE, FALSE, 0);
    return button;
}

/**
 * Fill the key row: the keys there are (and the one in the configuration,
 * should it be elsewhere), 不用; 生成密钥 without a key; 装到电脑 with one
 * chosen
 */
static void show_keys(void) {
    panel_clear_box(key_box);
    GPtrArray *keys = sshconf_keys();
    gboolean listed = !*chosen_key;
    for (guint i = 0; i < keys->len; i++) {
        listed = listed || !strcmp(chosen_key, g_ptr_array_index(keys, i));
    }
    if (!listed) {
        g_ptr_array_add(keys, g_strdup(chosen_key));
    }
    for (guint i = 0; i < keys->len; i++) {
        const gchar *key = g_ptr_array_index(keys, i);
        gchar *name = g_path_get_basename(key);
        key_button(name, key);
        g_free(name);
    }
    key_button("不用", "");
    if (!keys->len) {
        g_signal_connect(key_button("生成密钥", NULL), "clicked", G_CALLBACK(new_key_cb), NULL);
    }
    if (*chosen_key) {
        g_signal_connect(key_button("装到电脑", NULL), "clicked", G_CALLBACK(install_cb), NULL);
    }
    g_ptr_array_unref(keys);
    gtk_widget_show_all(key_box);
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
 * Build the list page
 * @return Page widget
 */
static GtkWidget * build_list_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING);
    gtk_widget_set_halign(header, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);
    disconnect_button = panel_button_new("断开");
    g_signal_connect(disconnect_button, "clicked", G_CALLBACK(disconnect_cb), NULL);
    gtk_box_pack_start(GTK_BOX(header), disconnect_button, FALSE, FALSE, 0);
    prev_button = panel_button_new("‹");
    next_button = panel_button_new("›");
    page_label = gtk_label_new(NULL);
    GtkWidget *search = panel_button_new("查找");
    GtkWidget *add = panel_button_new("新建");
    g_signal_connect(prev_button, "clicked", G_CALLBACK(page_cb), GINT_TO_POINTER(-1));
    g_signal_connect(next_button, "clicked", G_CALLBACK(page_cb), GINT_TO_POINTER(1));
    g_signal_connect(search, "clicked", G_CALLBACK(search_cb), NULL);
    g_signal_connect(add, "clicked", G_CALLBACK(new_cb), NULL);
    gtk_box_pack_start(GTK_BOX(header), prev_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), page_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), next_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), search, FALSE, FALSE, 0);
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
 * Build the page of the machines found in the local network
 * @return Page widget
 */
static GtkWidget * build_found_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    GtkWidget *header = panel_page_header(page, "局域网里的机器", list_page);
    GtkWidget *again = panel_button_new("再找");
    g_signal_connect(again, "clicked", G_CALLBACK(search_cb), NULL);
    gtk_box_pack_start(GTK_BOX(header), again, FALSE, FALSE, 0);
    found_rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    found_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(found_label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(found_label), TRUE);
    gtk_box_pack_start(GTK_BOX(page), found_rows, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), found_label, FALSE, FALSE, 0);
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
    // the key to log in with, not a field typed into
    GtkWidget *key_label = gtk_label_new("密钥");
    gtk_label_set_xalign(GTK_LABEL(key_label), 0);
    key_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_grid_attach(GTK_GRID(grid), key_label, 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), key_box, 1, 4, 1, 1);
    chosen_key = g_strdup("");
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

/** Add the connection list, discovery and editor to the connections tab. */
void connpage_init(void) {
    list_page = build_list_page();
    form_page = build_form_page();
    found_page = build_found_page();
    panel_add_tab(PANEL_TAB_CONNECTIONS, list_page, refresh_list);
    panel_add_page(found_page, FALSE, PANEL_TAB_CONNECTIONS);
    panel_add_page(form_page, TRUE, PANEL_TAB_CONNECTIONS);
}
#endif
