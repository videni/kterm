/* voicebar.c
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
 * Where text from the phone (voice.c) goes. It must not pull the reader
 * out of what they are looking at: scrolled back, in kterm's own history
 * or in tmux's copy mode, text typed into the terminal would not reach
 * the program at all (tmux takes every letter for a copy mode command:
 * "q" leaves it, and what follows lands half in the input box).
 *
 * So text is kept in a box, a notice at the top of the terminal (notice.c),
 * shown until it is put in with one of its buttons, those of the phone
 * page (run: "send and Enter"; "send": without Enter) or Enter on the
 * keyboard, which leave the scrolled back view first; "clear" throws it
 * away. "Send" from the phone always goes to the box; "send and Enter"
 * goes straight in when the terminal shows its input line, to the box
 * (marked to be run) when it is scrolled back, or when tmux cannot be
 * asked (tmux.c).
 *
 * Text from the phone and taps on the box are handled one after the
 * other, in the order they came: each may have to wait for tmux.
 */

#include <string.h>
#include <vte/vte.h>
#include "config.h"
#include "icons.h"
#include "keyboard.h"
#include "kterm.h"
#include "notice.h"
#include "panel.h"
#include "tmux.h"
#include "voicebar.h"

/** Something to do, waiting for what came before it */
typedef struct {
    /** Text from the phone; NULL: a tap putting the box's text in */
    gchar *text;
    /** Enter after it */
    gboolean run;
    /** For text from the phone: told what was done */
    VoiceReply reply;
    GWeakRef terminal;
    gpointer data;
} Incoming;

/** Most lines of the text shown: more loses its middle */
#define VOICEBAR_LINES 4

static GtkWidget *bar = NULL;
/** Enter: out of the scrolled back view, put in and run */
static GtkWidget *run_button = NULL;
/** Text waiting in the box */
static GString *staged = NULL;
/** Enter after it when it is put in */
static gboolean staged_run = FALSE;
/** What waits while tmux is asked about what came before (Incoming) */
static GQueue incoming = G_QUEUE_INIT;
/** tmux is being asked */
static gboolean probing = FALSE;

/**
 * Type text into the terminal
 * @param text Text, may be empty
 * @param run Enter after it
 */
static void type_text(const gchar *text, gboolean run) {
    GtkWidget *terminal = kterm_get_terminal();
    if (*text) {
        if (strchr(text, '\n')) {
            // several lines: pasted, so that a shell does not run each line
            // as it comes and Claude Code keeps them in one message
            vte_terminal_paste_text(VTE_TERMINAL(terminal), text);
        } else {
            vte_terminal_feed_child(VTE_TERMINAL(terminal), text, -1);
        }
    }
    if (run) {
        vte_terminal_feed_child(VTE_TERMINAL(terminal), "\r", 1);
    }
}

/**
 * Show the box with what waits in it, or take it away
 * @param note Shown in place of the text for a while (what is going on, a
 *  problem), NULL for the text
 */
static void show_bar(const gchar *note) {
    if (!bar) {
        return;
    }
    if (!staged->len) {
        gtk_widget_hide(bar);
        return;
    }
    // the phone asked for Enter: that button is the one to press
    GtkStyleContext *style = gtk_widget_get_style_context(run_button);
    if (staged_run) {
        gtk_style_context_add_class(style, "primary");
    } else {
        gtk_style_context_remove_class(style, "primary");
    }
    D printf("voicebar: show \"%s\"\n", staged->str);
    notice_set_text(bar, note ? note : staged->str);
    notice_show(bar);
}

static void clear_staged(void) {
    g_string_truncate(staged, 0);
    staged_run = FALSE;
    show_bar(NULL);
}

static gboolean incoming_is_current(Incoming *in) {
    GtkWidget *terminal = g_weak_ref_get(&in->terminal);
    const gboolean current = terminal && terminal == kterm_get_terminal();
    g_clear_object(&terminal);
    return current;
}

static void incoming_free(Incoming *in) {
    g_weak_ref_clear(&in->terminal);
    g_free(in->text);
    g_free(in);
}

static void process_incoming(void);

/**
 * tmux answered for text from the phone to be run
 */
