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
 * So text is kept in a box above the shortcut bar, shown until it is put
 * in with one of its buttons, those of the phone page ("send": without
 * Enter; "send and Enter") or Enter on the keyboard, which leave the
 * scrolled back view first; "clear" throws it away. "Send" from the phone
 * always goes to the box;
 * "send and Enter" goes straight in when the terminal shows its input
 * line, to the box (marked to be run) when it is scrolled back.
 *
 * Scrolled back where? kterm's own history it knows. tmux's copy mode is
 * asked of tmux, when text comes, not watched: the same ssh connection
 * runs one more command (ControlMaster, about 0.1 s on the Kindle), as
 * rootshell does. The tmux client of this terminal is the one on the
 * login the Kindle's address has there (who). If tmux cannot be asked
 * (a password login), text from the phone waits in the box; a tap on
 * the box puts it in all the same.
 *
 * Text from the phone and taps on the box are handled one after the
 * other, in the order they came: each may have to wait for tmux.
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <vte/vte.h>
#include "config.h"
#include "icons.h"
#include "keyboard.h"
#include "kterm.h"
#include "panel.h"
#include "voicebar.h"

#define VOICEBAR_NAME "ktermVoiceBar"
#define VOICEBAR_BUTTON_NAME "ktermVoiceButton"
#define VOICEBAR_ICON_PX 32
/** Room between the box and the sides of the screen, the shortcut bar, px */
#define VOICEBAR_MARGIN 12
/** tmux not answering this long counts as no answer, ms */
#define PROBE_TIMEOUT_MS 6000
/** The connection to the far machine is kept this long after the last
 *  question, s */
#define CONTROL_PERSIST "600"

/** Where the terminal is */
typedef enum {
    /** showing its input line */
    AT_INPUT,
    /** scrolled back */
    SCROLLED,
    /** not known: tmux could not be asked */
    UNKNOWN,
} Where;

typedef void (*ProbeDone)(Where where, gpointer data);

/** Something to do, waiting for what came before it */
typedef struct {
    /** Text from the phone; NULL: a tap putting the box's text in */
    gchar *text;
    /** Enter after it */
    gboolean run;
    /** For text from the phone: told what was done */
    VoiceReply reply;
    gpointer data;
} Incoming;

static GtkWidget *terminal_widget = NULL;
static GtkWidget *shortbar_widget = NULL;
static GtkWidget *bar = NULL;
/** The text, on one line */
static GtkWidget *text_area = NULL;
/** Enter: out of the scrolled back view, put in and run */
static GtkWidget *run_button = NULL;
/** Shown in place of the text for a while, NULL for none */
static gchar *message = NULL;
/** Text waiting in the box */
static GString *staged = NULL;
/** Enter after it when it is put in */
static gboolean staged_run = FALSE;
/** What waits while tmux is asked about what came before (Incoming) */
static GQueue incoming = G_QUEUE_INIT;
/** tmux is being asked */
static gboolean probing = FALSE;

/*
 * Run on the machine tmux is on: finds this terminal's tmux client and
 * prints its pane and whether that pane is in a mode (copy mode: scrolled
 * back), "none" without tmux clients, "unknown" if there are clients but
 * none is this terminal's. With "cancel", a pane in a mode leaves it.
 */
static const gchar PROBE_SCRIPT[] =
    "PATH=\"$PATH:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin\"\n"
    "command -v tmux >/dev/null 2>&1 || { echo none; exit 0; }\n"
    "clients=$(tmux list-clients -F '#{client_activity} #{client_tty} #{pane_id} #{pane_in_mode}' 2>/dev/null)\n"
    "[ -n \"$clients\" ] || { echo none; exit 0; }\n"
    "if [ -n \"$KTERM_TTY\" ]; then ttys=$KTERM_TTY\n"
    "else ip=${SSH_CONNECTION%% *}; ttys=$(who | awk -v h=\"($ip)\" '$NF == h { print \"/dev/\" $2 }'); fi\n"
    "best=$(for t in $ttys; do printf '%s\\n' \"$clients\" | awk -v t=\"$t\" '$2 == t'; done | sort -rn | head -n 1)\n"
    "[ -n \"$best\" ] || { echo unknown; exit 0; }\n"
    "set -- $best\n"
    "if [ \"$ACTION\" = cancel ] && [ \"$4\" = 1 ]; then tmux send-keys -t \"$3\" -X cancel && echo \"$3 0\"; exit 0; fi\n"
    "echo \"$3 $4\"\n";

