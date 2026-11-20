/* kterm.c
 *
 * This file is part of kterm
 *
 * Copyright(C) 2013-16 Bartek Fabiszewski (www.fabiszewski.net)
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
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <gtk/gtk.h>
#include <vte/vte.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <getopt.h>
#include <glib-unix.h>
#include "keyboard.h"
#include "kterm.h"
#include "panel.h"
#include "icons.h"
#include "flash.h"
#include "refresh.h"
#include "shortbar.h"
#include "shortpage.h"
#include "shortcuts.h"
#include "settings.h"
#include "connpage.h"
#ifdef KINDLE
#include "kindle.h"
#include "ime.h"
#include "voice.h"
#include "imageview.h"
#include "notice.h"
#include "tmux.h"
#include "tmuxpick.h"
#include "touch.h"
#endif
#include "config.h"

/** Vte version check for early versions */
#ifndef VTE_CHECK_VERSION
#define VTE_CHECK_VERSION(x,y,z) FALSE
#endif

/** Global config */
KTconf *conf;
/** Global debug */
gboolean debug = FALSE;
#ifdef KINDLE
typedef struct {
    GtkWidget *window;
    GtkNotebook *notebook;
    GtkWidget *add;
    gchar **envv;
    guint next_number;
    guint align_source;
    gint swipe_from;
    GtkWidget *rename_entry;
} TerminalTabs;

static TerminalTabs terminal_tabs;
#else
static GtkWidget *desktop_terminal = NULL;
#endif
/** The built-in keyboard */
static Keyboard *main_keyboard = NULL;

/**
 * @return The selected terminal widget
 */
GtkWidget * kterm_get_terminal(void) {
#ifdef KINDLE
    GtkNotebook *notebook = terminal_tabs.notebook;
    const gint page = notebook ? gtk_notebook_get_current_page(notebook) : -1;
    return page >= 0 ? gtk_notebook_get_nth_page(notebook, page) : NULL;
#else
    return desktop_terminal;
#endif
}

typedef struct {
    GPid child;
    gboolean is_shell;
#ifdef KINDLE
    guint number;
    GtkWidget *close_button;
    GtkWidget *title_label;
    gchar *custom_title;
#endif
} TerminalState;

static void free_terminal_state(gpointer data) {
    TerminalState *state = data;
#ifdef KINDLE
    g_free(state->custom_title);
#endif
    g_free(state);
}

static TerminalState *terminal_state(GtkWidget *terminal) {
    return terminal ? g_object_get_data(G_OBJECT(terminal), "kterm-state") : NULL;
}

/**
 * @return TRUE if kterm started a shell in the terminal; FALSE if it runs a
 *         program given with -e (tmux, ssh...), into which commands meant
 *         for a shell must not be typed
 */
gboolean kterm_terminal_has_shell(void) {
    TerminalState *state = terminal_state(kterm_get_terminal());
    return state && state->is_shell;
}

static pid_t terminal_owner(GtkWidget *terminal) {
    VtePty *pty = terminal ? vte_terminal_get_pty(VTE_TERMINAL(terminal)) : NULL;
    const pid_t owner = pty ? tcgetpgrp(vte_pty_get_fd(pty)) : -1;
    return owner > 0 ? owner : 0;
}

/**
 * @return Process group that has the terminal when it is not the process
 *         kterm started: a program is running in the shell (an ssh
 *         session, an editor) and what is typed goes to it. 0 if the
 *         process kterm started has it, or if it is unknown.
 */
pid_t kterm_terminal_foreground(GtkWidget *terminal) {
    TerminalState *state = terminal_state(terminal);
    if (!state || state->child <= 0) {
        return 0;
    }
    const pid_t owner = terminal_owner(terminal);
    return owner != state->child ? owner : 0;
}

/**
 * @return Process group that has the terminal, whichever it is: the
 *         process kterm started too. 0 if unknown.
 */
pid_t kterm_terminal_owner(void) {
    return terminal_owner(kterm_get_terminal());
}

/**
 * @param pid Process
 * @return Its command name ("ssh", "tmux: client"), newly allocated; NULL
 *         if it is gone
 */
gchar * kterm_process_name(pid_t pid) {
    if (pid <= 0) {
        return NULL;
    }
    gchar *path = g_strdup_printf("/proc/%d/comm", (int) pid);
    gchar *name = NULL;
    if (g_file_get_contents(path, &name, NULL, NULL)) {
        g_strstrip(name);
    }
    g_free(path);
    return name;
}

/**
 * @param pid Process
 * @param name Command name
 * @return TRUE if the process is running that command
 */
gboolean kterm_process_is(pid_t pid, const gchar *name) {
    gchar *comm = kterm_process_name(pid);
    const gboolean is = comm && !strcmp(comm, name);
    g_free(comm);
    return is;
}

/**
 * Type a command into the shell in the terminal and run it: ^A ^K first,
 * as anything half typed on the command line would otherwise run together
 * with it. Not ^U: on an empty line, bash rings the bell for it.
 * @param command Command
 */
void kterm_type_command(GtkWidget *terminal, const gchar *command) {
    gchar *line = g_strconcat("\x01\x0b", command, "\r", NULL);
    vte_terminal_feed_child(VTE_TERMINAL(terminal), line, -1);
    g_free(line);
}

/**
 * @return TRUE if the terminal keeps lines scrolled off its top: then a
 *         shell is in it, not a full screen program such as tmux (those
 *         use the alternate screen, which has no history).
 */
gboolean kterm_terminal_has_history(void) {
    GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(kterm_get_terminal()));
    return gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj) > gtk_adjustment_get_lower(adj);
}

/**
 * @return TRUE if the terminal shows earlier lines of its history, not its
 *         last ones
 */
gboolean kterm_terminal_scrolled(void) {
    GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(kterm_get_terminal()));
    return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) < gtk_adjustment_get_upper(adj) - 0.5;
}

/**
 * Show the terminal's last lines again
 */
void kterm_terminal_to_bottom(void) {
    GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(kterm_get_terminal()));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
}

/**
 * Signal callback, run from the main loop (not in the signal handler, where
 * GTK must not be called): quit as the menu would, so that the screen
 * orientation is restored
 * @param data Not used
 * @return G_SOURCE_REMOVE
 */
