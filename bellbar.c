/* bellbar.c
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
 * The bell (BEL) of the terminal: a program asking for attention, Claude
 * Code done or waiting, a tmux window's bell passed on. Shown as a notice
 * at the top of the terminal (notice.c), in kterm's own window: seen once
 * kterm is looked at, not over other applications. It says what rang: the
 * tmux window and its pane's title (asked of tmux, tmux.c), or the
 * terminal's title. Its 去看 button shows that tmux window; a tap
 * elsewhere takes it away. A bell while it is up only changes what it says.
 */

#include <vte/vte.h>
#include "config.h"
#include "bellbar.h"
#include "icons.h"
#include "kterm.h"
#include "notice.h"
#include "tmux.h"

/** Most lines of what rang */
#define BELL_LINES 2

static GtkWidget *bar = NULL;
/** Shows the window that rang */
static GtkWidget *go_button = NULL;
/** tmux window that rang, -1 if not known */
static gint rang = -1;
/** tmux is being asked which window rang: more bells wait for it */
static gboolean asking = FALSE;

void bellbar_hide(void) {
    gtk_widget_hide(bar);
}

static void go_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    UNUSED(data);
    tmux_select_window(rang);
    bellbar_hide();
}

/**
 * Show the bar
 * @param window tmux window that rang, -1 if not known
 * @param text What rang
 */
static void show_bar(gint window, const gchar *text) {
    D printf("bell: %s\n", text);
    rang = window;
    notice_set_text(bar, text);
    gtk_widget_set_visible(go_button, window >= 0);
    notice_show(bar);
}

/**
 * What rang without tmux: the terminal's title, if it has one
 */
static void show_own(void) {
    gchar *title = kterm_terminal_title();
    show_bar(-1, title ? title : "终端响铃");
    g_free(title);
}

static void bell_done(gint index, const gchar *label, gpointer data) {
    GWeakRef *ref = data;
    GtkWidget *terminal = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);
    asking = FALSE;
    const gboolean selected = terminal && terminal == kterm_get_terminal();
    g_clear_object(&terminal);
    if (!selected) {
        return;
    }
    if (index < 0) {
        show_own();
        return;
    }
    gchar *text = label && *label ? g_strdup_printf("窗口 %d · %s", index, label)
                                  : g_strdup_printf("窗口 %d", index);
    show_bar(index, text);
    g_free(text);
}

static void bell_cb(VteTerminal *terminal, gpointer data) {
    UNUSED(data);
    if (asking || GTK_WIDGET(terminal) != kterm_get_terminal()) {
        return;
    }
    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, terminal);
    asking = tmux_ask_bell(bell_done, ref);
    if (!asking) {
        g_weak_ref_clear(ref);
        g_free(ref);
        show_own();
    }
}

/**
 * @param terminal Terminal widget, whose bell is shown
 */
void bellbar_attach(GtkWidget *terminal) {
    // no beep: the Kindle has no speaker
    vte_terminal_set_audible_bell(VTE_TERMINAL(terminal), FALSE);
    g_signal_connect(terminal, "bell", G_CALLBACK(bell_cb), NULL);
}

void bellbar_init(void) {
    bar = notice_new(BELL_LINES, bellbar_hide, bellbar_hide);
    notice_lead(bar, icon_new(ICON_BELL, 32));
    go_button = notice_action(bar, ICON_GO, "去看");
    g_signal_connect(go_button, "clicked", G_CALLBACK(go_cb), NULL);
}
