/* touch.c
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
 * Fingers on the terminal, on the Kindle: taps, swipes (scrolling, tmux
 * windows while previewing, the device's menu from the top edge), long
 * presses selecting text, two fingers for the right button.
 */

#include <string.h>
#include <vte/vte.h>
#include "config.h"
#include "flash.h"
#include "kindle.h"
#include "kterm.h"
#include "shortbar.h"
#include "imageview.h"
#include "tmux.h"
#include "touch.h"

extern KTconf *conf;

/** How far a finger may move, in pixels, and still count as a tap */
#define TAP_SLOP_PX 30

/** Most wheel steps sent for one swipe */
#define SWIPE_WHEEL_MAX 60
/** Set while an event made here is given to VTE: touch_event() lets it
 *  through */
static gboolean forwarding = FALSE;

/**
 * Show VTE the pointer at a place in the terminal
 * @param terminal Terminal widget
 * @param source Touch event it stands for (window, device, time)
 * @param x,y Position in the terminal, pixels
 * @param state Buttons held: with one, a program that asked for the mouse
 *  is told about the move (a drag)
 */
static void terminal_pointer(GtkWidget *terminal, const GdkEvent *source, gdouble x, gdouble y, guint state) {
    gdouble sx = 0, sy = 0, rx = 0, ry = 0;
    gdk_event_get_coords(source, &sx, &sy);
    gdk_event_get_root_coords(source, &rx, &ry);
    GdkEvent *move = gdk_event_new(GDK_MOTION_NOTIFY);
    move->motion.window = g_object_ref(gdk_event_get_window(source));
    move->motion.send_event = TRUE;
    move->motion.time = gdk_event_get_time(source);
    move->motion.x = x;
    move->motion.y = y;
    move->motion.x_root = rx - sx + x;
    move->motion.y_root = ry - sy + y;
    move->motion.state = state;
    gdk_event_set_device(move, gdk_event_get_device(source));
    forwarding = TRUE;
    gtk_widget_event(terminal, move);
    forwarding = FALSE;
    gdk_event_free(move);
}

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
    terminal_pointer(terminal, (const GdkEvent *) source, x, y, 0);
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
        forwarding = TRUE;
        gtk_widget_event(terminal, wheel);
        forwarding = FALSE;
        gdk_event_free(wheel);
    }
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
    if (kterm_terminal_has_history()) {
        // the terminal's own history: exactly as far as the finger went
        const gdouble old = gtk_adjustment_get_value(adjustment);
        const gdouble value = CLAMP(old + (dy < 0 ? lines : -lines), lower, top_max);
        if (value != old) {
            // already at the end, nothing would be drawn to flash
            flash_next_draw();
            gtk_adjustment_set_value(adjustment, value);
        }
        return;
    }
    flash_next_draw();
    // No history here: a full screen program (tmux, less) keeps its own.
    // Finger up shows what is below, which is the wheel turning down.
    terminal_wheel(terminal, source, dy < 0 ? GDK_SCROLL_DOWN : GDK_SCROLL_UP, x, y, (guint) lines);
}

/**
 * A finger moved left or right in reading mode: next or previous tmux
 * window. Nothing is typed into a shell, a pager or an editor: the prefix
 * and the letter would mean something else to them.
 * @param dx Distance moved, pixels; negative is left
 */
static void terminal_swipe_switch(gdouble dx) {
    if (!tmux_enabled() || !tmux_may_have_terminal()) {
        return;
    }
    // towards the left brings the next one in, like turning a page
    flash_next_draw();
    tmux_send_command(dx < 0 ? "n" : "p");
}

/*
 * How the Kindle's touch driver reports a touch, as X buttons (recorded on
 * the device with xinput): a finger going down and up is button 1. Once a
 * swipe is recognised it clicks another button, button 1 still down: 3
 * going up, 4 going down (4 to 7 reach GTK as wheel turns, see
 * scroll_event()). A finger held still for half a second clicks button 9.
 * A second finger is button 2.
 *
 * The press of button 1 is kept from the terminal until the release tells
 * what the touch was: a tap is then given to the terminal whole, a swipe
 * scrolls, a long press selects text and two fingers are the right button.
 * Given the press at once, the program in the terminal (tmux) would take
 * every swipe and every right click for a click of the left button too.
 */

/** The touch under way, from the press of button 1 to its release */
static struct {
    gdouble x_root, y_root; /** Where the finger went down, on the screen */
    gdouble x, y;           /** ...and in the terminal */
    guint state;            /** Modifiers at the press */
    gboolean swiping;       /** The driver marked a swipe */
    gboolean two_fingers;   /** A second finger touched */
    gboolean selecting;     /** Held still, then moved: selecting text */
} touch;
/** A two finger tap pressed the right button: the next tap releases it
 *  where it lands, which picks the item of a menu the press opened */
