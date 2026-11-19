/* tmuxpick.c
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
 * Once an ssh session has the terminal, whether started from the panel or
 * typed by hand: if tmux on the far machine has sessions, and the login
 * did not go into one by itself, the panel offers them, the one used last
 * first. A tap types "tmux attach" for it; "新建会话" a new one; "不用
 * tmux" leaves the shell as it is. Asked once for each ssh session, only
 * while "tmux 集成" is on.
 *
 * The question goes over the session's own connection when it was opened
 * from the panel (connpage.c), else over one of its own (a key login).
 * Right after the ssh client starts, the login is not done yet (a
 * password being typed): it is asked again for a while.
 */

#include <string.h>
#include <vte/vte.h>
#include "config.h"
#include "kterm.h"
#include "panel.h"
#include "tmux.h"
#include "tmuxpick.h"

/** First question this long after the ssh client took the terminal, ms */
#define FIRST_DELAY_MS 1500
/** Asked again this often while there is no answer, ms ... */
#define RETRY_MS 2000
/** ... this many times: a minute for a password */
#define RETRIES 30
/** Sessions offered; the panel has room for this many */
#define SESSIONS_SHOWN 5

static GtkWidget *terminal_widget = NULL;
static GtkWidget *page = NULL;
static GtkWidget *rows = NULL;
/** Process group that had the terminal at the last look */
static pid_t seen_owner = 0;
/** The ssh session being asked about, 0 for none */
static pid_t asking = 0;
static guint tries = 0;
static guint timer = 0;

static void ask(void);

/**
 * Type into the terminal, over anything half typed
 */
static void type(const gchar *command) {
    gchar *line = g_strconcat("\x05\x15", command, "\r", NULL);
    vte_terminal_feed_child(VTE_TERMINAL(terminal_widget), line, -1);
    g_free(line);
}

static void attach_cb(GtkWidget *button, gpointer data) {
    UNUSED(data);
    const gchar *name = g_object_get_data(G_OBJECT(button), "session");
    gchar *quoted = g_shell_quote(name);
    gchar *command = g_strconcat("tmux attach -t ", quoted, NULL);
    panel_close();
    type(command);
    g_free(command);
    g_free(quoted);
}

static void new_cb(void) {
    panel_close();
    type("tmux new-session");
}

/**
 * @param seconds Since a session was used
 * @return How long ago, in words, newly allocated
 */
static gchar * ago(gint64 seconds) {
    if (seconds < 60) {
        return g_strdup("刚刚用过");
    }
    if (seconds < 3600) {
        return g_strdup_printf("%d 分钟前", (int) (seconds / 60));
    }
    if (seconds < 86400) {
        return g_strdup_printf("%d 小时前", (int) (seconds / 3600));
    }
    return g_strdup_printf("%d 天前", (int) (seconds / 86400));
}

/**
 * Show the sessions in the panel
 * @param sessions TmuxSession, the one used last first
 */
static void offer(GPtrArray *sessions) {
    GList *old = gtk_container_get_children(GTK_CONTAINER(rows));
    for (GList *l = old; l; l = l->next) {
        gtk_widget_destroy(l->data);
    }
    g_list_free(old);
    for (guint i = 0; i < sessions->len && i < SESSIONS_SHOWN; i++) {
        const TmuxSession *session = g_ptr_array_index(sessions, i);
        GtkWidget *button = panel_button_new("");
        gchar *when = ago(session->idle);
        gchar *markup = g_markup_printf_escaped("<b>%s</b>    %u 个窗口 · %s%s", session->name, session->windows,
                                                when, session->attached ? " · 别处正连着" : "");
        GtkWidget *label = gtk_bin_get_child(GTK_BIN(button));
        gtk_label_set_markup(GTK_LABEL(label), markup);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        g_free(markup);
        g_free(when);
        g_object_set_data_full(G_OBJECT(button), "session", g_strdup(session->name), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(attach_cb), NULL);
        gtk_box_pack_start(GTK_BOX(rows), button, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(rows);
    panel_open();
    panel_show_page(page);
}

/**
 * The ssh session is the same one still
 */
static gboolean still_asking(void) {
    return asking && kterm_terminal_owner() == asking && tmux_enabled();
}

static gboolean retry_cb(gpointer data) {
    UNUSED(data);
    timer = 0;
    ask();
    return G_SOURCE_REMOVE;
}

static void answer_cb(TmuxAsked asked, GPtrArray *sessions, gpointer data) {
    UNUSED(data);
    if (!still_asking()) {
        asking = 0;
        return;
    }
    if (asked == TMUX_NOT_ASKED && ++tries < RETRIES) {
        // the login is not done yet, or not a key login
        timer = g_timeout_add(RETRY_MS, retry_cb, NULL);
        return;
    }
    asking = 0;
    // nothing to choose from: no tmux there, or no session; already in one
    if (asked == TMUX_LISTED && sessions->len && !panel_wants_keys()) {
        offer(sessions);
    }
}

static void ask(void) {
    if (!still_asking() || !tmux_list_sessions(answer_cb, NULL)) {
        asking = 0;
    }
}

/**
 * The terminal changed: has an ssh client taken it?
 */
static void contents_cb(void) {
    const pid_t owner = kterm_terminal_owner();
    if (owner == seen_owner) {
        return;
    }
    seen_owner = owner;
    if (!tmux_enabled()) {
        return;
    }
    gchar *name = kterm_process_name(owner);
    const gboolean ssh = name && !strcmp(name, "ssh");
    g_free(name);
    if (!ssh) {
        return;
    }
    asking = owner;
    tries = 0;
    if (timer) {
        g_source_remove(timer);
    }
    timer = g_timeout_add(FIRST_DELAY_MS, retry_cb, NULL);
}

/**
 * Add the page of sessions to the panel and watch for ssh sessions
 * @param terminal Terminal
 */
void tmuxpick_init(GtkWidget *terminal) {
    terminal_widget = terminal;
    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    panel_page_header(page, "tmux 会话", NULL);
    rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    gtk_box_pack_start(GTK_BOX(page), rows, FALSE, FALSE, 0);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING);
    gtk_box_set_homogeneous(GTK_BOX(actions), TRUE);
    GtkWidget *create = panel_button_new("新建会话");
    GtkWidget *none = panel_button_new("不用 tmux");
    g_signal_connect(create, "clicked", G_CALLBACK(new_cb), NULL);
    g_signal_connect(none, "clicked", G_CALLBACK(panel_close), NULL);
    gtk_box_pack_start(GTK_BOX(actions), create, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(actions), none, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), actions, FALSE, FALSE, 0);
    panel_add_page(page, FALSE);
    g_signal_connect(terminal, "contents-changed", G_CALLBACK(contents_cb), NULL);
}