static gboolean exit_on_signal(gpointer data) {
    UNUSED(data);
    D printf("exiting on signal\n");
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

/**
 * SIGUSR1: kterm was started again (kindle/kpm/launch.sh), which only
 * brings this one to the front. The Kindle's window manager ignores being
 * asked to (_NET_ACTIVE_WINDOW); an application window it is given anew
 * becomes the active application (lab126_application_layer.lua): the
 * window is taken away and shown again.
 * @param data Not used
 * @return G_SOURCE_CONTINUE
 */
static gboolean present_on_signal(gpointer data) {
    UNUSED(data);
    GtkWidget *terminal = kterm_get_terminal();
    GtkWidget *window = terminal ? gtk_widget_get_toplevel(terminal) : NULL;
    if (window && gtk_widget_get_realized(window)) {
        D printf("brought to the front\n");
        gtk_widget_hide(window);
        gtk_widget_show(window);
        gtk_window_present(GTK_WINDOW(window));
    }
    return G_SOURCE_CONTINUE;
}

/**
 * Install signal handlers
 */
static void install_signal_handlers(void) {
    // SIGCHLD left as it is: GLib and VTE reap the children they start,
    // and image loaders (glycin) wait for theirs
    g_unix_signal_add(SIGINT, exit_on_signal, NULL);
    g_unix_signal_add(SIGTERM, exit_on_signal, NULL);
    g_unix_signal_add(SIGUSR1, present_on_signal, NULL);
}

/**
 * Free all resources
 * @param keyboard Keyboard structure
 */
static void clean_on_exit(Keyboard *keyboard) {
    D printf("cleanup\n");
#ifdef KINDLE
    keyboard_grab(NULL, FALSE);
    orientation_restore();
    ime_shutdown();
#endif
    keyboard_free(&keyboard);
    g_free(conf);
}

/**
 * Grab focus on callback
 * @param widget Calling widget
 * @param data User data
 */
#ifndef KINDLE
static void grab_focus(GtkWidget *widget, gpointer data) {
    UNUSED(data);
    D printf("grab focus: %s\n", gtk_widget_get_name(widget)) ;
    gtk_widget_grab_focus(widget);
}

/**
 * Terminal exit handler
 */
static void terminal_exit(void) {
    sleep(1); // time for kb to send key up event
    gtk_main_quit();
}
#endif

#if VTE_CHECK_VERSION(0,20,0)
/**
 * Set terminal cursor shape
 * @param terminal Terminal
 * @param cursor_shape Letter representing desired shape ('B', 'I' or 'U')
 */
static void set_terminal_cursor(VteTerminal *terminal, gchar cursor_shape) {
#if VTE_CHECK_VERSION(0,38,0)
    VteCursorShape shape = 0;
#else
    VteTerminalCursorShape shape = 0;
#endif
    switch (cursor_shape) {
        case 'B':
            shape = VTE_CURSOR_SHAPE_BLOCK;
            break;
        case 'I':
            shape = VTE_CURSOR_SHAPE_IBEAM;
            break;
        case 'U':
            shape = VTE_CURSOR_SHAPE_UNDERLINE;
            break;
    }
    vte_terminal_set_cursor_shape(terminal, shape);
#ifdef KINDLE
    // each blink would be a refresh of the e-ink screen
    vte_terminal_set_cursor_blink_mode(terminal, VTE_CURSOR_BLINK_OFF);
#endif
}
#endif

/**
 * Set terminal font
 * @param terminal Terminal
 * @param font_family Font family
 * @param font_size Font size
 */
static void set_terminal_font(VteTerminal *terminal, const gchar *font_family, const gint font_size) {
    gchar font_name[200];
    snprintf(font_name, sizeof(font_name), "%s %i", font_family, font_size);
    D printf("font_name: %s\n", font_name);
    PangoFontDescription *desc = pango_font_description_from_string(font_name);
    vte_terminal_set_font(VTE_TERMINAL(terminal), desc);
    pango_font_description_free(desc);
}

#ifndef KINDLE
/**
 * Resize terminal font
 * @param terminal Terminal
 * @param mod FONT_UP or FONT_DOWN
 */
static void resize_font(VteTerminal *terminal, const guint mod) {
    const PangoFontDescription *pango_desc = vte_terminal_get_font(VTE_TERMINAL(terminal));
    gchar *pango_name = pango_font_description_to_string(pango_desc);
    if G_UNLIKELY(!pango_name) {
        return;
    }
    gchar *size_offset = strrchr(pango_name, ' ');
    if (size_offset) {
        *size_offset = '\0';
        gint font_size = atoi(++size_offset);
        D printf("font_family: %s\n", pango_name);
        D printf("font_size: %i\n", font_size);
        if (mod == FONT_UP) {
            font_size++;
        }
        else if (font_size > 1) {
            font_size--;
        }
        set_terminal_font(terminal, pango_name, font_size);
        // the settings page counts from here
        conf->font_size = (guint) font_size;
    }
    g_free(pango_name);
}

/**
 * Increase font size menu callback
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void fontup(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    resize_font(terminal, FONT_UP);
}

/**
 * Decrease font size menu callback
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void fontdown(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    resize_font(terminal, FONT_DOWN);
}
#endif
/**
 * Setup terminal color scheme
 * @param terminal Terminal
 * @param scheme VTE_SCHEME_LIGHT or VTE_SCHEME_DARK
 */
static void set_terminal_colors(GtkWidget *terminal, gboolean scheme) {
#if GTK_CHECK_VERSION(3,14,0)
#define ANSI_GRAY(value) { (value) / 255.0, (value) / 255.0, (value) / 255.0, 1.0 }
    // ANSI order: black, red, green, yellow, blue, magenta, cyan, white.
    GdkRGBA palette_light[8] = {
        ANSI_GRAY(0), ANSI_GRAY(0x50), ANSI_GRAY(0x70), ANSI_GRAY(0xa0),
        ANSI_GRAY(0x80), ANSI_GRAY(0x30), ANSI_GRAY(0x90), ANSI_GRAY(0xff)
    };
    GdkRGBA palette_dark[8] = {
        ANSI_GRAY(0), ANSI_GRAY(0x88), ANSI_GRAY(0x98), ANSI_GRAY(0xd0),
        ANSI_GRAY(0xa8), ANSI_GRAY(0x70), ANSI_GRAY(0xb8), ANSI_GRAY(0xff)
    };
#undef ANSI_GRAY
    GdkRGBA *palette;
    GdkRGBA color_white = { 1, 1, 1, 1 };
    GdkRGBA color_black = { 0, 0, 0, 1 };
    GdkRGBA color_bg, color_fg;
#else
    // light background
    GdkColor palette_light[8] = {{ 0, 0x0000, 0x0000, 0x0000 },
        { 0, 0x5050, 0x5050, 0x5050 },
        { 0, 0x7070, 0x7070, 0x7070 },
        { 0, 0xa0a0, 0xa0a0, 0xa0a0 },
        { 0, 0x8080, 0x8080, 0x8080 },
        { 0, 0x3030, 0x3030, 0x3030 },
        { 0, 0x9090, 0x9090, 0x9090 },
        { 0, 0xffff, 0xffff, 0xffff }};
    // dark background
    GdkColor palette_dark[8] = {{ 0, 0x0000, 0x0000, 0x0000 },
        { 0, 0x8888, 0x8888, 0x8888 },
        { 0, 0x9898, 0x9898, 0x9898 },
        { 0, 0xd0d0, 0xd0d0, 0xd0d0 },
        { 0, 0xa8a8, 0xa8a8, 0xa8a8 },
        { 0, 0x7070, 0x7070, 0x7070 },
        { 0, 0xb8b8, 0xb8b8, 0xb8b8 },
        { 0, 0xffff, 0xffff, 0xffff }};
    GdkColor *palette;
    GdkColor color_white = { 0, 0xffff, 0xffff, 0xffff };
    GdkColor color_black = { 0, 0x0000, 0x0000, 0x0000 };
    GdkColor color_dim = { 0, 0x8888, 0x8888, 0x8888 };
    GdkColor color_bg, color_fg;
#endif
    switch (scheme) {
        default:
        case VTE_SCHEME_LIGHT:
            palette = palette_light;
            color_bg = color_white;
            color_fg = color_black;
            break;
        case VTE_SCHEME_DARK:
            palette = palette_dark;
            color_bg = color_black;
            color_fg = color_white;
            break;
    }
    vte_terminal_set_colors(VTE_TERMINAL(terminal), NULL, NULL, palette, 8);
    vte_terminal_set_color_background(VTE_TERMINAL(terminal), &color_bg);
    vte_terminal_set_color_foreground(VTE_TERMINAL(terminal), &color_fg);
#if !GTK_CHECK_VERSION(3,14,0)
    vte_terminal_set_color_dim(VTE_TERMINAL(terminal), &color_dim);
#endif
    vte_terminal_set_color_bold(VTE_TERMINAL(terminal), &color_fg);
    vte_terminal_set_color_cursor(VTE_TERMINAL(terminal), NULL);
    vte_terminal_set_color_highlight(VTE_TERMINAL(terminal), NULL);
    conf->color_reversed = scheme;
}

#ifndef KINDLE
/**
 * Reverse color scheme menu callback
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void reverse_colors(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    set_terminal_colors(terminal, !conf->color_reversed);
}
#endif

#ifdef KINDLE
/** Built-in keyboard container */
static GtkWidget *builtin_kb_box = NULL;
/** Reading: the keyboard stays folded away, a tap on the terminal does not
 *  bring it back. Left from the panel, or by anything that shows a keyboard. */
static gboolean read_mode = FALSE;
/** Back to the preview after the next key, see kterm_preview_after_key() */
static gboolean preview_after_key = FALSE;
/**
 * Pass a downward swipe over the panel or the tab reveal strip to the
 * Kindle's quick settings, as a swipe over the terminal's top edge does.
 * @param drag Gesture on the panel, seen before its buttons
 * @param dx Horizontal travel
 * @param dy Vertical travel, down positive
 * @param data Not used
 */
static void top_swipe_cb(GtkGestureDrag *drag, gdouble dx, gdouble dy, gpointer data) {
    UNUSED(data);
    gdouble x = 0, y = 0;
    if (!gtk_gesture_drag_get_start_point(drag, &x, &y)) {
        return;
    }
    if (y <= TOUCH_EDGE_TOP_PX && dy >= TOUCH_SWIPE_MIN_PX && dy >= ABS(dx) * 1.5) {
        panel_close();
        show_system_menu();
    }
}

static void watch_top_swipe(GtkWidget *widget) {
    GtkGesture *swipe = gtk_gesture_drag_new(widget);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(swipe), GTK_PHASE_CAPTURE);
    g_signal_connect(swipe, "drag-end", G_CALLBACK(top_swipe_cb), NULL);
    g_object_set_data_full(G_OBJECT(widget), "kterm-top-swipe", swipe, g_object_unref);
}

/**
 * Fold the built-in keyboard away. Nothing brings it back by itself:
 * the user does, with a tap on the terminal or from the panel.
 */
void kterm_hide_keyboard(void) {
    if (builtin_kb_box && conf->kb_on) {
        conf->kb_on = FALSE;
        gtk_widget_hide(builtin_kb_box);
    }
}
/** Shown instead of the built-in keyboard while the system one is up */
static GtkWidget *ime_panel = NULL;
/** Empty area of ime_panel that the system keyboard window covers */
static GtkWidget *ime_spacer = NULL;

/**
 * Switch between the built-in keyboard and the system keyboard.
 * The system keyboard is a separate window drawn over the bottom of ours,
 * so the same height is kept free below the terminal while it is open.
 */
void kterm_toggle_ime(void) {
    if (!ime_panel) {
        return;
    }
    if (ime_is_open()) {
        ime_close();
        gtk_widget_hide(ime_panel);
        if (conf->kb_on) {
            gtk_widget_show(builtin_kb_box);
        }
        return;
    }
    // the panel's form is filled in with the built-in keyboard
    panel_close();
    if (ime_open()) {
        gtk_widget_set_size_request(ime_spacer, -1, ime_keyboard_height());
        gtk_widget_hide(builtin_kb_box);
        gtk_widget_show(ime_panel);
    }
}