/**
 * The ssh command that asks the far machine, built from the client that
 * has the terminal: its destination and the options that say how to get
 * there (port, user, key, jump host, config); not its forwardings, its
 * remote command or its terminal.
 * @param pid ssh client
 * @return Arguments, NULL if they cannot be read
 */
static GPtrArray * probe_command(pid_t pid, const gchar *action) {
    gchar *path = g_strdup_printf("/proc/%d/cmdline", (int) pid);
    gchar *raw = NULL;
    gsize size = 0;
    const gboolean read = g_file_get_contents(path, &raw, &size, NULL);
    g_free(path);
    if (!read || !size) {
        g_free(raw);
        return NULL;
    }
    GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(args, g_strdup("ssh"));
    // first: ssh takes the first value it is given for an option
    static const gchar *own[] = {
        "BatchMode=yes", "RemoteCommand=none", "RequestTTY=no", "ConnectTimeout=5",
        "ControlMaster=auto", "ControlPath=/tmp/kterm-ssh-%C", "ControlPersist=" CONTROL_PERSIST,
        "ClearAllForwardings=yes", "LogLevel=ERROR",
    };
    for (guint i = 0; i < G_N_ELEMENTS(own); i++) {
        g_ptr_array_add(args, g_strdup("-o"));
        g_ptr_array_add(args, g_strdup(own[i]));
    }
    const gchar *destination = NULL;
    const gchar *end = raw + size;
    const gchar *arg = raw + strlen(raw) + 1;
    while (arg < end && !destination) {
        const gchar *next = arg + strlen(arg) + 1;
        if (arg[0] != '-' || !arg[1]) {
            destination = arg;
            break;
        }
        // options that take a value, kept or not
        static const gchar *with_value = "BbcDEeFIiJLlmOopQRSWw";
        static const gchar *kept = "cFiJlmop";
        for (const gchar *c = arg + 1; *c; c++) {
            if (strchr(with_value, *c)) {
                const gchar *value = c[1] ? c + 1 : (next < end ? next : NULL);
                if (strchr(kept, *c) && value) {
                    g_ptr_array_add(args, g_strdup_printf("-%c", *c));
                    g_ptr_array_add(args, g_strdup(value));
                }
                if (!c[1] && next < end) {
                    next += strlen(next) + 1;
                }
                break;
            }
            if (strchr("46C", *c)) {
                g_ptr_array_add(args, g_strdup_printf("-%c", *c));
            }
        }
        arg = next;
    }
    if (!destination) {
        g_ptr_array_unref(args);
        g_free(raw);
        return NULL;
    }
    g_ptr_array_add(args, g_strdup(destination));
    g_ptr_array_add(args, g_strdup_printf("ACTION=%s sh -s", action));
    g_free(raw);
    return args;
}

/** A question to tmux under way */
typedef struct {
    ProbeDone done;
    gpointer data;
    guint timeout;
    GSubprocess *process;
} Probe;

static gboolean probe_timeout(gpointer data) {
    Probe *probe = data;
    probe->timeout = 0;
    g_subprocess_force_exit(probe->process);
    return G_SOURCE_REMOVE;
}

static void probe_finished(GObject *source, GAsyncResult *result, gpointer data) {
    Probe *probe = data;
    if (probe->timeout) {
        g_source_remove(probe->timeout);
    }
    gchar *output = NULL;
    gchar *errors = NULL;
    Where where = UNKNOWN;
    // only the output tells: kterm leaves its children to the kernel
    // (SIGCHLD ignored), so their exit status is never known
    if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, &errors, NULL) && output) {
        g_strstrip(output);
        D printf("voicebar: tmux says \"%s\"\n", output);
        if (!strcmp(output, "none")) {
            // no tmux there: the history is kterm's own
            where = kterm_terminal_scrolled() ? SCROLLED : AT_INPUT;
        } else if (g_str_has_suffix(output, " 0")) {
            where = AT_INPUT;
        } else if (g_str_has_suffix(output, " 1")) {
            where = SCROLLED;
        }
    } else {
        D printf("voicebar: tmux not asked: %s\n", errors ? errors : "");
    }
    g_free(output);
    g_free(errors);
    g_object_unref(probe->process);
    probe->done(where, probe->data);
    g_free(probe);
}

