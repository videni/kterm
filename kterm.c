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
#include <termios.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <getopt.h>
#include "keyboard.h"
#include "panel.h"
#include "confedit.h"
#include "shortbar.h"
#include "shortpage.h"
#include "shortcuts.h"
#ifdef KINDLE
#include "kindle.h"
#include "ime.h"
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

/**
 * @return The terminal widget
 */
GtkWidget * kterm_get_terminal(void) {
    return main_terminal;
}

/** Process started in the terminal (the shell), 0 if unknown */
static GPid main_child = 0;

pid_t kterm_terminal_foreground(void);

/**
 * @return TRUE if something other than the shell kterm started has the
 *         terminal: a program is running in it (an ssh session, an editor),
 *         and what is typed goes to that program, not to the shell.
 *         FALSE if the shell is waiting for a command, or if it is unknown.
 */
gboolean kterm_terminal_busy(void) {
    return kterm_terminal_foreground() != 0;
}

/**
 * @return Process group that has the terminal when it is not the shell
 *         kterm started; 0 if the shell has it or if it is unknown.
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
 * Signals handler
 * @param signo Signal number
 */
static void exit_on_signal(gint signo) {
    D printf("exiting on signal %i\n", signo);
    if (gtk_main_level()) {
        gtk_main_quit();
    } else {
        exit(0);
    }
}

/**
 * Install signal handlers
 */
static void install_signal_handlers(void) {
    signal(SIGCHLD, SIG_IGN); // kernel should handle zombies
    signal(SIGINT, exit_on_signal);
    signal(SIGQUIT, exit_on_signal);
    signal(SIGTERM, exit_on_signal);
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

/**
 * Reverse color scheme menu callback
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void reverse_colors(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    set_terminal_colors(terminal, !conf->color_reversed);
}

#ifdef KINDLE
/** How far a finger may move, in pixels, and still count as a tap */
#define TAP_SLOP_PX 30
/** A swipe from a screen edge starts within this many pixels of the top */
#define EDGE_TOP_PX 64
/** ... or of the bottom */
#define EDGE_BOTTOM_PX 120
/** and travels at least this far */
#define EDGE_SWIPE_MIN_PX 96
/** Built-in keyboard container */
static GtkWidget *builtin_kb_box = NULL;
/** Reading: the keyboard stays folded away, a tap on the terminal does not
 *  bring it back. Left from the panel, or by anything that shows a keyboard. */
static gboolean read_mode = FALSE;

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
    if (ime_is_open()) {
        kterm_toggle_ime();
    }
    if (builtin_kb_box && !conf->kb_on) {
        conf->kb_on = TRUE;
        gtk_widget_show(builtin_kb_box);
    }
}

/**
 * Touch handler of the bar above the system keyboard: a swipe up from it
 * opens the panel, as a swipe up from the built-in keyboard does. The
 * system keyboard itself is another program's window, swipes on it never
 * reach us.
 * @return FALSE, the bar's button still works as a button
 */
static gboolean ime_bar_event(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    UNUSED(widget);
    UNUSED(data);
    static gdouble press_y = 0;
    if (event->button != 1) {
        return FALSE;
    }
    if (event->type == GDK_BUTTON_PRESS) {
        press_y = event->y_root;
    } else if (event->type == GDK_BUTTON_RELEASE && press_y - event->y_root >= KB_SWIPE_MIN_PX) {
        panel_open();
    }
    return FALSE;
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
    g_signal_connect(back, "button-press-event", G_CALLBACK(ime_bar_event), NULL);
    g_signal_connect(back, "button-release-event", G_CALLBACK(ime_bar_event), NULL);
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
#endif

/**
 * Keyboard widget size allocation signal handler.
 * Updates keyboard size
 * @param keyboard_box Keyboard widget
 * @param alloc Size allocation
 * @param keyboard Keyboard structure
 */
static void keyboard_update(GtkWidget *keyboard_box, GtkAllocation *alloc, Keyboard *keyboard) {
    UNUSED(keyboard_box);
    GdkScreen *screen = gdk_screen_get_default();
    gint screen_height = gdk_screen_get_height(screen);
    static gint saved_width = -1;
    static gint saved_height = -1;
    if (conf->kb_on && alloc && (alloc->width != saved_width || screen_height != saved_height)) {
        D printf("set keyboard size: %ix%i\n", alloc->width, alloc->height);
        g_idle_add(keyboard_set_size, keyboard);
        saved_width = alloc->width;
        saved_height = screen_height;
    }
}

#ifdef KINDLE
/**
 * Rotate screen menu callback
 * @param widget Calling widget
 * @param box Kterm container
 */
static void screen_rotate(GtkWidget *widget, gpointer box) {
    UNUSED(widget);
    UNUSED(box);
    char request = 0;
    if (conf->orientation == 'U') {
        request = 'R';
    } else {
        request = 'U';
    }
    if (set_orientation(request)) {
        conf->orientation = request;
    }
}
#endif

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
#ifdef KINDLE
            read_mode = FALSE;
#endif
        }
    }
}