static gboolean right_held = FALSE;
static gdouble right_x = 0, right_y = 0;

void touch_reset(void) {
    memset(&touch, 0, sizeof(touch));
    right_held = FALSE;
}

/**
 * Give the terminal a button event made here
 * @param terminal Terminal widget
 * @param source Touch event it stands for (window, device, time)
 * @param type GDK_BUTTON_PRESS or GDK_BUTTON_RELEASE
 * @param button Button number
 * @param x,y Position in the terminal, pixels
 * @param state Modifiers and buttons held
 */
static void terminal_button(GtkWidget *terminal, const GdkEvent *source, GdkEventType type, guint button,
                            gdouble x, gdouble y, guint state) {
    gdouble sx = 0, sy = 0, rx = 0, ry = 0;
    gdk_event_get_coords(source, &sx, &sy);
    gdk_event_get_root_coords(source, &rx, &ry);
    GdkEvent *event = gdk_event_new(type);
    event->button.window = g_object_ref(gdk_event_get_window(source));
    event->button.send_event = TRUE;
    event->button.time = gdk_event_get_time(source);
    event->button.x = x;
    event->button.y = y;
    event->button.x_root = rx - sx + x;
    event->button.y_root = ry - sy + y;
    event->button.state = state;
    event->button.button = button;
    gdk_event_set_device(event, gdk_event_get_device(source));
    forwarding = TRUE;
    gtk_widget_event(terminal, event);
    forwarding = FALSE;
    gdk_event_free(event);
}

/** The character the selection started on, and the side of it the
 *  selection is anchored to: its start while dragging forward, its end
 *  while dragging back, so that it is always in the selection */
static glong select_col = 0, select_row = 0;
static gboolean select_forward = TRUE;

/**
 * @param terminal Terminal widget
 * @param x,y Position in the terminal, pixels
 * @param col,row Set to the character cell there
 */
static void cell_at(GtkWidget *terminal, gdouble x, gdouble y, glong *col, glong *row) {
    GtkBorder padding;
    gtk_style_context_get_padding(gtk_widget_get_style_context(terminal), gtk_widget_get_state_flags(terminal),
                                  &padding);
    const glong width = MAX(1, vte_terminal_get_char_width(VTE_TERMINAL(terminal)));
    const glong height = MAX(1, vte_terminal_get_char_height(VTE_TERMINAL(terminal)));
    *col = MAX(0, (glong) (x - padding.left) / width);
    *row = MAX(0, (glong) (y - padding.top) / height);
}

/**
 * Point VTE at a character, on one side of its middle. VTE takes a
 * character into a selection only if the pointer is past its middle: a
 * finger on the left half of the first character, or the right half of
 * the last, would leave it out.
 * @param terminal Terminal widget
 * @param col,row Character cell
 * @param after TRUE for its right half, FALSE for its left half
 * @param x,y Set to the position, in the terminal
 */
static void cell_point(GtkWidget *terminal, glong col, glong row, gboolean after, gdouble *x, gdouble *y) {
    GtkBorder padding;
    gtk_style_context_get_padding(gtk_widget_get_style_context(terminal), gtk_widget_get_state_flags(terminal),
                                  &padding);
    const glong width = MAX(1, vte_terminal_get_char_width(VTE_TERMINAL(terminal)));
    const glong height = MAX(1, vte_terminal_get_char_height(VTE_TERMINAL(terminal)));
    *x = padding.left + (col + (after ? 0.75 : 0.25)) * width;
    *y = padding.top + (row + 0.5) * height;
}

/**
 * Start VTE's selection at the first character, on the side the finger
 * goes away from. Shift makes VTE select even when the program in the
 * terminal asked for the mouse (tmux); with no selection there it starts a
 * new one rather than extending it.
 * @param terminal Terminal widget
 * @param source Touch event it stands for
 */
static void selection_anchor(GtkWidget *terminal, const GdkEvent *source) {
    gdouble x = 0, y = 0;
    vte_terminal_unselect_all(VTE_TERMINAL(terminal));
    cell_point(terminal, select_col, select_row, !select_forward, &x, &y);
    terminal_button(terminal, source, GDK_BUTTON_PRESS, 1, x, y, touch.state | GDK_SHIFT_MASK);
}

/**
 * The finger is at a place of the terminal while selecting: the selection
 * reaches the character there, whichever way it went
 * @param terminal Terminal widget
 * @param source Motion or release event
 * @param x,y Position of the finger, in the terminal
 */