/**
 * Make the built-in keyboard the one on screen: leave the system keyboard
 * and unfold ours. The panel's form is typed with it.
 */
void kterm_use_builtin_keyboard(void) {
    read_mode = FALSE;
    preview_after_key = FALSE;
    if (ime_is_open()) {
        kterm_toggle_ime();
    }
    if (builtin_kb_box && !conf->kb_on) {
        conf->kb_on = TRUE;
        gtk_widget_show(builtin_kb_box);
    }
    shortbar_update();
}

/**
 * Lay the built-in keyboard out again: a key came or went (the "语音"
 * key, see voice.c)
 */
void kterm_keyboard_relayout(void) {
    if (main_keyboard) {
        keyboard_set_size(main_keyboard);
    }
}

/**
 * Preview: every keyboard folded away, the shortcut bar alone at the foot
 * of the screen. Touches on the terminal only scroll, switch tmux windows
 * or are passed to the program. Left, the built-in keyboard is back.
 * @param on TRUE to preview
 */
void kterm_set_preview(gboolean on) {
    if (on == read_mode) {
        return;
    }
    if (!on) {
        kterm_use_builtin_keyboard();
        return;
    }
    if (ime_is_open()) {
        kterm_toggle_ime();
    }
    read_mode = TRUE;
    preview_after_key = FALSE;
    kterm_hide_keyboard();
    shortbar_update();
}

/**
 * @return TRUE while previewing
 */
gboolean kterm_previewing(void) {
    return read_mode;
}

/**
 * The keyboard came out of the preview for one key, after a prefix: once
 * that key is typed, back to the preview
 */
void kterm_preview_after_key(void) {
    preview_after_key = TRUE;
}

/**
 * A tap on the terminal: it brings a folded keyboard back, as tapping a
 * text field does elsewhere on the device. Not while previewing: there a
 * tap belongs to the program (tmux switches to the window whose name is
 * tapped). Nor with the system keyboard up.
 */
void kterm_keyboard_back_on_tap(void) {
    if (!read_mode && !conf->kb_on && builtin_kb_box && !ime_is_open()) {
        conf->kb_on = TRUE;
        gtk_widget_show(builtin_kb_box);
    }
}

/**
 * A key was typed, on the keyboard or the shortcut bar (not a modifier,
 * which waits for its key)
 */
void kterm_key_typed(void) {
    if (preview_after_key && !ime_is_open() && !panel_wants_keys()) {
        kterm_set_preview(TRUE);
    }
}

/**
 * Build the panel that replaces the built-in keyboard while the system
 * keyboard is open: one button to come back, and room for that keyboard.
 * @return Panel widget, hidden
 */
static GtkWidget * build_ime_panel(void) {
#if GTK_CHECK_VERSION(3,2,0)
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
#else
    GtkWidget *panel = gtk_vbox_new(FALSE, 0);
#endif
    GtkWidget *back = gtk_button_new_with_label("返回终端键盘");
    gtk_widget_set_name(back, "ktermKbButton");
    // focus must stay on the terminal, that is where the keys go
    gtk_widget_set_can_focus(back, FALSE);
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(kterm_toggle_ime), NULL);
    ime_spacer = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(panel), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panel), ime_spacer, FALSE, FALSE, 0);
    gtk_widget_show(back);
    gtk_widget_show(ime_spacer);
    // stays hidden through gtk_widget_show_all() on the window
    gtk_widget_set_no_show_all(panel, TRUE);
    return panel;
}
#else
void kterm_toggle_ime(void) {}
void kterm_hide_keyboard(void) {}
void kterm_use_builtin_keyboard(void) {}
void kterm_set_preview(gboolean on) { UNUSED(on); }
gboolean kterm_previewing(void) { return FALSE; }
void kterm_preview_after_key(void) {}
void kterm_key_typed(void) {}
#endif

/**
 * Lay the keyboard out again, after its keys got new sizes
 * @param keyboard_box Keyboard widget
 * @return FALSE, once
 */
static gboolean keyboard_relayout(gpointer keyboard_box) {
    gtk_widget_queue_resize(GTK_WIDGET(keyboard_box));
    return G_SOURCE_REMOVE;
}

/**
 * Keyboard widget size allocation signal handler.
 * Updates keyboard size
 * @param keyboard_box Keyboard widget
 * @param alloc Size allocation
 * @param keyboard Keyboard structure
 */
static void keyboard_update(GtkWidget *keyboard_box, GtkAllocation *alloc, Keyboard *keyboard) {
    GdkScreen *screen = gdk_screen_get_default();
    gint screen_height = gdk_screen_get_height(screen);
    static gint saved_width = -1;
    static gint saved_height = -1;
    if (conf->kb_on && alloc && (alloc->width != saved_width || screen_height != saved_height)) {
        D printf("set keyboard size: %ix%i\n", alloc->width, alloc->height);
        // At once, not from an idle callback: GTK lays the window out
        // again before drawing it, so the keys sized for the old width
        // (after a rotation, an empty band on the right) are never drawn,
        // which on e-ink would stay visible for a whole refresh.
        keyboard_set_size(keyboard);
        saved_width = alloc->width;
        saved_height = screen_height;
        // GTK drops sizes asked for while it is laying the window out: after
        // a rotation the keys kept their old widths, an empty band on the
        // right. Lay them out again before the next frame is drawn.
        g_idle_add_full(G_PRIORITY_HIGH_IDLE, keyboard_relayout, keyboard_box, NULL);
    }
}

#ifndef KINDLE
/**
 * Reset terminal manu callback
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void reset_terminal(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    vte_terminal_reset(terminal, TRUE, TRUE);
}

/**
 * Toggle keyboard menu callback
 * @param widget Calling widget
 * @param box Kterm container
 */
static void toggle_keyboard(GtkWidget *widget, gpointer box) {
    UNUSED(widget);
    GtkWidget *keyboard_box = NULL;
    GList *box_list = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *cur = box_list; cur != NULL; cur = cur->next) {
        const gchar *box_name = gtk_widget_get_name(GTK_WIDGET(cur->data));
        D printf("box: %s\n", box_name);
        if (!strncmp(box_name, "kbBox", 5)) { keyboard_box = GTK_WIDGET(cur->data); }
    }
    g_list_free(box_list);
    if (keyboard_box) {
        if (conf->kb_on) {
            gtk_widget_hide(keyboard_box);
            conf->kb_on = FALSE;
        } else {
            gtk_widget_show(keyboard_box);
            conf->kb_on = TRUE;
        }
    }
}
#endif

#ifndef KINDLE
/**
 * Build popup menu
 * @param terminal Terminal widget
 * @param box Kterm container
 * @return Menu widget
 */
static GtkWidget * build_popup(GtkWidget *terminal, GtkWidget *box) {
    // popup menu on button release
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *fontup_item = gtk_menu_item_new_with_label("Font increase");
    GtkWidget *fontdown_item = gtk_menu_item_new_with_label("Font decrease");
    GtkWidget *color_item = gtk_menu_item_new_with_label("Reverse colors");
    GtkWidget *kb_item = gtk_menu_item_new_with_label("Toggle keyboard");
    GtkWidget *reset_item = gtk_menu_item_new_with_label("Reset terminal");
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), fontup_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), fontdown_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), color_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), kb_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), reset_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
    
    
    g_signal_connect(G_OBJECT(fontup_item), "activate", G_CALLBACK(fontup), (gpointer) terminal);
    g_signal_connect(G_OBJECT(fontdown_item), "activate", G_CALLBACK(fontdown), (gpointer) terminal);
    g_signal_connect(G_OBJECT(color_item), "activate", G_CALLBACK(reverse_colors), (gpointer) terminal);
    g_signal_connect(G_OBJECT(kb_item), "activate", G_CALLBACK(toggle_keyboard), box);
    g_signal_connect(G_OBJECT(reset_item), "activate", G_CALLBACK(reset_terminal), (gpointer) terminal);
    g_signal_connect(G_OBJECT(quit_item), "activate", G_CALLBACK(gtk_main_quit), NULL);
    
    gtk_widget_show_all(menu);
    return menu;
}
#endif

#if GTK_CHECK_VERSION(3,2,0)
#ifdef KINDLE
/**
 * "文字加深": on the grey scale screen coloured and dim text turns into
 * light grey; on, text is drawn far from its background (kterm's own VTE,
 * kindle/vte/0004-text-contrast.patch)
 * @param on Non-zero for on
 */
void kterm_set_text_contrast(guint on) {
    conf->text_contrast = on ? 1 : 0;
    GtkWidget *terminal = kterm_get_terminal();
    if (terminal) {
        vte_terminal_set_text_contrast(VTE_TERMINAL(terminal), on != 0);
    }
}
#endif

/**
 * Set the terminal's font size, as from the settings page
 * @param size Points
 */