#ifdef KINDLE
/**
 * Wrapper for g_signal_handlers_disconnect_by_func()
 * The only reason is to avoid warnings for converting a function pointer to a void pointer.
 * FIXME: is there a better way?
 * @param instance: The instance to remove handlers from.
 * @param func: The C closure callback of the handlers (useless for non-C closures).
 * @param data: The closure data of the handlers' closures.
 * @return The number of handlers that matched.
 */
static guint signal_handlers_disconnect_by_func(gpointer instance, GCallback func, gpointer data) {
    return g_signal_handlers_disconnect_by_func(instance, *(gpointer*)&func, data);
}

/**
 * Callback on menu deactivated signal
 * @param widget Calling widget
 * @param data User data
 */
static void menu_deactivate_cb(GtkWidget *widget, gpointer data) {
    UNUSED(widget);
    D printf("Menu deactivated\n");
    GdkEventButton *event = data;
    signal_handlers_disconnect_by_func(widget, G_CALLBACK(menu_deactivate_cb), data);
    gdk_test_simulate_button(event->window, (gint) event->x, (gint) event->y, 1, event->state, GDK_BUTTON_RELEASE);
    gdk_event_free((GdkEvent *) event);
}
#endif

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
#ifdef KINDLE
    GtkWidget *rotate_item = gtk_menu_item_new_with_label("Screen rotate");
#endif
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), fontup_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), fontdown_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), color_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), kb_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), reset_item);
#ifdef KINDLE
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), rotate_item);
#endif
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
    
    
    g_signal_connect(G_OBJECT(fontup_item), "activate", G_CALLBACK(fontup), (gpointer) terminal);
    g_signal_connect(G_OBJECT(fontdown_item), "activate", G_CALLBACK(fontdown), (gpointer) terminal);
    g_signal_connect(G_OBJECT(color_item), "activate", G_CALLBACK(reverse_colors), (gpointer) terminal);
    g_signal_connect(G_OBJECT(kb_item), "activate", G_CALLBACK(toggle_keyboard), box);
    g_signal_connect(G_OBJECT(reset_item), "activate", G_CALLBACK(reset_terminal), (gpointer) terminal);
#ifdef KINDLE
    g_signal_connect(G_OBJECT(rotate_item), "activate", G_CALLBACK(screen_rotate), box);
#endif
    g_signal_connect(G_OBJECT(quit_item), "activate", G_CALLBACK(gtk_main_quit), NULL);
    
    gtk_widget_show_all(menu);
    return menu;
}

#if GTK_CHECK_VERSION(3,2,0)
#ifdef KINDLE
static void panel_read_label_cb(GtkWidget *panel, gpointer read_button) {
    UNUSED(panel);
    gtk_button_set_label(GTK_BUTTON(read_button), read_mode ? "退出阅读" : "阅读");
}

/**
 * The panel's reading button. Reading mode folds every keyboard away and
 * keeps it away: touches on the terminal only scroll, switch tmux windows
 * or are passed to the program. Pressed again, the keyboard is back.
 */