static void selection_follow(GtkWidget *terminal, const GdkEvent *source, gdouble x, gdouble y) {
    glong col = 0, row = 0;
    cell_at(terminal, x, y, &col, &row);
    const gboolean forward = row > select_row || (row == select_row && col >= select_col);
    if (forward != select_forward) {
        // turned back past the first character: anchored on its other side
        gdouble ax = 0, ay = 0;
        cell_point(terminal, select_col, select_row, !select_forward, &ax, &ay);
        terminal_button(terminal, source, GDK_BUTTON_RELEASE, 1, ax, ay,
                        touch.state | GDK_SHIFT_MASK | GDK_BUTTON1_MASK);
        select_forward = forward;
        selection_anchor(terminal, source);
    }
    gdouble px = 0, py = 0;
    cell_point(terminal, col, row, forward, &px, &py);
    terminal_pointer(terminal, source, px, py, touch.state | GDK_SHIFT_MASK | GDK_BUTTON1_MASK);
}

/**
 * A long press: start selecting text at the character under the finger,
 * which is drawn selected at once, to show that the long press was taken
 * @param terminal Terminal widget
 * @param source Event of the long press
 */
static void selection_start(GtkWidget *terminal, const GdkEventButton *source) {
    cell_at(terminal, touch.x, touch.y, &select_col, &select_row);
    select_forward = TRUE;
    selection_anchor(terminal, (const GdkEvent *) source);
    selection_follow(terminal, (const GdkEvent *) source, touch.x, touch.y);
    touch.selecting = TRUE;
}

/**
 * The finger is lifted from a selection: it is copied as it is lifted, and
 * the shortcut bar offers to paste it. The selection stays drawn, to show
 * what was copied, until the next tap.
 * @param terminal Terminal widget
 * @param event Release event
 */
static void selection_end(GtkWidget *terminal, const GdkEventButton *event) {
    touch.selecting = FALSE;
    selection_follow(terminal, (const GdkEvent *) event, event->x, event->y);
    glong col = 0, row = 0;
    gdouble x = 0, y = 0;
    cell_at(terminal, event->x, event->y, &col, &row);
    cell_point(terminal, col, row, select_forward, &x, &y);
    terminal_button(terminal, (const GdkEvent *) event, GDK_BUTTON_RELEASE, 1, x, y,
                    touch.state | GDK_SHIFT_MASK | GDK_BUTTON1_MASK);
    if (!vte_terminal_get_has_selection(VTE_TERMINAL(terminal))) {
        return;
    }
    gchar *text = vte_terminal_get_text_selected(VTE_TERMINAL(terminal), VTE_FORMAT_TEXT);
    if (text && *text) {
        shortbar_offer_paste(text);
    }
    g_free(text);
}

/** Rows of the screen before shown again after turning a page, to read on
 *  from */
#define PAGE_OVERLAP_ROWS 2

/**
 * A tap on the top or bottom third of the terminal (the keyboard is not
 * part of it), previewing or not: a page back or forward, a screen less
 * two rows. The middle third and the last row are left to the program:
 * tmux's status line, whose window names are tapped.
 * @param terminal Terminal widget
 * @param source Release event of the tap
 * @param x,y Where the tap was, in the terminal
 * @return TRUE if a page was turned, FALSE if the tap is the program's
 */
static gboolean terminal_tap_page(GtkWidget *terminal, const GdkEventButton *source, gdouble x, gdouble y) {
    const glong rows = vte_terminal_get_row_count(VTE_TERMINAL(terminal));
    const glong cell = vte_terminal_get_char_height(VTE_TERMINAL(terminal));
    const gint height = gtk_widget_get_allocated_height(terminal);
    glong col = 0, row = 0;
    cell_at(terminal, x, y, &col, &row);
    if (cell <= 0 || row >= rows - 1) {
        return FALSE;
    }
    gint forward = 0;
    if (y < height / 3.0) {
        forward = -1;
    } else if (y >= height * 2 / 3.0) {
        forward = 1;
    } else {
        return FALSE;
    }
    const glong lines = MAX(1, rows - PAGE_OVERLAP_ROWS);
    D printf("page %s, %ld rows\n", forward > 0 ? "forward" : "back", lines);
    // as a finger moving that far: up shows what is below
    terminal_swipe_scroll(terminal, source, x, y, (gdouble) (-forward * lines * cell));
    return TRUE;
}

/**
 * Touches on the terminal
 * @param terminal Terminal widget
 * @param any Button or motion event
 * @return TRUE to keep the event from VTE
 */
