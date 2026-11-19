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
/** The terminal widget */
static GtkWidget *main_terminal = NULL;
/** The built-in keyboard */
static Keyboard *main_keyboard = NULL;

/**
 * @return The terminal widget
 */
GtkWidget * kterm_get_terminal(void) {
    return main_terminal;
}

/** Process started in the terminal, 0 if unknown */
static GPid main_child = 0;
/** TRUE if that process is a shell, FALSE for a program given with -e */
static gboolean main_child_is_shell = FALSE;

/**
 * @return TRUE if kterm started a shell in the terminal; FALSE if it runs a
 *         program given with -e (tmux, ssh...), into which commands meant
 *         for a shell must not be typed
 */
gboolean kterm_terminal_has_shell(void) {
    return main_child_is_shell;
}

/**
 * @return Process group that has the terminal when it is not the process
 *         kterm started: a program is running in the shell (an ssh
 *         session, an editor) and what is typed goes to it. 0 if the
 *         process kterm started has it, or if it is unknown.
 */
pid_t kterm_terminal_foreground(void) {
    if (!main_terminal || main_child <= 0) {
        return 0;
    }
    VtePty *pty = vte_terminal_get_pty(VTE_TERMINAL(main_terminal));
    if (!pty) {
        return 0;
    }
    const pid_t foreground = tcgetpgrp(vte_pty_get_fd(pty));
    return (foreground > 0 && foreground != (pid_t) main_child) ? foreground : 0;
}

/**
 * @return Process group that has the terminal, whichever it is: the
 *         process kterm started too. 0 if unknown.
 */
pid_t kterm_terminal_owner(void) {
    VtePty *pty = main_terminal ? vte_terminal_get_pty(VTE_TERMINAL(main_terminal)) : NULL;
    const pid_t owner = pty ? tcgetpgrp(vte_pty_get_fd(pty)) : -1;
    return owner > 0 ? owner : 0;
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
 * Type a command into the shell in the terminal and run it: ^E ^U first,
 * as anything half typed on the command line would otherwise run together
 * with it
 * @param command Command
 */
void kterm_type_command(const gchar *command) {
    gchar *line = g_strconcat("\x05\x15", command, "\r", NULL);
    vte_terminal_feed_child(VTE_TERMINAL(main_terminal), line, -1);
    g_free(line);
}

/**
 * @return TRUE if the terminal keeps lines scrolled off its top: then a
 *         shell is in it, not a full screen program such as tmux (those
 *         use the alternate screen, which has no history).
 */
gboolean kterm_terminal_has_history(void) {
    GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(main_terminal));
    return gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj) > gtk_adjustment_get_lower(adj);
}

/**
 * @return TRUE if the terminal shows earlier lines of its history, not its
 *         last ones
 */
gboolean kterm_terminal_scrolled(void) {
    GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(main_terminal));
    return gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) < gtk_adjustment_get_upper(adj) - 0.5;
}

/**
 * Show the terminal's last lines again
 */
void kterm_terminal_to_bottom(void) {
    GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(main_terminal));
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
 * Install signal handlers
 */