static void panel_read_clicked_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    UNUSED(data);
    if (read_mode) {
        kterm_use_builtin_keyboard();
        return;
    }
    if (ime_is_open()) {
        kterm_toggle_ime();
    }
    kterm_hide_keyboard();
    read_mode = TRUE;
}

/** The panel's reading button */
static GtkWidget *panel_read_button = NULL;
#endif

/** Font sizes offered on the settings page */
#define FONT_SIZE_MIN 4
#define FONT_SIZE_MAX 30

/**
 * Font size buttons of the settings page: the new size is used at once and
 * written to kterm.conf for the next start.
 * @param button Calling widget, carries the step (-1 or 1)
 * @param label Shows the size
 */
static void font_size_cb(GtkWidget *button, gpointer label) {
    const gint step = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "step"));
    const gint size = CLAMP((gint) conf->font_size + step, FONT_SIZE_MIN, FONT_SIZE_MAX);
    if (size == (gint) conf->font_size || !main_terminal) {
        return;
    }
    conf->font_size = (guint) size;
    set_terminal_font(VTE_TERMINAL(main_terminal), conf->font_family, size);
    gchar *text = g_strdup_printf("%d", size);
    gtk_label_set_text(GTK_LABEL(label), text);
    g_free(text);
    gchar *line = g_strdup_printf("font_size = %d", size);
    GError *error = NULL;
    if (!confedit_save(CONFIG_FULL_PATH, "font_size", line, &error)) {
        D printf("font size not saved: %s\n", error->message);
        g_error_free(error);
    }
    g_free(line);
}

/**
 * @param text Button text
 * @param step Font size change
 * @param label Shows the size
 * @return Button
 */
static GtkWidget * font_size_button(const gchar *text, gint step, GtkWidget *label) {
    GtkWidget *button = panel_button_new(text);
    g_object_set_data(G_OBJECT(button), "step", GINT_TO_POINTER(step));
    g_signal_connect(button, "clicked", G_CALLBACK(font_size_cb), label);
    return button;
}

/**
 * Fill the panel: actions on its first page, the settings page
 * @param box Kterm container
 */
static void add_panel_actions(GtkWidget *box) {
#ifdef KINDLE
    GtkWidget *read_button = panel_button_new("阅读");
    panel_read_button = read_button;
    g_signal_connect(read_button, "clicked", G_CALLBACK(panel_read_clicked_cb), NULL);
    g_signal_connect_swapped(read_button, "clicked", G_CALLBACK(panel_close), NULL);
    panel_add_action(read_button, -1);
    GtkWidget *rotate_button = panel_button_new("旋转");
    g_signal_connect_swapped(rotate_button, "clicked", G_CALLBACK(panel_close), NULL);
    g_signal_connect(rotate_button, "clicked", G_CALLBACK(screen_rotate), box);
    panel_add_action(rotate_button, -1);
#else
    UNUSED(box);
#endif
    GtkWidget *quit_button = panel_button_new("退出");
    g_signal_connect(quit_button, "clicked", G_CALLBACK(gtk_main_quit), NULL);
    panel_add_action(quit_button, -1);

    GtkWidget *font_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gchar *size = g_strdup_printf("%u", conf->font_size);
    GtkWidget *size_label = gtk_label_new(size);
    g_free(size);
    gtk_label_set_width_chars(GTK_LABEL(size_label), 3);
    gtk_box_pack_start(GTK_BOX(font_row), font_size_button("−", -1, size_label), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(font_row), size_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(font_row), font_size_button("+", 1, size_label), FALSE, FALSE, 0);
    panel_add_setting("字号", font_row);
}
#endif

/**
 * Mouse button event callback
 * @param terminal Terminal widget
 * @param event Button event
 * @param box Kterm container
 * @return True to stop processing event, false otherwise
 */
#ifdef KINDLE
/** Most wheel steps sent for one swipe */
#define SWIPE_WHEEL_MAX 60
/** Set while terminal_wheel() moves VTE's idea of the pointer: that one
 *  motion event has to get through to VTE */
static gboolean pointer_placing = FALSE;