/**
 * Find out whether the terminal is scrolled back
 * @param cancel TRUE to leave tmux's copy mode as well, if it is in it
 * @param done Called with the answer
 * @param data For done
 */
static void probe_where(gboolean cancel, ProbeDone done, gpointer data) {
    // what has the terminal: the shell, or what was started from it, or
    // the program kterm runs in place of a shell
    VtePty *pty = vte_terminal_get_pty(VTE_TERMINAL(terminal_widget));
    const pid_t pid = kterm_terminal_owner();
    gchar *name = kterm_process_name(pid);
    const gchar *action = cancel ? "cancel" : "query";
    GPtrArray *args = NULL;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE
                                                              | G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                                              | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    if (name && !strcmp(name, "ssh")) {
        args = probe_command(pid, action);
    } else if (name && g_str_has_prefix(name, "tmux")) {
        // tmux here: its client is on this terminal's tty
        const char *tty = pty ? ptsname(vte_pty_get_fd(pty)) : NULL;
        if (tty) {
            g_subprocess_launcher_setenv(launcher, "KTERM_TTY", tty, TRUE);
            g_subprocess_launcher_setenv(launcher, "ACTION", action, TRUE);
            args = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(args, g_strdup("sh"));
            g_ptr_array_add(args, g_strdup("-s"));
        }
    }
    g_free(name);
    if (!args) {
        g_object_unref(launcher);
        if (cancel) {
            kterm_terminal_to_bottom();
        }
        done(cancel || !kterm_terminal_scrolled() ? AT_INPUT : SCROLLED, data);
        return;
    }
    g_ptr_array_add(args, NULL);
    GError *error = NULL;
    Probe *probe = g_new0(Probe, 1);
    probe->done = done;
    probe->data = data;
    probe->process = g_subprocess_launcher_spawnv(launcher, (const gchar * const *) args->pdata, &error);
    g_object_unref(launcher);
    g_ptr_array_unref(args);
    if (!probe->process) {
        D printf("voicebar: %s\n", error->message);
        g_error_free(error);
        g_free(probe);
        done(UNKNOWN, data);
        return;
    }
    probe->timeout = g_timeout_add(PROBE_TIMEOUT_MS, probe_timeout, probe);
    g_subprocess_communicate_utf8_async(probe->process, PROBE_SCRIPT, NULL, probe_finished, probe);
}

/**
 * Type text into the terminal
 * @param text Text, may be empty
 * @param run Enter after it
 */