void kterm_set_font_size(guint size) {
    conf->font_size = size;
    GtkWidget *terminal = kterm_get_terminal();
    if (terminal) {
        set_terminal_font(VTE_TERMINAL(terminal), conf->font_family, (gint) size);
    }
}

#endif

#ifdef KINDLE
/** In the terminal's title, followed by 1 or 0: whether the program in
 *  tmux's active pane waits for input. tmux.conf puts it there, see the
 *  README. */
#define PASTE_TITLE_MARK "kterm-paste:"

/**
 * The terminal changed: the paste button is offered while the program in
 * it waits for input, which it tells by asking for pasted text to be
 * marked (bracketed paste). The question is kterm's own VTE's, see
 * kindle/vte/. tmux keeps bracketed paste on for itself, whatever its panes
 * want: when tmux has the terminal, the title it gives it says, if
 * tmux.conf asks it to.
 */
static void paste_target_cb(VteTerminal *terminal, gpointer data) {
    UNUSED(data);
    if (GTK_WIDGET(terminal) != kterm_get_terminal()) {
        return;
    }
    gboolean wanted = vte_terminal_get_bracketed_paste(terminal);
    const char *title = vte_terminal_get_termprop_string_by_id(terminal, VTE_PROPERTY_ID_XTERM_TITLE, NULL);
    const char *mark = title ? strstr(title, PASTE_TITLE_MARK) : NULL;
    // a title left behind by a tmux no longer there does not count
    if (mark && tmux_may_have_terminal()) {
        wanted = mark[strlen(PASTE_TITLE_MARK)] == '1';
    }
    shortbar_paste_wanted(wanted);
}

static void set_tab_title(VteTerminal *terminal, GtkWidget *label_widget) {
    TerminalState *state = terminal_state(GTK_WIDGET(terminal));
    g_autofree gchar *automatic = NULL;
    const gchar *label = state->custom_title;
    if (!label) {
        const gchar *title = vte_terminal_get_termprop_string_by_id(terminal, VTE_PROPERTY_ID_XTERM_TITLE, NULL);
        const gchar *mark = title ? strstr(title, PASTE_TITLE_MARK) : NULL;
        automatic = title ? g_strstrip(mark ? g_strndup(title, mark - title) : g_strdup(title)) : NULL;
        if (!automatic || !*automatic) {
            g_free(automatic);
            automatic = g_strdup_printf("终端 %u", state->number);
        }
        label = automatic;
    }
    gtk_label_set_text(GTK_LABEL(label_widget), label);
    gtk_widget_set_tooltip_text(label_widget, label);
}

static void title_cb(VteTerminal *terminal, const char *name, GtkWidget *label_widget) {
    UNUSED(name);
    set_tab_title(terminal, label_widget);
    paste_target_cb(terminal, NULL);
}

#endif

/**
 * Mouse button and motion callback
 * @param terminal Terminal widget
 * @param event Button event
 * @param menu Popup menu (not on the Kindle)
 * @return True to stop processing event, false otherwise
 */
static gboolean button_event(GtkWidget *terminal, GdkEventButton *event, gpointer menu) {
#ifdef KINDLE
    if (touch_forwarding()) {
        return FALSE;
    }
    if (terminal != kterm_get_terminal()) {
        return TRUE;
    }
#endif
    // A touch while the panel is open puts it away, and the program in the
    // terminal sees none of it.
    static gboolean closing_panel = FALSE;
    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        closing_panel = panel_is_open();
    }
    if (closing_panel) {
        if (event->type == GDK_BUTTON_RELEASE && event->button == 1) {
            closing_panel = FALSE;
            panel_close();
        }
        return TRUE;
    }
#ifdef KINDLE
    UNUSED(menu);
    return touch_event(terminal, (GdkEvent *) event);
#else
    UNUSED(terminal);
    if (event->button == BUTTON_MENU) {
        if (event->type == GDK_BUTTON_RELEASE) { return FALSE; }
        gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, event->button, event->time);
        return TRUE;
    }
    return FALSE;
#endif
}

/**
 * Print version number.
 * If possible check for consistent libraries usage.
 */
static void version(void) {
    printf("kterm %s (vte %i.%i.%i, gtk+ %i.%i.%i)\n",
           VERSION, VTE_MAJOR_VERSION, VTE_MINOR_VERSION, VTE_MICRO_VERSION,
           GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION);
#if VTE_CHECK_VERSION(0,40,0)
    if (vte_get_major_version() != VTE_MAJOR_VERSION ||
        vte_get_minor_version() != VTE_MINOR_VERSION ||
        vte_get_micro_version() != VTE_MICRO_VERSION) {
        printf("Warning, using different vte version than compiled with (%i.%i.%i)!\n",
               vte_get_major_version(),
               vte_get_minor_version(),
               vte_get_micro_version());
    }
#endif
#if GTK_CHECK_VERSION(3,0,0)
    if (gtk_get_major_version() != GTK_MAJOR_VERSION ||
        gtk_get_minor_version() != GTK_MINOR_VERSION ||
        gtk_get_micro_version() != GTK_MICRO_VERSION) {
        printf("Warning, using different gtk+ version than compiled with (%i.%i.%i)!\n",
               gtk_get_major_version(),
               gtk_get_minor_version(),
               gtk_get_micro_version());
    }
#endif
    exit(0);
}

/**
 * Print usage info and exit
 */
static void usage(void) {
    printf("Usage: kterm [OPTIONS]\n");
    printf("        -c <0|1>      color scheme (0 light, 1 dark)\n");
    printf("        -d            debug mode\n");
    printf("        -e <command>  execute command in kterm\n");
    printf("        -E <var>      set environment variable\n");
    printf("        -f <family>   font family\n");
    printf("        -h            show this message\n");
    printf("        -k <0|1>      keyboard off/on\n");
    printf("        -l <path>     keyboard layout config path\n");
#ifdef KINDLE
    printf("        -o <U|R|L>    screen orientation (up, right, left)\n");
#endif
    printf("        -s <size>     font size\n");
    printf("        -t <encoding> terminal encoding\n");
#if VTE_CHECK_VERSION(0,20,0)
    printf("        -u <B|I|U>    cursor shape (block, I-beam, underline)\n");
#endif
    printf("        -v            print version and exit\n");
    exit(0);
}

static void configure_terminal(GtkWidget *terminal) {
    g_object_set_data_full(G_OBJECT(terminal), "kterm-state", g_new0(TerminalState, 1), free_terminal_state);
    set_terminal_colors(terminal, conf->color_reversed);
#ifdef KINDLE
    vte_terminal_set_text_contrast(VTE_TERMINAL(terminal), conf->text_contrast != 0);
#endif
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(terminal), VTE_SCROLLBACK_LINES);
    set_terminal_font(VTE_TERMINAL(terminal), conf->font_family, (gint) conf->font_size);
#if VTE_CHECK_VERSION(0,20,0)
    set_terminal_cursor(VTE_TERMINAL(terminal), conf->cursor_shape);
#endif
#if VTE_CHECK_VERSION(0,38,0)
    vte_terminal_set_encoding(VTE_TERMINAL(terminal), conf->encoding, NULL);
#else
    vte_terminal_set_encoding(VTE_TERMINAL(terminal), conf->encoding);
#endif
    vte_terminal_set_allow_bold(VTE_TERMINAL(terminal), TRUE);
}

/**
 * Start the program after the terminal has its initial size.
 * @param terminal Terminal
 * @param command Command passed to terminal, null if none
 * @param envv Null terminated array of env variable=value pairs passed to terminal
 * @param error Set on error, null otherwise
 */
static void spawn_terminal(GtkWidget *terminal, const gchar *command, gchar **envv, GError **error) {
    gchar *argv[TERM_ARGS_MAX] = { NULL };
    gint argc = 0;
    gchar *shell = NULL;
    gchar *command_args = g_strdup(command);
    TerminalState *state = terminal_state(terminal);
#ifdef KINDLE
    gchar **login_env = NULL;
#endif
#if VTE_CHECK_VERSION(0,25,1)
    if (!command || *command == '\0') {
        // prepend args with shell
        shell = vte_get_user_shell();
        if (!shell || !*shell) {
            g_free(shell);
            shell = g_strdup("/bin/sh");
        }
        argv[argc++] = shell;
#ifdef KINDLE
        argv[argc++] = "-l";
        login_env = g_environ_setenv(g_strdupv(envv), "SHELL", shell, FALSE);
        envv = login_env;
#endif
    }
#endif
    if (command_args) {
        gchar *argbuf = strtok(command_args, " ");
        while (argbuf != NULL && argc < (TERM_ARGS_MAX - 1)) {
            argv[argc++] = argbuf;
            argbuf = strtok(NULL, " ");
        }
    }
    if (argv[0]) {
        static const gchar *shells[] = { "sh", "ash", "bash", "dash", "ksh", "mksh", "zsh", "fish" };
        gchar *name = g_path_get_basename(argv[0]);
        for (guint i = 0; i < G_N_ELEMENTS(shells); i++) {
            state->is_shell |= !strcmp(name, shells[i]);
        }
        g_free(name);
    }

#if VTE_CHECK_VERSION(0,38,0)
    vte_terminal_spawn_sync(VTE_TERMINAL(terminal), 0, NULL, argv, envv, G_SPAWN_SEARCH_PATH, NULL, NULL, &state->child, NULL, error);
#elif VTE_CHECK_VERSION(0,25,1)
    vte_terminal_fork_command_full(VTE_TERMINAL(terminal), 0, NULL, argv, envv, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, error);
#else
    gboolean ret = TRUE;
    ret = vte_terminal_fork_command(VTE_TERMINAL(terminal), argv[0], (argv[0] ? argv : NULL), envv, NULL, FALSE, FALSE, FALSE);
    if (!ret) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "vte_terminal_fork_command returned error");
    }