/**
 * Send mouse wheel steps to the terminal, as a mouse over it would.
 * VTE passes them on to a program that asked for the mouse (tmux, less,
 * vim), and scrolls its own history otherwise.
 * @param terminal Terminal widget
 * @param source Touch event that caused it (window, device, time)
 * @param direction GDK_SCROLL_UP or GDK_SCROLL_DOWN
 * @param x,y Position in the terminal, pixels
 * @param steps How many
 */
static void terminal_wheel(GtkWidget *terminal, const GdkEventButton *source, GdkScrollDirection direction,
                           gdouble x, gdouble y, guint steps) {
    // VTE reports the wheel at the place it last saw the pointer, not at
    // the place the wheel event names: show it the pointer there first.
    GdkEvent *move = gdk_event_new(GDK_MOTION_NOTIFY);
    move->motion.window = g_object_ref(source->window);
    move->motion.send_event = TRUE;
    move->motion.time = source->time;
    move->motion.x = x;
    move->motion.y = y;
    move->motion.x_root = source->x_root - source->x + x;
    move->motion.y_root = source->y_root - source->y + y;
    move->motion.state = 0;
    gdk_event_set_device(move, gdk_event_get_device((const GdkEvent *) source));
    pointer_placing = TRUE;
    gtk_widget_event(terminal, move);
    pointer_placing = FALSE;
    gdk_event_free(move);
    for (guint i = 0; i < steps && i < SWIPE_WHEEL_MAX; i++) {
        GdkEvent *wheel = gdk_event_new(GDK_SCROLL);
        wheel->scroll.window = g_object_ref(source->window);
        wheel->scroll.send_event = TRUE;
        wheel->scroll.time = source->time;
        wheel->scroll.x = x;
        wheel->scroll.y = y;
        wheel->scroll.x_root = source->x_root - source->x + x;
        wheel->scroll.y_root = source->y_root - source->y + y;
        wheel->scroll.state = 0;
        wheel->scroll.direction = direction;
        gdk_event_set_device(wheel, gdk_event_get_device((const GdkEvent *) source));
        gtk_widget_event(terminal, wheel);
        gdk_event_free(wheel);
    }
}

/**
 * @return TRUE if the terminal keeps lines scrolled off its top: then a
 *         shell is in it, not a full screen program such as tmux (those
 *         use the alternate screen, which has no history).
 */
static gboolean terminal_has_history(GtkWidget *terminal) {
    GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(terminal));
    return gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment)
           > gtk_adjustment_get_lower(adjustment);
}

/**
 * A finger moved up or down the terminal: move the text with it.
 * @param terminal Terminal widget
 * @param source Release event of the touch
 * @param x,y Where the touch started, in the terminal
 * @param dy Distance moved, pixels; negative is up
 */
static void terminal_swipe_scroll(GtkWidget *terminal, const GdkEventButton *source, gdouble x, gdouble y, gdouble dy) {
    const glong cell = vte_terminal_get_char_height(VTE_TERMINAL(terminal));
    const gint lines = cell > 0 ? (gint) (ABS(dy) / cell + 0.5) : 0;
    if (lines < 1) {
        return;
    }
    GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(terminal));
    const gdouble lower = gtk_adjustment_get_lower(adjustment);
    const gdouble top_max = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
    if (terminal_has_history(terminal)) {
        // the terminal's own history: exactly as far as the finger went
        const gdouble value = gtk_adjustment_get_value(adjustment) + (dy < 0 ? lines : -lines);
        gtk_adjustment_set_value(adjustment, CLAMP(value, lower, top_max));
        return;
    }
    // No history here: a full screen program (tmux, less) keeps its own.
    // Finger up shows what is below, which is the wheel turning down.
    terminal_wheel(terminal, source, dy < 0 ? GDK_SCROLL_DOWN : GDK_SCROLL_UP, x, y, (guint) lines);
}

/**
 * Type a tmux command key: the prefix from the configuration (tmux_prefix,
 * "C-b" unless set), then the key.
 * @param terminal Terminal widget
 * @param key Key after the prefix, e.g. "n"
 * @return FALSE if the configured prefix is not understood
 */