static void type_text(const gchar *text, gboolean run) {
    if (*text) {
        if (strchr(text, '\n')) {
            // several lines: pasted, so that a shell does not run each line
            // as it comes and Claude Code keeps them in one message
            vte_terminal_paste_text(VTE_TERMINAL(terminal_widget), text);
        } else {
            vte_terminal_feed_child(VTE_TERMINAL(terminal_widget), text, -1);
        }
    }
    if (run) {
        vte_terminal_feed_child(VTE_TERMINAL(terminal_widget), "\r", 1);
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
    g_free(message);
    message = g_strdup(note);
    // the phone asked for Enter: that button is the one to press
    GtkStyleContext *style = gtk_widget_get_style_context(run_button);
    if (staged_run) {
        gtk_style_context_add_class(style, "primary");
    } else {
        gtk_style_context_remove_class(style, "primary");
    }
    D printf("voicebar: show \"%s\"\n", staged->str);
    gtk_widget_show(bar);
    gtk_widget_queue_draw(text_area);
    gtk_widget_queue_resize(bar);
}

static void clear_staged(void) {
    g_string_truncate(staged, 0);
    staged_run = FALSE;
    show_bar(NULL);
}

static void incoming_free(Incoming *in) {
    g_free(in->text);
    g_free(in);
}

static void process_incoming(void);

/**
 * tmux answered for text from the phone to be run
 */
static void received_where(Where where, gpointer data) {
    Incoming *in = data;
    probing = FALSE;
    // with what already waits in the box: it came first
    g_string_append(staged, in->text);
    if (where == AT_INPUT) {
        type_text(staged->str, TRUE);
        clear_staged();
        in->reply("ran", in->data);
    } else {
        staged_run = TRUE;
        show_bar(where == UNKNOWN ? "查不到 tmux 是不是在往上翻，先放在这里" : NULL);
        in->reply("staged-run", in->data);
    }
    incoming_free(in);
    process_incoming();
}

/**
 * tmux answered for a tap putting the box's text in, having been told to
 * leave its copy mode
 */
static void put_in_where(Where where, gpointer data) {
    Incoming *in = data;
    probing = FALSE;
    if (where == SCROLLED) {
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
            if (!staged->len) {
                incoming_free(in);
                continue;
            }
            probing = TRUE;
            show_bar("正在放入…");
            probe_where(TRUE, put_in_where, in);
        } else if (!in->run) {
            g_string_append(staged, in->text);
            show_bar(NULL);
            in->reply("staged", in->data);
            incoming_free(in);
        } else {
            probing = TRUE;
            probe_where(FALSE, received_where, in);
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

/**
 * A touch on the box: taken, so that it does not reach the terminal below;
 * the buttons act, the rest does nothing (the box stays until its text is
 * put in)
 */
static gboolean bar_event(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    UNUSED(widget);
    UNUSED(event);
    UNUSED(data);
    return TRUE;
}

/**
 * Too long for its room, the text loses its middle, a bit right of the
 * centre: most of how it starts and some of how it ends stay in view
 * @param layout Layout to set the text in
 * @param text Text, on one line
 * @param width Room, Pango units
 */
static void fit_text(PangoLayout *layout, const gchar *text, gint width) {
    gint w = 0;
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_size(layout, &w, NULL);
    if (w <= width) {
        return;
    }
    static const gchar ELLIPSIS[] = "…";
    pango_layout_set_text(layout, ELLIPSIS, -1);
    gint ellipsis = 0;
    pango_layout_get_size(layout, &ellipsis, NULL);
    const gint room = MAX(0, width - ellipsis);
    const gint head_room = room * 3 / 5;
    const glong chars = g_utf8_strlen(text, -1);
    // the most characters from the start, then from the end, that fit
    glong lo = 0, hi = chars;
    while (lo < hi) {
        const glong mid = (lo + hi + 1) / 2;
        pango_layout_set_text(layout, text, (gint) (g_utf8_offset_to_pointer(text, mid) - text));
        pango_layout_get_size(layout, &w, NULL);
        if (w <= head_room) { lo = mid; } else { hi = mid - 1; }
    }
    const glong head = lo;
    pango_layout_set_text(layout, text, (gint) (g_utf8_offset_to_pointer(text, head) - text));
    pango_layout_get_size(layout, &w, NULL);
    const gint tail_room = room - w;
    lo = 0;
    hi = chars - head;
    while (lo < hi) {
        const glong mid = (lo + hi + 1) / 2;
        pango_layout_set_text(layout, g_utf8_offset_to_pointer(text, chars - mid), -1);
        pango_layout_get_size(layout, &w, NULL);
        if (w <= tail_room) { lo = mid; } else { hi = mid - 1; }
    }
    gchar *start = g_strndup(text, (gsize) (g_utf8_offset_to_pointer(text, head) - text));
    gchar *shown = g_strconcat(start, ELLIPSIS, g_utf8_offset_to_pointer(text, chars - lo), NULL);
    pango_layout_set_text(layout, shown, -1);
    g_free(shown);
    g_free(start);
}

/**
 * Draw the text waiting, on one line: line breaks shown as spaces
 */
static gboolean text_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data) {
    UNUSED(data);
    gchar *line = g_strdup(message ? message : staged->str);
    g_strdelimit(line, "\r\n\t", ' ');
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, NULL);
    const gint width = gtk_widget_get_allocated_width(widget);
    const gint height = gtk_widget_get_allocated_height(widget);
    fit_text(layout, line, width * PANGO_SCALE);
    gint h = 0;
    pango_layout_get_pixel_size(layout, NULL, &h);
    gtk_render_layout(gtk_widget_get_style_context(widget), cr, 0, (height - h) / 2.0, layout);
    g_object_unref(layout);
    g_free(line);
    return FALSE;
}

static void put_in_cb(GtkWidget *button, gpointer run) {
    UNUSED(button);
    put_in(GPOINTER_TO_INT(run));
}

/**
 * @param icon Icon, OUTLINE_...: the sign alone, as on the phone page
 * @return A button of the box
 */
static GtkWidget * bar_button(const gchar *icon) {
    GtkWidget *button = gtk_button_new();
    gtk_widget_set_name(button, VOICEBAR_BUTTON_NAME);
    gtk_widget_set_can_focus(button, FALSE);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(button), icon_new_outline(icon, VOICEBAR_ICON_PX));
    return button;
}