#endif
    if (shell) { g_free(shell); }
#ifdef KINDLE
    g_strfreev(login_env);
#endif
    g_free(command_args);
    if (*error) {
        g_prefix_error(error, "VTE terminal fork failed.\n");
        D printf("%s\n", (*error)->message);
    }
}

/**
 * Print key event debug info on callback
 * @param widget Calling widget
 * @param event Event
 * @return Always false to propagate event
 */
static gboolean debug_key_event(GtkWidget *widget, GdkEventKey *event) {
    printf("key event; widget: %s\n", gtk_widget_get_name(widget));
    printf("key event; type: %s (%i)\n", (event->type == 8) ? "press" : "release", event->type);
    printf("key event; window: %p\n", (void *) event->window);
    printf("key event; send event: %i\n", event->send_event);
    printf("key event; time: %i\n", event->time);
    printf("key event; state: %i\n", event->state);
    printf("key event; keyval: %i (%s)\n", event->keyval, gdk_keyval_name(event->keyval));
    printf("key event; length: %i\n", event->length);
    printf("key event; string: %s\n", event->string);
    printf("key event; hardware_keycode: %i\n", event->hardware_keycode);
    printf("key event; group: %i\n", event->group);
    printf("key event; modifier: %i\n", event->is_modifier);
    return FALSE;
}

/**
 * Display dialog with error message and clear error
 * @param window Parent window
 * @param error Error structure
 */
static void error_handle(GtkWidget *window, GError **error) {
    GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), flags,
                                               GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                               "%s", (*error)->message);
#ifdef KINDLE
    gtk_window_set_title(GTK_WINDOW(dialog), TITLE_DIALOG);
#endif
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_clear_error(error);
}

#ifdef KINDLE
typedef struct {
    GtkWidget *window;
    GtkWidget *terminal;
    gchar *command;
    gchar **envv;
    gulong handler;
    gint status;
} InitialChild;

static void spawn_after_paint(GdkFrameClock *clock, InitialChild *initial) {
    g_clear_signal_handler(&initial->handler, clock);
    GError *error = NULL;
    spawn_terminal(initial->terminal, initial->command, initial->envv, &error);
    if (error) {
        initial->status = 1;
        error_handle(initial->window, &error);
        gtk_main_quit();
    }
}

/** Run GTK and start the child once the displayed grid has been laid out. */
static gint run_terminal(GtkWidget *window, GtkWidget *terminal, gchar *command, gchar **envv) {
    InitialChild initial = {
        .window = window,
        .terminal = terminal,
        .command = command,
        .envv = envv,
    };
    GdkFrameClock *clock = g_object_ref(gtk_widget_get_frame_clock(terminal));
    initial.handler = g_signal_connect_after(clock, "after-paint", G_CALLBACK(spawn_after_paint), &initial);
    gtk_widget_queue_draw(terminal);
    gtk_main();
    g_clear_signal_handler(&initial.handler, clock);
    g_object_unref(clock);
    return initial.status;
}
#endif

#ifdef KINDLE
void kterm_set_tab_hidden(guint hidden) {
    conf->auto_hide_tabs = hidden != 0;
    gtk_notebook_set_show_tabs(terminal_tabs.notebook, !hidden);
}

static gboolean tab_titles_overflow(GtkNotebook *notebook) {
    const gint count = gtk_notebook_get_n_pages(notebook);
    if (count < 2) {
        return FALSE;
    }
    GtkWidget *first = gtk_notebook_get_nth_page(notebook, 0);
    GtkWidget *last = gtk_notebook_get_nth_page(notebook, count - 1);
    return !gtk_widget_get_child_visible(gtk_notebook_get_tab_label(notebook, first))
        || !gtk_widget_get_child_visible(gtk_notebook_get_tab_label(notebook, last));
}

static gboolean size_tab_titles(GtkNotebook *notebook, gint count, gint width) {
    gint available = width - TAB_CORNER_PX - 1;
    if (available <= 0) {
        return FALSE;
    }
    typedef struct {
        GtkWidget *label;
        gint text_width;
        gint chrome_width;
    } TabWidth;
    g_autofree TabWidth *widths = g_new(TabWidth, count);
    gint64 total = 0;
    for (gint i = 0; i < count; ++i) {
        GtkWidget *page = gtk_notebook_get_nth_page(notebook, i);
        GtkWidget *label = terminal_state(page)->title_label;
        GtkWidget *header = gtk_notebook_get_tab_label(notebook, page);
        PangoLayout *layout = pango_layout_copy(gtk_label_get_layout(GTK_LABEL(label)));
        pango_layout_set_width(layout, -1);
        gint text_width, label_min, header_min;
        pango_layout_get_pixel_size(layout, &text_width, NULL);
        g_object_unref(layout);
        gtk_widget_get_preferred_width(label, &label_min, NULL);
        gtk_widget_get_preferred_width(header, &header_min, NULL);
        widths[i] = (TabWidth) { label, text_width, header_min - label_min };
        total += text_width + widths[i].chrome_width;
    }

    if (count > 1 && total > available) {
        available -= 2 * TAB_ARROW_PX;
    }
    gboolean resized = FALSE;
    // GtkNotebook uses the minimum width, even when there is room for the full title.
    for (gint i = 0; i < count; ++i) {
        TabWidth *tab = &widths[i];
        const gint wanted = CLAMP(tab->text_width, 1, MAX(1, available - tab->chrome_width));
        gint current;
        gtk_widget_get_size_request(tab->label, &current, NULL);
        if (wanted != current) {
            gtk_widget_set_size_request(tab->label, wanted, -1);
            resized = TRUE;
        }
    }
    return resized;
}

static gboolean align_tab_titles(gpointer data) {
    TerminalTabs *tabs = data;
    tabs->align_source = 0;
    GtkNotebook *notebook = tabs->notebook;
    const gint count = gtk_notebook_get_n_pages(notebook);
    if (!count || !gtk_notebook_get_show_tabs(notebook)) {
        return G_SOURCE_REMOVE;
    }

    GtkWidget *widget = GTK_WIDGET(notebook);
    const gint width = gtk_widget_get_allocated_width(widget);
    GtkWidget *spacer = gtk_notebook_get_action_widget(notebook, GTK_PACK_START);
    gint previous;
    gtk_widget_get_size_request(spacer, &previous, NULL);
    // GTK floors the spacer's width request at one pixel.
    gint offset = 1;
    if (!size_tab_titles(notebook, count, width) && !tab_titles_overflow(notebook)) {
        GtkWidget *first = gtk_notebook_get_nth_page(notebook, 0);
        GtkWidget *last = gtk_notebook_get_nth_page(notebook, count - 1);
        GtkWidget *first_title = gtk_widget_get_parent(terminal_state(first)->title_label);
        GtkWidget *last_title = gtk_widget_get_parent(terminal_state(last)->title_label);
        gint left, right, end;
        if (gtk_widget_translate_coordinates(first_title, widget, 0, 0, &left, NULL)
            && gtk_widget_translate_coordinates(last_title, widget, gtk_widget_get_allocated_width(last_title), 0, &right, NULL)
            && gtk_widget_translate_coordinates(tabs->add, widget, gtk_widget_get_allocated_width(tabs->add), 0, &end, NULL)) {
            const gint centered = (width - (right - left)) / 2;
            const gint available = MAX(1, previous + width - TAB_CORNER_PX - end);
            offset = CLAMP(previous + centered - left, 1, available);
        }
    }
    if (offset != previous) {
        gtk_widget_set_size_request(spacer, offset, 0);
    }
    return G_SOURCE_REMOVE;
}

static void tabs_allocated(GtkWidget *notebook, GtkAllocation *allocation, TerminalTabs *tabs) {
    UNUSED(notebook);
    UNUSED(allocation);
    // Changing a size request during allocation can lose the next layout.
    if (!tabs->align_source) {
        tabs->align_source = g_idle_add(align_tab_titles, tabs);
    }
}

static gboolean tab_corner_contains(TerminalTabs *tabs, gdouble x, gdouble y) {
    const gint width = gtk_widget_get_allocated_width(tabs->window);
    return x >= width - TAB_CORNER_PX && x < width && y >= 0 && y < TAB_CORNER_PX;
}