static gboolean tmux_send(GtkWidget *terminal, const gchar *key) {
    const gchar *prefix = conf->tmux_prefix;
    gchar code;
    if (g_ascii_strncasecmp(prefix, "C-", 2) != 0) {
        return FALSE;
    }
    if (!g_ascii_strcasecmp(prefix + 2, "Space")) {
        code = '\0';
    } else if (prefix[2] && !prefix[3] && g_ascii_isalpha(prefix[2])) {
        code = (gchar) (g_ascii_toupper(prefix[2]) & 0x1f);
    } else {
        return FALSE;
    }
    D printf("tmux: prefix %s + %s\n", prefix, key);
    vte_terminal_feed_child(VTE_TERMINAL(terminal), &code, 1);
    vte_terminal_feed_child(VTE_TERMINAL(terminal), key, -1);
    return TRUE;
}

/**
 * A finger moved left or right in reading mode: next or previous tmux
 * window. Nothing is typed into a plain shell.
 * @param terminal Terminal widget
 * @param dx Distance moved, pixels; negative is left
 */
static void terminal_swipe_switch(GtkWidget *terminal, gdouble dx) {
    if (terminal_has_history(terminal)) {
        return;
    }
    // towards the left brings the next one in, like turning a page
    tmux_send(terminal, dx < 0 ? "n" : "p");
}
#endif

static gboolean button_event(GtkWidget *terminal, GdkEventButton *event, gpointer menu) {
    D printf("event-type: %i\n", event->type);
    D printf("event-button: %i\n", event->button);
    D {
        GtkWidget *grab = gtk_grab_get_current();
        printf("event-at: %.0f,%.0f (widget %.0f,%.0f) gtk-grab: %s\n", event->x_root, event->y_root,
               event->x, event->y, grab ? gtk_widget_get_name(grab) : "none");
    }
#ifdef KINDLE
    // ignore any motion events (i think we don't need selecting text as one finger motion scrolls buffer)
    if (event->type == GDK_MOTION_NOTIFY) { return !pointer_placing; }
#endif
#ifdef KINDLE
    // The touch driver clicks button 3 during a swipe, while button 1 is
    // still down: that touch is scrolling, whatever distance it ends at.
    static gboolean swiping = FALSE;
    if (event->button == 3) {
        if (event->type == GDK_BUTTON_PRESS) {
            swiping = TRUE;
        }
        // It is a mark, not a click: passed on, tmux and others would
        // open their context menu at every swipe.
        return TRUE;
    }
    if (event->button == 1) {
        static gdouble press_x = 0, press_y = 0, press_in_terminal_x = 0, press_in_terminal_y = 0;
        if (event->type == GDK_BUTTON_PRESS) {
            swiping = FALSE;
            press_x = event->x_root;
            press_y = event->y_root;
            press_in_terminal_x = event->x;
            press_in_terminal_y = event->y;
        } else if (event->type == GDK_BUTTON_RELEASE) {
            const gdouble dx = event->x_root - press_x;
            const gdouble dy = event->y_root - press_y;
            const gboolean vertical = ABS(dy) >= EDGE_SWIPE_MIN_PX && ABS(dy) >= ABS(dx) * 1.5;
            if (vertical && dy > 0 && press_y <= EDGE_TOP_PX) {
                // Down from the top edge is the device's own gesture for its
                // quick settings (light, dark mode, airplane mode). A full
                // screen application has to pass it on.
                show_system_menu();
                return TRUE;
            }
            if (vertical && dy < 0 && (!conf->kb_on || ime_is_open())
                && press_in_terminal_y >= gtk_widget_get_allocated_height(terminal) - EDGE_BOTTOM_PX) {
                // No keyboard of ours to swipe up from: it is folded away,
                // or the system keyboard (another program's window) is in
                // its place. The swipe then starts at the foot of the
                // terminal.
                panel_open();
                return TRUE;
            }
            const gboolean horizontal = ABS(dx) >= EDGE_SWIPE_MIN_PX && ABS(dx) >= ABS(dy) * 1.5;
            if (vertical && !panel_is_open()) {
                terminal_swipe_scroll(terminal, event, press_in_terminal_x, press_in_terminal_y, dy);
            } else if (horizontal && read_mode && !panel_is_open()) {
                terminal_swipe_switch(terminal, dx);
            }
            if (!vertical && !swiping && ABS(dx) < TAP_SLOP_PX && ABS(dy) < TAP_SLOP_PX && !read_mode
                && !panel_is_open() && !conf->kb_on && builtin_kb_box && !ime_is_open()) {
                // A tap on the terminal brings a hidden keyboard back, as
                // tapping a text field does elsewhere on the device. Not
                // while reading: there a tap belongs to the program (tmux
                // switches to the window whose name is tapped).
                conf->kb_on = TRUE;
                gtk_widget_show(builtin_kb_box);
            }
        }
    }
#endif
    if (event->button == 1 && event->type == GDK_BUTTON_RELEASE && panel_is_open()) {
        // a touch outside the panel puts it away
        panel_close();
        return TRUE;
    }
    if (event->button == BUTTON_MENU) {
#ifdef KINDLE
        // ignore button click to disable paste (quite messy on kindle)
        if (event->type == GDK_BUTTON_PRESS) { return TRUE; }
        guint button = 0;
        // emit button 1 release on deactivate, otherwise vte enters selection mode
        GdkEventButton *event_copy = (GdkEventButton *) gdk_event_copy((GdkEvent *) event);
        g_signal_connect(menu, "deactivate", G_CALLBACK(menu_deactivate_cb), event_copy);
#else
        if (event->type == GDK_BUTTON_RELEASE) { return FALSE; }
        guint button = event->button;
#endif

        gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, button, event->time);
        return TRUE;
    }
    return FALSE;
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
    //  \- vbox
    //      \- terminal  \- keyboard_box
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
    // to. The X server also offers a 32-bit TrueColor visual; use that and
    // let the server convert to gray.
    GdkVisual *rgba_visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(window));
    if (rgba_visual) {
        gtk_widget_set_visual(window, rgba_visual);
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
    // the panel is drawn over the whole window: over the keyboard while it
    // is open, so that the terminal keeps its size and stays in view
    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), vbox);
    gtk_container_add(GTK_CONTAINER(window), overlay);