/**
 * Lay the box out: over the shortcut bar (over the foot of the terminal
 * without one), a little in from the sides
 */
static gboolean position_cb(GtkOverlay *overlay, GtkWidget *child, GdkRectangle *rect, gpointer data) {
    UNUSED(data);
    if (child != bar) {
        return FALSE;
    }
    const gboolean on_bar = shortbar_widget && gtk_widget_get_visible(shortbar_widget)
                            && gtk_widget_get_mapped(shortbar_widget);
    gint x = 0, bottom = 0;
    if (on_bar) {
        gtk_widget_translate_coordinates(shortbar_widget, GTK_WIDGET(overlay), 0, 0, &x, &bottom);
    } else {
        gtk_widget_translate_coordinates(terminal_widget, GTK_WIDGET(overlay), 0,
                                         gtk_widget_get_allocated_height(terminal_widget), &x, &bottom);
    }
    const gint width = gtk_widget_get_allocated_width(GTK_WIDGET(overlay)) - 2 * VOICEBAR_MARGIN;
    gint height = 0;
    gtk_widget_get_preferred_height_for_width(bar, width, NULL, &height);
    rect->x = VOICEBAR_MARGIN;
    rect->width = width;
    rect->height = height;
    rect->y = MAX(0, bottom - VOICEBAR_MARGIN - height);
    return TRUE;
}

static gboolean requeue(gpointer data) {
    UNUSED(data);
    if (bar && gtk_widget_get_visible(bar)) {
        gtk_widget_queue_resize(bar);
    }
    return G_SOURCE_REMOVE;
}

static void anchor_moved(GtkWidget *widget, GdkRectangle *alloc, gpointer data) {
    UNUSED(widget);
    UNUSED(alloc);
    UNUSED(data);
    g_idle_add(requeue, NULL);
}

/**
 * Build the box
 * @param overlay Overlay of the window, which it is shown in
 * @param terminal Terminal typed into
 * @param shortbar Shortcut bar it covers, NULL if there is none
 */
void voicebar_init(GtkWidget *overlay, GtkWidget *terminal, GtkWidget *shortbar) {
    terminal_widget = terminal;
    shortbar_widget = shortbar;
    staged = g_string_new(NULL);
    // takes the touches; drawn by the box inside it, rounded, the
    // terminal showing at its corners
    bar = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(bar), FALSE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_name(row, VOICEBAR_NAME);
    text_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(text_area, TRUE);
    // one line high
    PangoLayout *layout = gtk_widget_create_pango_layout(text_area, "Ag中");
    gint line = 0;
    pango_layout_get_pixel_size(layout, NULL, &line);
    g_object_unref(layout);
    gtk_widget_set_size_request(text_area, 1, line);
    gtk_widget_set_valign(text_area, GTK_ALIGN_CENTER);
    g_signal_connect(text_area, "draw", G_CALLBACK(text_draw_cb), NULL);
    // the buttons of the phone page, same signs
    GtkWidget *put = bar_button(OUTLINE_SEND);        // 发送
    run_button = bar_button(OUTLINE_PLAY);            // 发送并回车
    GtkWidget *clear = bar_button(OUTLINE_X);         // 清空
    g_signal_connect(put, "clicked", G_CALLBACK(put_in_cb), GINT_TO_POINTER(FALSE));
    g_signal_connect(run_button, "clicked", G_CALLBACK(put_in_cb), GINT_TO_POINTER(TRUE));
    g_signal_connect(clear, "clicked", G_CALLBACK(voicebar_clear), NULL);
    // Enter at the far left, the text between it and the close button:
    // next to each other, one was hit for the other
    gtk_box_pack_start(GTK_BOX(row), run_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), text_area, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), put, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), clear, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(bar), row);
    gtk_widget_show_all(row);
    gtk_widget_add_events(bar, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(bar, "button-press-event", G_CALLBACK(bar_event), NULL);
    g_signal_connect(bar, "button-release-event", G_CALLBACK(bar_event), NULL);
    g_signal_connect(overlay, "get-child-position", G_CALLBACK(position_cb), NULL);
    g_signal_connect(terminal, "size-allocate", G_CALLBACK(anchor_moved), NULL);
    if (shortbar) {
        g_signal_connect(shortbar, "size-allocate", G_CALLBACK(anchor_moved), NULL);
    }
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bar);
    gtk_widget_set_no_show_all(bar, TRUE);
    gtk_widget_hide(bar);
}