static void tab_bar_press(GtkGestureDrag *gesture, gdouble x, gdouble y, TerminalTabs *tabs) {
    tabs->swipe_from = -1;
    if (panel_is_open()) {
        return;
    }
    GdkEventSequence *seq = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
    const GdkEvent *event = gtk_gesture_get_last_event(GTK_GESTURE(gesture), seq);
    GtkWidget *target = event ? gtk_get_event_widget((GdkEvent *) event) : NULL;
    GtkWidget *notebook = GTK_WIDGET(tabs->notebook);
    if (!target || (target != notebook && !gtk_widget_is_ancestor(target, notebook)
                    && !gtk_widget_is_ancestor(notebook, target))) {
        return;
    }
    if (tab_corner_contains(tabs, x, y)) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        return;
    }
    GtkWidget *terminal = kterm_get_terminal();
    gint top;
    if (terminal && gtk_notebook_get_show_tabs(tabs->notebook)
        && gtk_widget_translate_coordinates(terminal, tabs->window, 0, 0, NULL, &top)
        && y < top && tab_titles_overflow(tabs->notebook)) {
        tabs->swipe_from = gtk_notebook_get_current_page(tabs->notebook);
    }
}

static void tab_bar_release(GtkGestureDrag *gesture, gdouble dx, gdouble dy, TerminalTabs *tabs) {
    GdkEventSequence *seq = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
    if (gtk_gesture_get_sequence_state(GTK_GESTURE(gesture), seq) == GTK_EVENT_SEQUENCE_CLAIMED) {
        // A tap that started in the corner can drift across its edge.
        if (ABS(dx) < 30 && ABS(dy) < 30) {
            gtk_notebook_set_show_tabs(tabs->notebook, !gtk_notebook_get_show_tabs(tabs->notebook));
        } else {
            top_swipe_cb(gesture, dx, dy, NULL);
        }
    } else if (tabs->swipe_from >= 0 && ABS(dx) >= TOUCH_SWIPE_MIN_PX && ABS(dx) >= ABS(dy) * 1.5) {
        const gint last = gtk_notebook_get_n_pages(tabs->notebook) - 1;
        const gint destination = tabs->swipe_from + (dx < 0 ? 1 : -1);
        gtk_notebook_set_current_page(tabs->notebook, CLAMP(destination, 0, last));
    }
}

static GtkWidget *tab_button(const gchar *icon, const gchar *label, gint size) {
    GtkWidget *button = gtk_button_new();
    gtk_widget_set_name(button, "ktermTabButton");
    gtk_widget_set_focus_on_click(button, FALSE);
    gtk_widget_set_tooltip_text(button, label);
    atk_object_set_name(gtk_widget_get_accessible(button), label);
    GtkWidget *image = icon_new(icon, size);
    gtk_container_add(GTK_CONTAINER(button), image);
    gtk_widget_show(image);
    return button;
}

static void save_tab_name(GtkEntry *entry) {
    GtkWidget *terminal = kterm_get_terminal();
    TerminalState *state = terminal_state(terminal);
    if (!state || !panel_showing(gtk_widget_get_parent(GTK_WIDGET(entry)))) {
        return;
    }
    g_free(state->custom_title);
    state->custom_title = g_strstrip(g_strdup(gtk_entry_get_text(entry)));
    if (!*state->custom_title) {
        g_clear_pointer(&state->custom_title, g_free);
    }
    set_tab_title(VTE_TERMINAL(terminal), state->title_label);
    panel_close();
}

static void open_tab_rename(TerminalTabs *tabs, TerminalState *state) {
    if (!tabs->rename_entry) {
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
        GtkWidget *title = gtk_label_new("重命名标签");
        gtk_label_set_xalign(GTK_LABEL(title), 0);
        gtk_box_pack_start(GTK_BOX(page), title, FALSE, FALSE, 0);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "留空恢复自动标题");
        gtk_box_pack_start(GTK_BOX(page), entry, FALSE, FALSE, 0);
        GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING);
        gtk_widget_set_halign(actions, GTK_ALIGN_END);
        GtkWidget *cancel = panel_button_new("取消");
        GtkWidget *save = panel_button_new("保存");
        gtk_box_pack_start(GTK_BOX(actions), cancel, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(actions), save, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), actions, FALSE, FALSE, 0);
        g_signal_connect(cancel, "clicked", G_CALLBACK(panel_close), NULL);
        g_signal_connect_swapped(save, "clicked", G_CALLBACK(save_tab_name), entry);
        g_signal_connect(entry, "activate", G_CALLBACK(save_tab_name), NULL);
        tabs->rename_entry = entry;
        panel_add_page(page, TRUE, PANEL_TAB_NONE);
    }
    GtkWidget *entry = tabs->rename_entry;
    kterm_use_builtin_keyboard();
    gtk_entry_set_text(GTK_ENTRY(entry), gtk_label_get_text(GTK_LABEL(state->title_label)));
    panel_open_page(gtk_widget_get_parent(entry));
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
}

static gboolean widget_contains_point(GtkWidget *widget, GtkWidget *window, gdouble x, gdouble y) {
    gint local_x, local_y;
    return gtk_widget_get_mapped(widget)
        && gtk_widget_translate_coordinates(window, widget, (gint) x, (gint) y, &local_x, &local_y)
        && local_x >= 0 && local_x < gtk_widget_get_allocated_width(widget)
        && local_y >= 0 && local_y < gtk_widget_get_allocated_height(widget);
}

static void tab_rename_pressed(GtkGesture *gesture, gdouble x, gdouble y, TerminalTabs *tabs) {
    if (!gtk_notebook_get_show_tabs(tabs->notebook) || panel_is_open()) {
        return;
    }
    const gint count = gtk_notebook_get_n_pages(tabs->notebook);
    for (gint page = 0; page < count; ++page) {
        TerminalState *state = terminal_state(gtk_notebook_get_nth_page(tabs->notebook, page));
        GtkWidget *title = gtk_widget_get_parent(state->title_label);
        if (widget_contains_point(title, tabs->window, x, y)
            && !widget_contains_point(state->close_button, tabs->window, x, y)) {
            gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
            gtk_notebook_set_current_page(tabs->notebook, page);
            open_tab_rename(tabs, state);
            return;
        }
    }
}

static void tab_rename_held(GtkGestureMultiPress *gesture, gint presses, gdouble x, gdouble y, TerminalTabs *tabs) {
    UNUSED(presses);
    tab_rename_pressed(GTK_GESTURE(gesture), x, y, tabs);
}

static void place_add_tab_button(TerminalTabs *tabs, GtkWidget *header) {
    GtkWidget *parent = gtk_widget_get_parent(tabs->add);
    if (parent) {
        gtk_container_remove(GTK_CONTAINER(parent), tabs->add);
    }
    gtk_box_pack_end(GTK_BOX(header), tabs->add, FALSE, FALSE, 0);
}

static void update_tab_headers(GtkNotebook *notebook) {
    const gint count = gtk_notebook_get_n_pages(notebook);
    for (gint index = 0; index < count; ++index) {
        TerminalState *state = terminal_state(gtk_notebook_get_nth_page(notebook, index));
        gtk_widget_set_visible(state->close_button, count > 1);
    }
    GtkWidget *spacer = gtk_notebook_get_action_widget(notebook, GTK_PACK_START);
    gtk_widget_set_size_request(spacer, 1, 0);
    // Refit before GTK measures the new set of tabs and their buttons.
    size_tab_titles(notebook, count, gtk_widget_get_allocated_width(GTK_WIDGET(notebook)));
}

static void close_tab(GtkWidget *terminal, TerminalTabs *tabs, gboolean child_exited) {
    if (!terminal || gtk_widget_in_destruction(terminal)) {
        return;
    }
    const gint index = gtk_notebook_page_num(tabs->notebook, terminal);
    if (index < 0) {
        return;
    }
    const gint count = gtk_notebook_get_n_pages(tabs->notebook);
    if (count == 1) {
        if (child_exited) {
            gtk_main_quit();
        }
        return;
    }
    if (index == count - 1) {
        GtkWidget *previous = gtk_notebook_get_nth_page(tabs->notebook, index - 1);
        place_add_tab_button(tabs, gtk_notebook_get_tab_label(tabs->notebook, previous));
    }
    // VTE can emit child-exited during disposal. Detach the page first.
    g_object_ref(terminal);
    gtk_notebook_remove_page(tabs->notebook, index);
    gtk_widget_destroy(terminal);
    g_object_unref(terminal);
    update_tab_headers(tabs->notebook);
}

static void close_tab_cb(GtkButton *button, GtkWidget *terminal) {
    UNUSED(button);
    close_tab(terminal, &terminal_tabs, FALSE);
}

static void terminal_exit(VteTerminal *terminal, gint status, TerminalTabs *tabs) {
    UNUSED(status);
    close_tab(GTK_WIDGET(terminal), tabs, TRUE);
}

static void switch_tab(GtkNotebook *notebook, GtkWidget *terminal, guint page, gpointer data) {
    UNUSED(notebook);
    UNUSED(page);
    UNUSED(data);
    gtk_widget_set_can_focus(terminal, TRUE);
    panel_close();
    touch_reset();
    imageview_hide();
    ime_reset_context();
    kterm_set_font_size(conf->font_size);
    kterm_set_text_contrast(conf->text_contrast);
    paste_target_cb(VTE_TERMINAL(terminal), NULL);
    tmuxpick_check();
    refresh_activity();
    gtk_widget_grab_focus(terminal);
}