#else
    gtk_container_add(GTK_CONTAINER(window), vbox);
#endif

#if GTK_CHECK_VERSION(3,0,0)
    GtkWidget *keyboard_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(keyboard_box), TRUE);
#else
    GtkWidget *keyboard_box = gtk_vbox_new(TRUE, 0);
#endif
    gtk_widget_set_name(keyboard_box, "kbBox");
    
    Keyboard *keyboard = build_layout(keyboard_box, &error);
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
    add_panel_actions(vbox);
#ifdef KINDLE
    g_signal_connect(panel, "show", G_CALLBACK(panel_read_label_cb), panel_read_button);
#endif
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), panel);
    panel_add_bottom(keyboard_box, TRUE);
#endif
    
#ifdef KINDLE
    builtin_kb_box = keyboard_box;
    if (ime_init(terminal)) {
        ime_panel = build_ime_panel();
        gtk_box_pack_end(GTK_BOX(vbox), ime_panel, FALSE, FALSE, 0);
        g_signal_connect(window, "key-press-event", G_CALLBACK(ime_key_event), NULL);
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
    shortbar_follow(shortbar, keyboard_box, keyboard->row_count, system_keyboard_bar);
    panel_add_bottom(shortbar, TRUE);
    panel_add_bottom(system_keyboard_bar, FALSE);
    shortpage_init();
#endif

    GtkWidget *menu = build_popup(terminal, vbox);
    // signals
    g_signal_connect(window, "delete_event", G_CALLBACK(gtk_main_quit), NULL);
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
    gtk_main();
    
    clean_on_exit(keyboard);
    gtk_widget_destroy(menu);
    D printf("the end\n");
    return 0;
}