static void received_where(TmuxWhere where, gpointer data) {
    Incoming *in = data;
    probing = FALSE;
    // with what already waits in the box: it came first
    g_string_append(staged, in->text);
    if (where == TMUX_AT_INPUT && incoming_is_current(in)) {
        type_text(staged->str, TRUE);
        clear_staged();
        in->reply("ran", in->data);
    } else {
        staged_run = TRUE;
        show_bar(where == TMUX_UNKNOWN ? "查不到 tmux 是不是在往上翻，先放在这里" : NULL);
        in->reply("staged-run", in->data);
    }
    incoming_free(in);
    process_incoming();
}

/**
 * tmux answered for a tap putting the box's text in, having been told to
 * leave its copy mode
 */
static void put_in_where(TmuxWhere where, gpointer data) {
    Incoming *in = data;
    probing = FALSE;
    if (!incoming_is_current(in)) {
        show_bar(NULL);
    } else if (where == TMUX_SCROLLED) {
        // tmux did not leave copy mode: typed now, the text would be taken
        // for copy mode commands. A tap tries again.
        show_bar("tmux 没能退出往上翻。手动退出后，再点一次");
    } else {
        // not known (tmux could not be asked): put in, as tapped for
        type_text(staged->str, in->run);
        clear_staged();
    }
    incoming_free(in);
    process_incoming();
}

/**
 * Handle what waits, in order, until something has to wait for tmux
 */
static void process_incoming(void) {
    Incoming *in = NULL;
    while (!probing && (in = g_queue_pop_head(&incoming))) {
        if (!in->text) {
            // a tap: out of the scrolled back view, then in
            if (!staged->len || !incoming_is_current(in)) {
                incoming_free(in);
                continue;
            }
            probing = TRUE;
            show_bar("正在放入…");
            tmux_ask_where(TRUE, put_in_where, in);
        } else if (!in->run || !incoming_is_current(in)) {
            g_string_append(staged, in->text);
            staged_run |= in->run;
            show_bar(NULL);
            in->reply(in->run ? "staged-run" : "staged", in->data);
            incoming_free(in);
        } else {
            probing = TRUE;
            tmux_ask_where(FALSE, received_where, in);
        }
    }
}

/**
 * Text from the phone
 * @param text Text, may be empty
 * @param run TRUE for Enter after it ("send and Enter")
 * @param reply Told what was done: "staged" (in the box), "ran" (typed and
 *  entered), "staged-run" (in the box to be run: scrolled back)
 * @param data For reply
 */
void voicebar_receive(const gchar *text, gboolean run, VoiceReply reply, gpointer data) {
    Incoming *in = g_new0(Incoming, 1);
    g_weak_ref_init(&in->terminal, kterm_get_terminal());
    in->text = g_strdup(text);
    in->run = run;
    in->reply = reply;
    in->data = data;
    g_queue_push_tail(&incoming, in);
    process_incoming();
}

/**
 * Put the text in the box into the terminal: out of the scrolled back
 * view first. After what is under way, if anything is.
 * @param run Enter after it
 */
static void put_in(gboolean run) {
    if (!staged->len) {
        return;
    }
    Incoming *in = g_new0(Incoming, 1);
    g_weak_ref_init(&in->terminal, kterm_get_terminal());
    in->run = run;
    g_queue_push_tail(&incoming, in);
    process_incoming();
}

/**
 * Enter on the keyboard
 * @return TRUE if it put the text in the box in, and is not to be typed
 */
gboolean voicebar_take_enter(void) {
    if (!staged || !staged->len || panel_wants_keys()) {
        return FALSE;
    }
    // Enter if the phone asked for it ("send and Enter")
    put_in(staged_run);
    return TRUE;
}

/**
 * Throw away what waits in the box
 */
void voicebar_clear(void) {
    if (staged) {
        clear_staged();
    }
}

static void put_in_cb(GtkWidget *button, gpointer run) {
    UNUSED(button);
    put_in(GPOINTER_TO_INT(run));
}

/**
 * Build the box
 */
void voicebar_init(void) {
    staged = g_string_new(NULL);
    // a tap beside the buttons does nothing: the box stays until its text
    // is put in or thrown away (its close button, the phone's 清空)
    bar = notice_new(VOICEBAR_LINES, NULL, voicebar_clear);
    // the buttons of the phone page, same signs
    run_button = notice_action(bar, ICON_PLAY, "放入并回车");     // 发送并回车
    GtkWidget *put = notice_action(bar, ICON_SEND, "只放入");      // 发送
    g_signal_connect(run_button, "clicked", G_CALLBACK(put_in_cb), GINT_TO_POINTER(TRUE));
    g_signal_connect(put, "clicked", G_CALLBACK(put_in_cb), GINT_TO_POINTER(FALSE));
}