static void position_notices(GtkWidget *terminal, GtkAllocation *allocation, GtkWidget *window) {
    UNUSED(allocation);
    gint y = 0;
    if (terminal == kterm_get_terminal() && gtk_widget_translate_coordinates(terminal, window, 0, 0, NULL, &y)) {
        notices_set_top(y);
    }
}

static GtkWidget *new_tab(TerminalTabs *tabs) {
    GtkWidget *terminal = vte_terminal_new();
    configure_terminal(terminal);
    gtk_widget_set_name(terminal, "termBox");
    vte_terminal_set_audible_bell(VTE_TERMINAL(terminal), FALSE);

    TerminalState *state = terminal_state(terminal);
    state->number = ++tabs->next_number;
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(title_box, "ktermTabTitle");
    gtk_box_pack_start(GTK_BOX(header), title_box, TRUE, TRUE, 0);
    GtkWidget *label = gtk_label_new(NULL);
    state->title_label = label;
    set_tab_title(VTE_TERMINAL(terminal), label);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
    gtk_box_pack_start(GTK_BOX(title_box), label, TRUE, TRUE, 0);
    GtkWidget *close = tab_button(ICON_CLOSE, "关闭标签", 25);
    state->close_button = close;
    gtk_widget_set_no_show_all(close, TRUE);
    g_signal_connect_object(close, "clicked", G_CALLBACK(close_tab_cb), terminal, 0);
    gtk_box_pack_end(GTK_BOX(title_box), close, FALSE, FALSE, 0);
    place_add_tab_button(tabs, header);
    gtk_widget_show_all(header);

    g_signal_connect(terminal, "child-exited", G_CALLBACK(terminal_exit), tabs);
    g_signal_connect(terminal, "scroll-event", G_CALLBACK(touch_scroll_event), NULL);
    g_signal_connect(terminal, "button-press-event", G_CALLBACK(button_event), NULL);
    g_signal_connect(terminal, "button-release-event", G_CALLBACK(button_event), NULL);
    g_signal_connect(terminal, "motion-notify-event", G_CALLBACK(button_event), NULL);
    g_signal_connect(terminal, "contents-changed", G_CALLBACK(paste_target_cb), NULL);
    g_signal_connect(terminal, "cursor-moved", G_CALLBACK(paste_target_cb), NULL);
    g_signal_connect_object(terminal, "termprop-changed::" VTE_TERMPROP_XTERM_TITLE, G_CALLBACK(title_cb), label, 0);
    g_signal_connect(terminal, "size-allocate", G_CALLBACK(position_notices), tabs->window);
    D g_signal_connect(terminal, "key-release-event", G_CALLBACK(debug_key_event), NULL);
    D g_signal_connect(terminal, "key-press-event", G_CALLBACK(debug_key_event), NULL);
    refresh_attach(terminal);
    flash_attach(terminal);
    tmuxpick_attach(terminal);
    imageview_attach(terminal);

    gtk_widget_show(terminal);
    const gint index = gtk_notebook_append_page(tabs->notebook, terminal, header);
    update_tab_headers(tabs->notebook);
    gtk_notebook_set_current_page(tabs->notebook, index);
    return terminal;
}

static void add_tab_cb(GtkButton *button, TerminalTabs *tabs) {
    UNUSED(button);
    gtk_notebook_set_show_tabs(tabs->notebook, TRUE);
    GtkWidget *terminal = new_tab(tabs);
    gtk_container_check_resize(GTK_CONTAINER(tabs->window));
    GError *error = NULL;
    spawn_terminal(terminal, NULL, tabs->envv, &error);
    if (error) {
        close_tab(terminal, tabs, FALSE);
        error_handle(tabs->window, &error);
    }
}

static gboolean tab_key_cb(GtkWidget *window, GdkEventKey *event, TerminalTabs *tabs) {
    UNUSED(window);
    const guint modifiers = event->state & gtk_accelerator_get_default_mod_mask();
    if (modifiers != (GDK_CONTROL_MASK | GDK_SHIFT_MASK) || panel_wants_keys()) {
        return FALSE;
    }
    switch (gdk_keyval_to_lower(event->keyval)) {
    case GDK_KEY_t:
        add_tab_cb(NULL, tabs);
        return TRUE;
    case GDK_KEY_w:
        close_tab(kterm_get_terminal(), tabs, FALSE);
        return TRUE;
    default:
        return FALSE;
    }
}

static void tabs_init(TerminalTabs *tabs, GtkWidget *window, GtkWidget *box, gchar **envv) {
    GtkWidget *notebook = gtk_notebook_new();
    *tabs = (TerminalTabs) { .window = window, .notebook = GTK_NOTEBOOK(notebook), .envv = envv };
    gtk_widget_set_name(notebook, "ktermTabs");
    gtk_widget_set_can_focus(notebook, FALSE);
    gtk_notebook_set_scrollable(tabs->notebook, TRUE);
    gtk_notebook_set_show_border(tabs->notebook, FALSE);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(spacer, 1, 0);
    gtk_widget_show(spacer);
    gtk_notebook_set_action_widget(tabs->notebook, spacer, GTK_PACK_START);
    g_signal_connect_after(notebook, "size-allocate", G_CALLBACK(tabs_allocated), tabs);
    tabs->add = g_object_ref_sink(tab_button(ICON_NEW_TAB, "新建标签", 32));
    gtk_style_context_add_class(gtk_widget_get_style_context(tabs->add), "add");
    g_object_set_data_full(G_OBJECT(notebook), "kterm-add-tab", tabs->add, g_object_unref);
    g_signal_connect(tabs->add, "clicked", G_CALLBACK(add_tab_cb), tabs);
    g_signal_connect_after(notebook, "switch-page", G_CALLBACK(switch_tab), NULL);
    g_signal_connect(window, "key-press-event", G_CALLBACK(tab_key_cb), tabs);

    gtk_box_pack_start(GTK_BOX(box), notebook, TRUE, TRUE, 0);
    GtkGesture *corner = gtk_gesture_drag_new(window);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(corner), GTK_PHASE_CAPTURE);
    g_signal_connect(corner, "drag-begin", G_CALLBACK(tab_bar_press), tabs);
    g_signal_connect(corner, "drag-end", G_CALLBACK(tab_bar_release), tabs);
    g_object_set_data_full(G_OBJECT(window), "kterm-tab-touch", corner, g_object_unref);

    GtkGesture *rename = gtk_gesture_long_press_new(window);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(rename), GTK_PHASE_CAPTURE);
    g_signal_connect(rename, "pressed", G_CALLBACK(tab_rename_pressed), tabs);
    g_object_set_data_full(G_OBJECT(window), "kterm-tab-rename", rename, g_object_unref);

    GtkGesture *hold = gtk_gesture_multi_press_new(window);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(hold), TOUCH_LONG_PRESS_BUTTON);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(hold), GTK_PHASE_CAPTURE);
    g_signal_connect(hold, "pressed", G_CALLBACK(tab_rename_held), tabs);
    g_object_set_data_full(G_OBJECT(window), "kterm-tab-hold", hold, g_object_unref);
}
#endif

/** main */
gint main(gint argc, gchar **argv) {
    conf = parse_config(); // call first so args overide defaults/config
    
    gint c = -1;
    gint i = 0;
    gchar *command = NULL;
    gchar *envv[TERM_ARGS_MAX] = { NULL };
    gint envc = 0;
#ifdef KINDLE
    // modify buttons style (gtk+ 2)
    inject_gtkrc();
    // set short prompt
    envv[envc++] = "PS1=[\\W]\\$ ";
    // set terminfo path
    envv[envc++] = "TERMINFO=" TERMINFO_PATH;
#endif
    while((c = getopt(argc, argv, "c:de:E:f:hk:l:o:s:t:u:v")) != -1) {
        switch(c) {
            case 'c':
                i = atoi(optarg);
                if ((i == TRUE) | (i == FALSE)) { conf->color_reversed = i; }
                break;
            case 'd':
                debug = TRUE;
                // debug output is usually sent to a file: keep it in step
                setvbuf(stdout, NULL, _IOLBF, 0);
                break;
            case 'e':
                command = optarg;
                break;
            case 'E':
                if (envc < TERM_ARGS_MAX - 1) {
                    envv[envc++] = optarg;
                }
                break;
            case 'f':
                snprintf(conf->font_family, sizeof(conf->font_family), "%s", optarg);
                break;
            case 'h':
                usage();
                break;
            case 'k':
                i = atoi(optarg);
                if ((i == TRUE) | (i == FALSE)) { conf->kb_on = i; }
                break;
            case 'l':
                snprintf(conf->kb_conf_path, sizeof(conf->kb_conf_path), "%s", optarg);
                break;
            case 'o':
                if (optarg[0] == 'U' || optarg[0] == 'R' || optarg[0] == 'L') { conf->orientation = optarg[0]; }
                break;
            case 's':
                i = atoi(optarg);
                if (i > 0) conf->font_size = (guint) i;
                break;
            case 't':
                snprintf(conf->encoding, sizeof(conf->encoding), "%s", optarg);
                break;
            case 'u':
                if (optarg[0] == 'B' || optarg[0] == 'I' || optarg[0] == 'U') { conf->cursor_shape = optarg[0]; }
                break;
            case 'v':
                version();
                break;
        }
    }
    
#ifdef KINDLE
    orientation_init();
#endif
    
    GError *error = NULL;
    gtk_init(&argc, &argv);

#if defined(KINDLE) && GTK_CHECK_VERSION(3,0,0)
    // GTK+ 3 assumes 96 dpi unless an Xft.dpi resource says otherwise, and
    // the Kindle sets none. Derive the real value from the panel size so
    // that point sizes and millimetre based key sizes come out right.
    GdkScreen *default_screen = gdk_screen_get_default();
    const gint screen_width_mm = gdk_screen_get_width_mm(default_screen);
    if (screen_width_mm > 0) {
        const gdouble dpi = gdk_screen_get_width(default_screen) * 25.4 / screen_width_mm;
        g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", (gint) (dpi * 1024), NULL);
    }
#endif

    install_signal_handlers();
    
    // window
    //  \- overlay
    //      \- vbox
    //      |   \- terminal tabs
    //      |   \- shortcut bar, keyboard_box (or, while the system keyboard
    //      |      is up, the bar above it)
    //      \- notice over the terminal's top for text from the phone
    //      \- panel, drawn over the bottom of the window
    //      \- image shown over the whole window (imageview.c)
    //
    // main window
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), TITLE);
#ifdef KINDLE
    // modify buttons style (gtk+ 3)
    inject_styles();
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    // Request screen dimensions before mapping, since maximize is asynchronous.
    GdkScreen *screen = gtk_widget_get_screen(window);
    gtk_window_set_default_size(GTK_WINDOW(window), gdk_screen_get_width(screen), gdk_screen_get_height(screen));