static void install_signal_handlers(void) {
    signal(SIGCHLD, SIG_IGN); // kernel should handle zombies
    g_unix_signal_add(SIGINT, exit_on_signal, NULL);
    g_unix_signal_add(SIGTERM, exit_on_signal, NULL);
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
    // light background
    GdkRGBA palette_light[8] = {{ 0, 0, 0, 1 }, // black
        { 0x5050/0xffff, 0x5050/0xffff, 0x5050/0xffff, 1 }, // red
        { 0x7070/0xffff, 0x7070/0xffff, 0x7070/0xffff, 1 }, // green
        { 0xa0a0/0xffff, 0xa0a0/0xffff, 0xa0a0/0xffff, 1 }, // yellow
        { 0x8080/0xffff, 0x8080/0xffff, 0x8080/0xffff, 1 }, // blue
        { 0x3030/0xffff, 0x3030/0xffff, 0x3030/0xffff, 1 }, // magenta
        { 0x9090/0xffff, 0x9090/0xffff, 0x9090/0xffff, 1 }, // cyan
        { 1, 1, 1, 1 }}; // white
    // dark background
    GdkRGBA palette_dark[8] = {{ 0, 0, 0, 1 },
        { 0x8888/0xffff, 0x8888/0xffff, 0x8888/0xffff, 1 },
        { 0x9898/0xffff, 0x9898/0xffff, 0x9898/0xffff, 1 },
        { 0xd0d0/0xffff, 0xd0d0/0xffff, 0xd0d0/0xffff, 1 },
        { 0xa8a8/0xffff, 0xa8a8/0xffff, 0xa8a8/0xffff, 1 },
        { 0x7070/0xffff, 0x7070/0xffff, 0x7070/0xffff, 1 },
        { 0xb8b8/0xffff, 0xb8b8/0xffff, 0xb8b8/0xffff, 1 },
        { 1, 1, 1, 1 }};
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
/** Swipe down from the top edge over the panel, see panel_top_swipe_cb() */
static GtkGesture *panel_top_swipe = NULL;

/**
 * A swipe down from the top edge while the panel is open there: the panel
 * is in the way of the device's own gesture for its menu, which the
 * terminal passes on. The panel goes away and the menu comes.
 * @param drag Gesture on the panel, seen before its buttons
 * @param dx Horizontal travel
 * @param dy Vertical travel, down positive
 * @param data Not used
 */
static void panel_top_swipe_cb(GtkGestureDrag *drag, gdouble dx, gdouble dy, gpointer data) {
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
/**
 * Set the terminal's font size, as from the settings page
 * @param size Points
 */
void kterm_set_font_size(guint size) {
    conf->font_size = size;
    if (main_terminal) {
        set_terminal_font(VTE_TERMINAL(main_terminal), conf->font_family, (gint) size);
    }
}

/**
 * Fill the panel: the quit action on its first page, the settings page
 */
static void add_panel_actions(void) {
    GtkWidget *quit_button = panel_button_with_icon("退出", ICON_LOGOUT);
    g_signal_connect(quit_button, "clicked", G_CALLBACK(gtk_main_quit), NULL);
    panel_add_action(quit_button, -1);
    settings_init();
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
    gboolean wanted = vte_terminal_get_bracketed_paste(terminal);
    const char *title = vte_terminal_get_termprop_string_by_id(terminal, VTE_PROPERTY_ID_XTERM_TITLE, NULL);
    const char *mark = title ? strstr(title, PASTE_TITLE_MARK) : NULL;
    // a title left behind by a tmux no longer there does not count
    if (mark && tmux_may_have_terminal()) {
        wanted = mark[strlen(PASTE_TITLE_MARK)] == '1';
    }
    shortbar_paste_wanted(wanted);
}

/**
 * The terminal's title changed, see paste_target_cb()
 */
static void title_cb(VteTerminal *terminal, const char *name, gpointer data) {
    UNUSED(name);
    paste_target_cb(terminal, data);
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

/**
 * Setup terminal
 * @param terminal Terminal
 * @param command Command passed to terminal, null if none
 * @param envv Null terminated array of env variable=value pairs passed to terminal
 * @param error Set on error, null otherwise
 */
static void setup_terminal(GtkWidget *terminal, gchar *command, gchar **envv, GError **error) {
    gchar *argv[TERM_ARGS_MAX] = { NULL };
    gint argc = 0;
    gchar *shell = NULL;
#if VTE_CHECK_VERSION(0,25,1)
    if (!command || *command == '\0') {
        // prepend args with shell
        if ((shell = vte_get_user_shell()) == NULL) {
            shell = g_strdup("/bin/sh");
        }
        argv[argc++] = shell;
    }
#endif
    if (command) {
        gchar *argbuf = strtok(command, " ");
        while (argbuf != NULL && argc < (TERM_ARGS_MAX - 1)) {
            argv[argc++] = argbuf;
            argbuf = strtok(NULL, " ");
        }
    }
    if (argv[0]) {
        static const gchar *shells[] = { "sh", "ash", "bash", "dash", "ksh", "mksh", "zsh", "fish" };
        gchar *name = g_path_get_basename(argv[0]);
        for (guint i = 0; i < G_N_ELEMENTS(shells); i++) {
            main_child_is_shell |= !strcmp(name, shells[i]);
        }
        g_free(name);
    }

    set_terminal_colors(terminal, conf->color_reversed);
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
    
#if VTE_CHECK_VERSION(0,38,0)
    vte_terminal_spawn_sync(VTE_TERMINAL(terminal), 0, NULL, argv, envv, G_SPAWN_SEARCH_PATH, NULL, NULL, &main_child, NULL, error);
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
    //      |   \- terminal
    //      |   \- shortcut bar, keyboard_box (or, while the system keyboard
    //      |      is up, the bar above it)
    //      \- panel, drawn over the bottom of the window
    //
    // main window
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), TITLE);
#ifdef KINDLE
    // modify buttons style (gtk+ 3)
    inject_styles();
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
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
    
    GtkWidget *terminal = vte_terminal_new();
    setup_terminal(terminal, command, envv, &error);
    if G_UNLIKELY(error) {
        error_handle(window, &error);
        clean_on_exit(keyboard);
        exit(1);
    }
    gtk_widget_set_name(terminal, "termBox");
    main_terminal = terminal;
    gtk_box_pack_start(GTK_BOX(vbox), terminal, TRUE, TRUE, 0);
#if GTK_CHECK_VERSION(3,2,0)
    GtkWidget *panel = panel_new(terminal);
    connpage_init(terminal);
    add_panel_actions();
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), panel);
#ifdef KINDLE
    panel_top_swipe = gtk_gesture_drag_new(panel);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(panel_top_swipe), GTK_PHASE_CAPTURE);
    g_signal_connect(panel_top_swipe, "drag-end", G_CALLBACK(panel_top_swipe_cb), NULL);