gboolean touch_event(GtkWidget *terminal, GdkEvent *any) {
    if (any->type == GDK_MOTION_NOTIFY) {
        if (touch.selecting) {
            selection_follow(terminal, any, any->motion.x, any->motion.y);
        }
        // otherwise a moving finger scrolls, see terminal_swipe_scroll()
        return TRUE;
    }
    GdkEventButton *event = &any->button;
    if (event->type != GDK_BUTTON_PRESS && event->type != GDK_BUTTON_RELEASE) {
        // double clicks made by GTK out of two real presses: taps are given
        // to the terminal whole, at the release
        return TRUE;
    }
    if (event->button == 2) {
        if (event->type == GDK_BUTTON_PRESS) {
            touch.two_fingers = TRUE;
        }
        return TRUE;
    }
    if (event->button == 9) {
        if (event->type == GDK_BUTTON_PRESS && !touch.swiping && !touch.two_fingers && !touch.selecting) {
            selection_start(terminal, event);
        }
        return TRUE;
    }
    if (event->button != 1) {
        // the mark of a swipe; given on, tmux would open its menu
        if (event->type == GDK_BUTTON_PRESS) {
            touch.swiping = TRUE;
        }
        return TRUE;
    }
    if (event->type == GDK_BUTTON_PRESS) {
        touch.x_root = event->x_root;
        touch.y_root = event->y_root;
        touch.x = event->x;
        touch.y = event->y;
        touch.state = event->state;
        touch.swiping = touch.two_fingers = touch.selecting = FALSE;
        return TRUE;
    }
    if (touch.selecting) {
        selection_end(terminal, event);
        return TRUE;
    }
    if (touch.two_fingers) {
        if (right_held) {
            terminal_button(terminal, (const GdkEvent *) event, GDK_BUTTON_RELEASE, 3, right_x, right_y, touch.state | GDK_BUTTON3_MASK);
        }
        terminal_button(terminal, (const GdkEvent *) event, GDK_BUTTON_PRESS, 3, touch.x, touch.y, touch.state);
        right_held = TRUE;
        right_x = touch.x;
        right_y = touch.y;
        return TRUE;
    }
    const gdouble dx = event->x_root - touch.x_root;
    const gdouble dy = event->y_root - touch.y_root;
    const gboolean vertical = ABS(dy) >= TOUCH_SWIPE_MIN_PX && ABS(dy) >= ABS(dx) * 1.5;
    const gboolean horizontal = ABS(dx) >= TOUCH_SWIPE_MIN_PX && ABS(dx) >= ABS(dy) * 1.5;
    const gboolean tap = !touch.swiping && !vertical && !horizontal;
    if (right_held) {
        // A tap lets the right button go where it lands, as if dragged
        // there: on an item of a menu the press opened (tmux), that item is
        // chosen. Anything else lets it go where it was pressed, which
        // closes the menu.
        right_held = FALSE;
        const gdouble x = tap ? event->x : right_x;
        const gdouble y = tap ? event->y : right_y;
        terminal_pointer(terminal, (const GdkEvent *) event, x, y, touch.state | GDK_BUTTON3_MASK);
        terminal_button(terminal, (const GdkEvent *) event, GDK_BUTTON_RELEASE, 3, x, y, touch.state | GDK_BUTTON3_MASK);
        if (tap) {
            return TRUE;
        }
    }
    if (vertical && dy > 0 && touch.y_root <= TOUCH_EDGE_TOP_PX) {
        // Down from the top edge is the device's own gesture for its quick
        // settings (light, dark mode, airplane mode). A full screen
        // application has to pass it on.
        show_system_menu();
        return TRUE;
    }
    if (vertical) {
        terminal_swipe_scroll(terminal, event, touch.x, touch.y, dy);
        return TRUE;
    }
    if (horizontal && kterm_previewing()) {
        terminal_swipe_switch(dx);
    }
    if (!tap) {
        return TRUE;
    }
    // a tap puts away the selection drawn
    vte_terminal_unselect_all(VTE_TERMINAL(terminal));
    if (ABS(dx) < TAP_SLOP_PX && ABS(dy) < TAP_SLOP_PX) {
        // the path of an image first: it may be anywhere, page turning
        // thirds too
        if (imageview_tap(terminal, any) || terminal_tap_page(terminal, event, touch.x, touch.y)) {
            return TRUE;
        }
        kterm_keyboard_back_on_tap();
    }
    // the press kept back, then this release
    terminal_button(terminal, (const GdkEvent *) event, GDK_BUTTON_PRESS, 1, touch.x, touch.y, touch.state);
    return FALSE;
}

/**
 * @return TRUE while an event made here is given to VTE: let it through
 */
gboolean touch_forwarding(void) {
    return forwarding;
}

/**
 * Wheel turns on the terminal: those of the touch driver are marks of a
 * swipe (see touch_event()); only the ones made here reach VTE
 */
gboolean touch_scroll_event(GtkWidget *terminal, GdkEventScroll *event, gpointer data) {
    UNUSED(terminal);
    UNUSED(data);
    return !forwarding && !event->send_event;
}