#if GTK_CHECK_VERSION(3,0,0)
    // The Kindle root window is 8-bit StaticGray, which GTK+ 3 cannot draw
    // to. The X server also offers TrueColor visuals. The 8-bit one has the
    // depth of the screen, so what is drawn goes to it as it is. A 32-bit
    // window has to be converted by the server at every change: in the
    // Kindle's dark mode that conversion is about 50 times as slow, and a
    // terminal printing steadily kept the X server busy until the whole
    // device froze. The cost is the gray scale: red, green and blue share
    // the 8 bits, so grays come out in 8 levels (the styles in kindle.c
    // pick colours that land on the right ones). KTERM_VISUAL=32 brings
    // back the 32-bit window.
    GdkVisual *visual = NULL;
    if (g_strcmp0(g_getenv("KTERM_VISUAL"), "32")) {
        GList *visuals = gdk_screen_list_visuals(gtk_widget_get_screen(window));
        for (GList *l = visuals; l && !visual; l = l->next) {
            if (gdk_visual_get_depth(l->data) == 8 && gdk_visual_get_visual_type(l->data) == GDK_VISUAL_TRUE_COLOR) {
                visual = l->data;
            }
        }
        g_list_free(visuals);
    }
    if (!visual) {
        visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(window));
    }
    if (visual) {
        gtk_widget_set_visual(window, visual);
    }
#endif
#endif
    // box
#if GTK_CHECK_VERSION(3,2,0)
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
#else
    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
#endif
    gtk_widget_set_name(vbox, "ktermBox");
#if GTK_CHECK_VERSION(3,2,0)
    // the panel is drawn over the window, at its bottom: the terminal keeps
    // its size, the program in it is not disturbed
    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), vbox);
    gtk_container_add(GTK_CONTAINER(window), overlay);
#else
    gtk_container_add(GTK_CONTAINER(window), vbox);
#endif

#if GTK_CHECK_VERSION(3,0,0)
    // rows of their own heights, see keyboard_set_size()
    GtkWidget *keyboard_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
#else
    GtkWidget *keyboard_box = gtk_vbox_new(TRUE, 0);
#endif
    gtk_widget_set_name(keyboard_box, "kbBox");
    
    Keyboard *keyboard = build_layout(keyboard_box, &error);
    main_keyboard = keyboard;
    if G_UNLIKELY(error) {
        error_handle(window, &error);
        clean_on_exit(keyboard);
        exit(1);
    }
    gtk_box_pack_end(GTK_BOX(vbox), keyboard_box, FALSE, FALSE, 0);
    keyboard_set_size(keyboard);
    
#ifdef KINDLE
    TerminalTabs *tabs = &terminal_tabs;
    tabs_init(tabs, window, vbox, envv);
#else
    GtkWidget *terminal = vte_terminal_new();
    configure_terminal(terminal);
    gtk_widget_set_name(terminal, "termBox");
    desktop_terminal = terminal;
    gtk_box_pack_start(GTK_BOX(vbox), terminal, TRUE, TRUE, 0);
#endif
#if GTK_CHECK_VERSION(3,2,0)
#ifdef KINDLE
    // under the panel: it covers them
    notices_init(overlay);
#endif
    GtkWidget *panel = panel_new(overlay);
    connpage_init();
    settings_init();
#ifdef KINDLE
    watch_top_swipe(panel);
#else
    UNUSED(panel);
#endif
#endif
    
#ifdef KINDLE
    builtin_kb_box = keyboard_box;
    if (ime_init()) {
        ime_panel = build_ime_panel();
        gtk_box_pack_end(GTK_BOX(vbox), ime_panel, FALSE, FALSE, 0);
        if (g_getenv("KTERM_IME_TRACE")) {
            g_signal_connect(window, "key-press-event", G_CALLBACK(ime_key_event), NULL);
        }
    }
#endif
#if GTK_CHECK_VERSION(3,2,0)
    // above whichever keyboard is up
    GtkWidget *shortbar = shortbar_new(SYSCONFDIR "/kterm/" SHORTCUTS_FILE);
    gtk_box_pack_end(GTK_BOX(vbox), shortbar, FALSE, FALSE, 0);
    GtkWidget *system_keyboard_bar = NULL;
#ifdef KINDLE
    system_keyboard_bar = ime_panel;
#endif
    shortbar_follow(keyboard_box, keyboard ? keyboard->row_count : 0, system_keyboard_bar);
    shortpage_init();
#endif

#ifdef KINDLE
    refresh_init(window);
    flash_init(window);
    voice_init();
    tmuxpick_init();
    // over everything, the voice box too
    imageview_init(overlay);
    GtkWidget *terminal = new_tab(tabs);
    kterm_set_tab_hidden(conf->auto_hide_tabs);
#else
    GtkWidget *menu = build_popup(terminal, vbox);
#endif
    // signals
    g_signal_connect(window, "delete_event", G_CALLBACK(gtk_main_quit), NULL);
    // touches on the keyboard between its keys, see keyboard_gap_event()
    gtk_widget_add_events(window, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(window, "button-press-event", G_CALLBACK(keyboard_gap_event), keyboard_box);
    g_signal_connect(window, "button-release-event", G_CALLBACK(keyboard_gap_event), keyboard_box);
#ifndef KINDLE
    g_signal_connect(terminal, "child-exited", G_CALLBACK(terminal_exit), NULL);
    g_signal_connect(terminal, "button-press-event", G_CALLBACK(button_event), menu);
    g_signal_connect(terminal, "button-release-event", G_CALLBACK(button_event), menu);
    g_signal_connect(terminal, "motion-notify-event", G_CALLBACK(button_event), menu);
    g_signal_connect(terminal, "realize", G_CALLBACK(grab_focus), NULL);
    D g_signal_connect(terminal, "key-release-event", G_CALLBACK(debug_key_event), NULL);
    D g_signal_connect(terminal, "key-press-event", G_CALLBACK(debug_key_event), NULL);
#else
    g_object_set(window, "events", GDK_VISIBILITY_NOTIFY_MASK, NULL);
    g_signal_connect(window, "visibility-notify-event", G_CALLBACK(grab_keyboard_cb), NULL);
#endif
    
    gtk_window_maximize(GTK_WINDOW(window));
    gtk_widget_set_no_show_all(keyboard_box, !conf->kb_on);
    gtk_widget_show_all(window);
    gtk_widget_set_no_show_all(keyboard_box, FALSE);
    g_signal_connect(keyboard_box, "size-allocate", G_CALLBACK(keyboard_update), keyboard);
#ifdef KINDLE
    gtk_widget_grab_focus(terminal);
#endif
#ifdef KINDLE
    // Font metrics and the window allocation can still change after show_all.
    // Wait for the first painted grid before giving it to the child.
    const gint exit_status = run_terminal(window, terminal, command, envv);
#else
    spawn_terminal(terminal, command, envv, &error);
    if G_UNLIKELY(error) {
        error_handle(window, &error);
        clean_on_exit(keyboard);
        return 1;
    }
    gtk_main();
    const gint exit_status = 0;
#endif
    
#ifdef KINDLE
    g_clear_handle_id(&tabs->align_source, g_source_remove);
#endif
    clean_on_exit(keyboard);
#ifndef KINDLE
    if (menu) {
        gtk_widget_destroy(menu);
    }
#endif
    D printf("the end\n");
    return exit_status;
}