#endif
#endif
    
#ifdef KINDLE
    builtin_kb_box = keyboard_box;
    if (ime_init(terminal)) {
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

    // on the Kindle a second finger is the right button, see touch_event()
#ifdef KINDLE
    GtkWidget *menu = NULL;
    g_signal_connect(terminal, "scroll-event", G_CALLBACK(touch_scroll_event), NULL);
    g_signal_connect(terminal, "contents-changed", G_CALLBACK(paste_target_cb), NULL);
    g_signal_connect(terminal, "cursor-moved", G_CALLBACK(paste_target_cb), NULL);
    g_signal_connect(terminal, "termprop-changed::" VTE_TERMPROP_XTERM_TITLE, G_CALLBACK(title_cb), NULL);
#else
    GtkWidget *menu = build_popup(terminal, vbox);
#endif
    // signals
    g_signal_connect(window, "delete_event", G_CALLBACK(gtk_main_quit), NULL);
    // touches on the keyboard between its keys, see keyboard_gap_event()
    gtk_widget_add_events(window, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(window, "button-press-event", G_CALLBACK(keyboard_gap_event), keyboard_box);
    g_signal_connect(window, "button-release-event", G_CALLBACK(keyboard_gap_event), keyboard_box);
    g_signal_connect(terminal, "child-exited", G_CALLBACK(terminal_exit), NULL);
    g_signal_connect(terminal, "button-press-event", G_CALLBACK(button_event), menu);
    g_signal_connect(terminal, "button-release-event", G_CALLBACK(button_event), menu);
    g_signal_connect(terminal, "motion-notify-event", G_CALLBACK(button_event), menu);
    g_signal_connect(terminal, "realize", G_CALLBACK(grab_focus), NULL);
#ifdef KINDLE
    g_object_set(window, "events", GDK_VISIBILITY_NOTIFY_MASK, NULL);
    g_signal_connect(window, "visibility-notify-event", G_CALLBACK(grab_keyboard_cb), NULL);
#endif
    D g_signal_connect(terminal, "key-release-event", G_CALLBACK(debug_key_event), NULL);
    D g_signal_connect(terminal, "key-press-event", G_CALLBACK(debug_key_event), NULL);
    
    gtk_widget_show_all(window);
    if (!conf->kb_on) {
        gtk_widget_hide(keyboard_box);
    }
    g_signal_connect(keyboard_box, "size-allocate", G_CALLBACK(keyboard_update), keyboard);
    gtk_window_maximize(GTK_WINDOW(window));
#ifdef KINDLE
    refresh_init(window, terminal);
    flash_init(window, terminal);
    voice_init(overlay, terminal, shortbar);
    tmuxpick_init(terminal);
#endif
    gtk_main();
    
    clean_on_exit(keyboard);
    if (menu) {
        gtk_widget_destroy(menu);
    }
    D printf("the end\n");
    return 0;
}
