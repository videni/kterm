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
#include "refresh.h"
#include "shortbar.h"
#include "imageview.h"
#include "tmux.h"
#include "touch.h"

extern KTconf *conf;

/** How far a finger may move, in pixels, and still count as a tap */
#define TAP_SLOP_PX 30
/** A vertical swipe across at least TOUCH_SWIPE_MIN_PX within this time turns a page. */
#define FLICK_MAX_MS 240

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
    guint32 pressed_at;
    guint state;            /** Modifiers at the press */
    gboolean swiping;       /** The driver marked a swipe */
    gboolean two_fingers;   /** A second finger touched */
    gboolean selecting;     /** Held still, then moved: selecting text */
    gboolean dragging_history;
    gdouble drag_start;     /** History adjustment before a drag */
} touch;
/** A two finger tap pressed the right button: the next tap releases it
 *  where it lands, which picks the item of a menu the press opened */
static gboolean right_held = FALSE;
static gdouble right_x = 0, right_y = 0;

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

/** Let a screen update settle before measuring the moved row. */
#define PAGE_FRAME_MS 24
#define PAGE_REPLY_MS 2000
#define PAGE_BATCH_MAX 16
#define PAGE_ANCHOR_CHARS 12
#define PAGE_HIGHLIGHT_MS 1600
#define SCROLL_MARKER_WIDTH 16

/** Half-open range of columns containing the touched content. */
typedef struct {
    glong begin;
    glong end;
} PageColumns;

typedef struct {
    gchar **lines;
    glong first;
    glong columns;
    glong rows;
    PageColumns region;
} PageFrame;

typedef struct {
    GtkWidget *terminal;
    GdkEvent *event;
    pid_t owner;
    PageFrame before;
    PageFrame pending;
    GPtrArray *candidates;
    gchar *anchor;
    glong row;
    glong target;
    glong landing;
    glong swipe_rows;
    gdouble input_x;
    gdouble input_y;
    glong stride;
    guint sent;
    guint steady;
    gint direction;
    guint steps_left;
    gint64 deadline;
    gulong contents_handler;
    gboolean contents_dirty;
    gboolean first;
    gboolean moved;
} PageScroll;

static guint page_timer = 0;
static struct {
    GdkWindow *window;
    GtkWidget *terminal;
    gulong draw_handler;
    glong landing;
    guint timeout;
} page_marker;

static GdkRectangle page_marker_area(GtkWidget *terminal, glong row) {
    GtkBorder padding;
    gtk_style_context_get_padding(gtk_widget_get_style_context(terminal), gtk_widget_get_state_flags(terminal),
                                  &padding);
    const gint cell_height = vte_terminal_get_char_height(VTE_TERMINAL(terminal));
    return (GdkRectangle) {
        .y = padding.top + row * cell_height + 1,
        .width = SCROLL_MARKER_WIDTH,
        .height = MAX(1, cell_height - 2),
    };
}

static void paint_page_marker(cairo_t *cr, const GdkRectangle *area) {
    const gdouble tone = conf->color_reversed == VTE_SCHEME_DARK ? 1.0 : 0.0;
    cairo_save(cr);
    cairo_set_source_rgb(cr, tone, tone, tone);
    gdk_cairo_rectangle(cr, area);
    cairo_fill(cr);
    cairo_restore(cr);
}

/** Mark the source on the old frame while the terminal's updates are held. */
static void show_page_source_marker(GtkWidget *terminal, glong row) {
    GtkWidget *toplevel = gtk_widget_get_toplevel(terminal);
    gint x, y;
    if (!gtk_widget_get_mapped(terminal)
        || !gtk_widget_translate_coordinates(terminal, toplevel, 0, 0, &x, &y)) {
        return;
    }
    GdkRectangle area = page_marker_area(terminal, row);
    GdkWindowAttr attrs = {
        .window_type = GDK_WINDOW_CHILD,
        .wclass = GDK_INPUT_OUTPUT,
        .x = x,
        .y = y + area.y,
        .width = area.width,
        .height = area.height,
    };
    page_marker.window = gdk_window_new(gtk_widget_get_window(toplevel), &attrs, GDK_WA_X | GDK_WA_Y);
    if (!gdk_window_ensure_native(page_marker.window)) {
        g_clear_pointer(&page_marker.window, gdk_window_destroy);
        return;
    }
    gdk_window_set_pass_through(page_marker.window, TRUE);
    gdk_window_show(page_marker.window);
    area.y = 0;
    cairo_region_t *region = cairo_region_create_rectangle(&area);
    GdkDrawingContext *drawing = gdk_window_begin_draw_frame(page_marker.window, region);
    paint_page_marker(gdk_drawing_context_get_cairo_context(drawing), &area);
    gdk_window_end_draw_frame(page_marker.window, drawing);
    cairo_region_destroy(region);
}

static void clear_page_marker(void) {
    g_clear_handle_id(&page_marker.timeout, g_source_remove);
    if (page_marker.terminal) {
        GtkWidget *terminal = page_marker.terminal;
        GtkWidget *toplevel = gtk_widget_get_toplevel(terminal);
        const GdkRectangle area = page_marker_area(terminal, page_marker.landing);
        g_clear_signal_handler(&page_marker.draw_handler, terminal);
        gint x, y;
        if (gtk_widget_translate_coordinates(terminal, toplevel, area.x, area.y, &x, &y)) {
            gtk_widget_queue_draw_area(toplevel, x, y, area.width, area.height);
        }
        g_clear_object(&page_marker.terminal);
    }
    g_clear_pointer(&page_marker.window, gdk_window_destroy);
}

static gboolean expire_page_marker(gpointer data) {
    UNUSED(data);
    page_marker.timeout = 0;
    clear_page_marker();
    return G_SOURCE_REMOVE;
}

static gboolean draw_landed_page_marker(GtkWidget *terminal, cairo_t *cr, gpointer data) {
    UNUSED(data);
    const GdkRectangle area = page_marker_area(terminal, page_marker.landing);
    paint_page_marker(cr, &area);
    if (!page_marker.timeout) {
        page_marker.timeout = g_timeout_add(PAGE_HIGHLIGHT_MS, expire_page_marker, NULL);
    }
    return FALSE;
}

static void land_page_marker(GtkWidget *terminal, glong row) {
    clear_page_marker();
    if (row < 0 || row >= vte_terminal_get_row_count(VTE_TERMINAL(terminal))) {
        return;
    }
    page_marker.terminal = g_object_ref(terminal);
    page_marker.landing = row;
    page_marker.draw_handler = g_signal_connect_after(terminal, "draw", G_CALLBACK(draw_landed_page_marker), NULL);
    gtk_widget_queue_draw(terminal);
}

void touch_cancel_page(void) {
    clear_page_marker();
    if (page_timer) {
        D printf("page tracking cancelled by input\n");
        guint timer = page_timer;
        page_timer = 0;
        g_source_remove(timer);
    }
}

void touch_reset(void) {
    touch_cancel_page();
    memset(&touch, 0, sizeof(touch));
    right_held = FALSE;
}

static gchar *page_line(VteTerminal *terminal, glong row, glong begin, glong end) {
    gchar *text = vte_terminal_get_text_range_format(terminal, VTE_FORMAT_TEXT, row, begin, row, end, NULL);
    return text ? g_strchomp(text) : g_strdup("");
}

static gboolean page_range_matches(VteTerminal *terminal, const gchar *visible, glong first, glong rows) {
    gchar *range = vte_terminal_get_text_range_format(terminal, VTE_FORMAT_TEXT,
                                                      first, 0, first + rows, 0, NULL);
    const gboolean matches = range && !strcmp(range, visible);
    g_free(range);
    return matches;
}

/**
 * VTE's public scroll adjustment is relative to the start of its ring,
 * while text ranges use absolute rows. Locate the actual displayed text.
 * This path is only used at the live screen, where the cursor is inside it.
 */
static glong page_frame_origin(VteTerminal *terminal, glong rows, glong previous) {
    gchar *visible = vte_terminal_get_text_format(terminal, VTE_FORMAT_TEXT);
    if (!visible) {
        return -1;
    }
    glong first = -1;
    if (previous >= 0 && page_range_matches(terminal, visible, previous, rows)) {
        first = previous;
    } else {
        glong cursor;
        vte_terminal_get_cursor_position(terminal, NULL, &cursor);
        for (glong candidate = MAX(0, cursor - rows + 1); candidate <= cursor; candidate++) {
            if (page_range_matches(terminal, visible, candidate, rows)) {
                first = candidate;
                break;
            }
        }
    }
    g_free(visible);
    return first;
}

static PageFrame page_frame(GtkWidget *widget, glong previous, PageColumns region) {
    VteTerminal *terminal = VTE_TERMINAL(widget);
    PageFrame frame = {
        .columns = vte_terminal_get_column_count(terminal),
        .rows = vte_terminal_get_row_count(terminal),
    };
    frame.first = page_frame_origin(terminal, frame.rows, previous);
    if (frame.first < 0) {
        return frame;
    }
    frame.region.end = region.end ? MIN(region.end, frame.columns) : frame.columns;
    frame.region.begin = MIN(region.begin, frame.region.end - 1);
    frame.lines = g_new0(gchar *, frame.rows + 1);
    for (glong row = 0; row < frame.rows; row++) {
        frame.lines[row] = page_line(terminal, frame.first + row, frame.region.begin, frame.region.end);
    }
    return frame;
}

/** Follow a fragment only while it identifies one place in the frame. */
static gboolean page_find(const PageFrame *frame, const gchar *anchor, glong *row, gsize *offset) {
    *row = -1;
    for (glong line = 0; line < frame->rows; line++) {
        const gchar *match = strstr(frame->lines[line], anchor);
        if (!match) {
            continue;
        }
        if (*row >= 0 || strstr(match + 1, anchor)) {
            return FALSE;
        }
        *row = line;
        *offset = match - frame->lines[line];
    }
    return *row >= 0;
}

/**
 * Pick visible text near the tap. A short unique fragment can still be
 * found when a program rewraps the row or redraws text beside it.
 */
static gchar *page_anchor(GtkWidget *widget, const PageFrame *frame, glong row, glong column) {
    gchar *prefix = page_line(VTE_TERMINAL(widget), frame->first + row, frame->region.begin, column + 1);
    const gsize tap = strlen(prefix);
    g_free(prefix);
    const gchar *line = frame->lines[row];
    gchar *best = NULL;
    gsize best_distance = G_MAXSIZE;
    const gchar *start = line;
    while (*start) {
        start += strspn(start, " \t");
        if (!*start) {
            break;
        }
        const gchar *end = start + strcspn(start, " \t");
        const gsize begin = start - line;
        const gsize finish = end - line;
        const gsize distance = MAX(tap, begin) - MIN(tap, finish);
        if (distance < best_distance) {
            glong chars = MIN(PAGE_ANCHOR_CHARS, g_utf8_strlen(start, end - start));
            const gchar *stop = g_utf8_offset_to_pointer(start, chars);
            gchar *candidate = g_strndup(start, stop - start);
            glong found;
            gsize offset;
            if (page_find(frame, candidate, &found, &offset)) {
                g_free(best);
                best = candidate;
                best_distance = distance;
            } else {
                g_free(candidate);
            }
        }
        start = end;
    }
    if (!best) {
        gchar *whole = g_strstrip(g_strdup(line));
        glong found;
        gsize offset;
        if (*whole && page_find(frame, whole, &found, &offset)) {
            return whole;
        }
        g_free(whole);
    }
    return best;
}

/** Find the cell containing the matched byte offset, using VTE's own column widths. */
static glong page_column(GtkWidget *widget, const PageFrame *frame, glong row, gsize offset) {
    glong lower = frame->region.begin;
    glong upper = frame->region.end;
    while (lower < upper) {
        glong middle = lower + (upper - lower) / 2;
        gchar *prefix = page_line(VTE_TERMINAL(widget), frame->first + row, frame->region.begin, middle + 1);
        gboolean includes_match = strlen(prefix) > offset;
        g_free(prefix);
        if (includes_match) {
            upper = middle;
        } else {
            lower = middle + 1;
        }
    }
    return MIN(lower, frame->region.end - 1);
}

static gboolean vertical_separator(gunichar ch) {
    return ch == 0x2502 || ch == 0x2503 || ch == 0x2551;
}

/** Locate the bounds of the content the finger is over, using visible dividers. */
static PageColumns page_region(GtkWidget *widget, const PageFrame *frame, glong column) {
    PageColumns region = {0, frame->columns};
    if (frame->rows < 6) {
        return region;
    }
    const glong middle = frame->rows / 2;
    const gchar *line = frame->lines[middle];
    for (const gchar *ch = line; *ch; ch = g_utf8_next_char(ch)) {
        if (!vertical_separator(g_utf8_get_char(ch))) {
            continue;
        }
        const glong divider = page_column(widget, frame, middle, ch - line);
        gchar *above = page_line(VTE_TERMINAL(widget), frame->first + frame->rows / 4, divider, divider + 1);
        gchar *below = page_line(VTE_TERMINAL(widget), frame->first + 3 * frame->rows / 4, divider, divider + 1);
        const gboolean confirmed = vertical_separator(g_utf8_get_char(above))
                                   && vertical_separator(g_utf8_get_char(below));
        g_free(above);
        g_free(below);
        if (!confirmed) {
            continue;
        }
        if (divider < column) {
            region.begin = divider + 1;
        } else {
            region.end = divider;
            break;
        }
    }
    return region;
}

/** Keep wheel input at the gesture's starting point. */
static void page_wheel(PageScroll *page, guint count) {
    terminal_wheel(page->terminal, &page->event->button,
                   page->direction > 0 ? GDK_SCROLL_DOWN : GDK_SCROLL_UP, page->input_x, page->input_y, count);
}

static void page_send(PageScroll *page, guint count) {
    page->sent = count;
    page->steps_left -= count;
    page_wheel(page, count);
    page->deadline = g_get_monotonic_time() + PAGE_REPLY_MS * 1000;
}

/**
 * A row can change text while moving. Use its shared prefix when it
 * uniquely confirms the displacement observed in nearby rows.
 */
static gchar *page_shared_anchor(const PageFrame *before, const PageFrame *after,
                                  glong origin, glong destination) {
    const gchar *first = before->lines[origin];
    const gchar *end = first;
    const gchar *other = after->lines[destination];
    while (*end && *other && g_utf8_get_char(end) == g_utf8_get_char(other)) {
        end = g_utf8_next_char(end);
        other = g_utf8_next_char(other);
    }
    gchar *prefix = g_strstrip(g_strndup(first, end - first));
    glong row;
    gsize offset;
    if (g_utf8_strlen(prefix, -1) >= 4
        && page_find(before, prefix, &row, &offset) && row == origin
        && page_find(after, prefix, &row, &offset) && row == destination) {
        return prefix;
    }
    g_free(prefix);
    return NULL;
}

/** Track the touched row for a drag, or find a moving row from the edge. */
static gboolean page_choose_anchor(PageScroll *page, const PageFrame *after, glong *row, gsize *offset) {
    const glong edge = page->direction > 0 ? page->before.rows - 1 : 0;
    const gint step = -page->direction;
    const glong limit = page->swipe_rows ? (step > 0 ? page->before.rows : -1) : page->target;
    const glong drag_row = page->target + page->direction * page->swipe_rows;
    const gchar *drag_anchor = page->swipe_rows ? g_ptr_array_index(page->candidates, drag_row) : NULL;
    const gboolean follows_touch = drag_anchor && page_find(after, drag_anchor, row, offset)
                                   && (drag_row - *row) * page->direction > 0;
    glong origin_row = follows_touch ? drag_row : -1;
    for (glong origin = edge; !follows_touch && origin != limit; origin += step) {
        const gchar *candidate = g_ptr_array_index(page->candidates, origin);
        if (!candidate || !page_find(after, candidate, row, offset)) {
            continue;
        }
        if ((origin - *row) * page->direction > 0) {
            origin_row = origin;
            break;
        }
        // A first input can reflow the application before it scrolls.
        if (origin_row < 0 && strcmp(after->lines[*row], page->before.lines[origin])) {
            origin_row = origin;
        }
    }
    if (origin_row < 0) {
        return FALSE;
    }
    const gchar *candidate = g_ptr_array_index(page->candidates, origin_row);
    if (!page_find(after, candidate, row, offset)) {
        return FALSE;
    }
    g_autofree gchar *shared = NULL;
    const glong movement = *row - origin_row;
    if (!follows_touch && movement * step > 0 && origin_row != edge) {
        for (glong origin = edge; origin != origin_row; origin += step) {
            const glong destination = origin + movement;
            if (destination < 0 || destination >= after->rows) {
                continue;
            }
            shared = page_shared_anchor(&page->before, after, origin, destination);
            if (shared) {
                candidate = shared;
                origin_row = origin;
                page_find(after, candidate, row, offset);
                break;
            }
        }
    }
    page->anchor = g_strdup(candidate);
    page->row = origin_row;
    if (page->swipe_rows) {
        page->target = CLAMP(origin_row - page->direction * page->swipe_rows, 0, after->rows - 1);
    }
    g_clear_pointer(&page->candidates, g_ptr_array_unref);
    return TRUE;
}

static gboolean page_same_frame(const PageFrame *a, const PageFrame *b) {
    if (!a->lines || a->rows != b->rows || a->columns != b->columns
        || a->region.begin != b->region.begin || a->region.end != b->region.end) {
        return FALSE;
    }
    for (glong row = 0; row < a->rows; row++) {
        if (strcmp(a->lines[row], b->lines[row])) {
            return FALSE;
        }
    }
    return TRUE;
}

static void page_contents_changed(VteTerminal *terminal, PageScroll *page) {
    UNUSED(terminal);
    page->contents_dirty = TRUE;
}

/** Only read the grid on an update. Keep a changed frame for one interval
 * so a partial redraw can finish before more input is sent. */
static gboolean page_settled_frame(PageScroll *page, PageFrame *after) {
    VteTerminal *terminal = VTE_TERMINAL(page->terminal);
    const PageFrame *latest = page->pending.lines ? &page->pending : &page->before;
    if (!page->contents_dirty && latest->rows == vte_terminal_get_row_count(terminal)
        && latest->columns == vte_terminal_get_column_count(terminal)) {
        if (!page->pending.lines) {
            return FALSE;
        }
        *after = page->pending;
        page->pending.lines = NULL;
        return TRUE;
    }

    page->contents_dirty = FALSE;
    *after = page_frame(page->terminal, latest->first, page->before.region);
    if (!after->lines || page_same_frame(&page->before, after)) {
        g_clear_pointer(&page->pending.lines, g_strfreev);
        g_strfreev(after->lines);
        return FALSE;
    }
    if (!page_same_frame(&page->pending, after)) {
        g_strfreev(page->pending.lines);
        page->pending = *after;
        return FALSE;
    }
    g_clear_pointer(&page->pending.lines, g_strfreev);
    return TRUE;
}

static gboolean page_tick(gpointer data) {
    PageScroll *page = data;
    if (!gtk_widget_get_mapped(page->terminal) || page->owner != kterm_terminal_owner()) {
        page_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (g_get_monotonic_time() >= page->deadline) {
        if (page->moved) {
            PageFrame after = page_frame(page->terminal, page->before.first, page->before.region);
            glong row;
            gsize offset;
            if (after.lines && page_find(&after, page->anchor, &row, &offset)) {
                page->landing = row;
            }
            g_strfreev(after.lines);
        }
        page_timer = 0;
        return G_SOURCE_REMOVE;
    }
    PageFrame after;
    if (!page_settled_frame(page, &after)) {
        return G_SOURCE_CONTINUE;
    }
    glong row;
    gsize offset;
    const gboolean found = page->anchor ? page_find(&after, page->anchor, &row, &offset)
                                        : page_choose_anchor(page, &after, &row, &offset);
    if (!found) {
        g_strfreev(after.lines);
        if (page->first && page->swipe_rows) {
            const guint steps = MIN(page->swipe_rows, SWIPE_WHEEL_MAX);
            if (steps > page->sent) {
                flash_next_draw();
                page_wheel(page, steps - page->sent);
            }
            page_timer = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }
    const glong moved = (page->row - row) * page->direction;
    const glong expected = page->stride * (glong) page->sent;
    const gboolean reflow = page->first && strcmp(after.lines[row], page->before.lines[page->row]);
    if ((moved <= 0 && !reflow) || (page->sent > 1 && moved < expected)) {
        g_strfreev(after.lines);
        return G_SOURCE_CONTINUE;
    }
    const glong step = MAX(1, moved / page->sent);
    const gboolean consistent = moved == expected;
    page->steady = moved > 0 ? (consistent ? page->steady + 1 : 1) : 0;
    page->stride = step;
    if (moved > 0 && !page->moved) {
        show_page_source_marker(page->terminal, page->row);
    }
    page->moved |= moved > 0;
    const glong edge = page->direction > 0 ? 0 : after.rows - 1;
    const glong target = CLAMP(page->target, 0, after.rows - 1);
    const glong distance = (row - target) * page->direction;
    // With a multi-row step, leave the last step for nearest-row feedback.
    const gboolean next = page->steps_left && step < 2 * distance
                          && step <= (row - edge) * page->direction;
    guint count = next ? 1 : 0;
    if (next && page->steady >= 2) {
        const glong remaining = distance / step - (step > 1 ? 1 : 0);
        const guint batch = (guint) CLAMP(remaining, 1, PAGE_BATCH_MAX);
        count = MIN(page->steps_left, batch);
    }
    D printf("page anchor %u at row %ld; target %ld, moved %ld, sending %u\n",
             g_str_hash(page->anchor), row, target, moved, count);
    if (!count) {
        page->landing = page->moved ? row : -1;
        g_strfreev(after.lines);
        page_timer = 0;
        return G_SOURCE_REMOVE;
    }

    g_strfreev(page->before.lines);
    page->before = after;
    page->row = row;
    page->first = FALSE;
    page_send(page, count);
    return G_SOURCE_CONTINUE;
}

static void page_free(gpointer data) {
    PageScroll *page = data;
    g_clear_signal_handler(&page->contents_handler, page->terminal);
    if (page->landing >= 0) {
        flash_next_draw();
        land_page_marker(page->terminal, page->landing);
    } else {
        clear_page_marker();
    }
    refresh_show_page();
    g_strfreev(page->before.lines);
    g_strfreev(page->pending.lines);
    g_clear_pointer(&page->candidates, g_ptr_array_unref);
    g_free(page->anchor);
    gdk_event_free(page->event);
    g_object_unref(page->terminal);
    g_free(page);
}

static gboolean page_application(GtkWidget *terminal, const GdkEventButton *source,
                                 gdouble x, gdouble y, glong row, glong column, gint direction, glong swipe_rows) {
    touch_cancel_page();
    PageFrame frame = page_frame(terminal, 0, (PageColumns) {0});
    if (!frame.lines) {
        if (swipe_rows) {
            return FALSE;
        }
        flash_next_draw();
        terminal_wheel(terminal, source, direction > 0 ? GDK_SCROLL_DOWN : GDK_SCROLL_UP, x, y, 1);
        return TRUE;
    }
    const PageColumns region = page_region(terminal, &frame, column);
    const gboolean split = region.begin > 0 || region.end < frame.columns;
    if (column >= region.end) {
        g_strfreev(frame.lines);
        return FALSE;
    }
    if (split) {
        g_strfreev(frame.lines);
        frame = page_frame(terminal, frame.first, region);
        if (!frame.lines) {
            return FALSE;
        }
    }
    const glong edge = direction > 0 ? frame.rows - 1 : 0;
    const glong limit = swipe_rows ? (direction > 0 ? -1 : frame.rows) : row;
    GPtrArray *candidates = g_ptr_array_new_full(frame.rows, g_free);
    g_ptr_array_set_size(candidates, frame.rows);
    gboolean has_anchor = FALSE;
    for (glong origin = edge; origin != limit; origin -= direction) {
        gchar *anchor = page_anchor(terminal, &frame, origin, column);
        g_ptr_array_index(candidates, origin) = anchor;
        has_anchor |= anchor != NULL;
    }
    if (swipe_rows && !has_anchor) {
        g_strfreev(frame.lines);
        g_ptr_array_unref(candidates);
        return FALSE;
    }
    D printf("page tapped %ld,%ld in %ld+%ld; looking from row %ld\n",
             column, row, frame.first, frame.rows, edge);
    PageScroll *page = g_new0(PageScroll, 1);
    page->terminal = g_object_ref(terminal);
    page->event = gdk_event_copy((const GdkEvent *) source);
    page->owner = kterm_terminal_owner();
    page->before = frame;
    page->candidates = candidates;
    page->target = row;
    page->swipe_rows = swipe_rows;
    page->input_x = x;
    page->input_y = y;
    page->landing = -1;
    page->direction = direction;
    page->steps_left = frame.rows;
    page->first = TRUE;
    page->contents_handler = g_signal_connect(terminal, "contents-changed", G_CALLBACK(page_contents_changed), page);
    refresh_hold_page();
    page_send(page, 1);
    page_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, PAGE_FRAME_MS, page_tick, page, page_free);
    return TRUE;
}

typedef struct {
    GtkWidget *terminal;
    gdouble value;
    glong landing;
} HistoryPage;

static gboolean history_page_turn(gpointer data) {
    HistoryPage *page = data;
    page_timer = 0;
    if (gtk_widget_get_mapped(page->terminal)) {
        flash_next_draw();
        gtk_adjustment_set_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(page->terminal)), page->value);
        land_page_marker(page->terminal, page->landing);
    } else {
        clear_page_marker();
    }
    return G_SOURCE_REMOVE;
}

static void history_page_free(gpointer data) {
    HistoryPage *page = data;
    g_object_unref(page->terminal);
    g_free(page);
}

static gdouble history_scroll_value(GtkAdjustment *adjustment, gdouble before, glong rows) {
    const gdouble lower = gtk_adjustment_get_lower(adjustment);
    const gdouble upper = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
    return CLAMP(before - rows, lower, upper);
}

static void history_page_scroll(GtkWidget *terminal, glong origin, glong rows) {
    GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(terminal));
    const gdouble before = gtk_adjustment_get_value(adjustment);
    const gdouble value = history_scroll_value(adjustment, before, rows);
    const glong moved = (glong) (before - value);
    if (moved) {
        show_page_source_marker(terminal, origin);
        HistoryPage *page = g_new(HistoryPage, 1);
        *page = (HistoryPage) {g_object_ref(terminal), value, origin + moved};
        page_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, PAGE_FRAME_MS, history_page_turn, page, history_page_free);
    }
}

static void move_history_drag(GtkWidget *terminal, gdouble dy, gboolean released) {
    GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(terminal));
    glong col, origin, target;
    cell_at(terminal, touch.x, touch.y, &col, &origin);
    cell_at(terminal, touch.x, touch.y + dy, &col, &target);
    target = MIN(target, vte_terminal_get_row_count(VTE_TERMINAL(terminal)) - 1);
    const gdouble value = history_scroll_value(adjustment, touch.drag_start, target - origin);
    const glong moved = (glong) (touch.drag_start - value);
    if (released && moved) {
        flash_next_draw();
        land_page_marker(terminal, origin + moved);
    } else if (value != gtk_adjustment_get_value(adjustment)) {
        refresh_activity();
    }
    gtk_adjustment_set_value(adjustment, value);
}

static void follow_history_drag(GtkWidget *terminal, const GdkEventMotion *event) {
    if (!(event->state & GDK_BUTTON1_MASK) || !touch.pressed_at || touch.two_fingers || right_held) {
        return;
    }
    const gdouble dy = event->y_root - touch.y_root;
    if (!touch.dragging_history) {
        const gdouble dx = event->x_root - touch.x_root;
        if (event->time - touch.pressed_at <= FLICK_MAX_MS || ABS(dy) < TAP_SLOP_PX
            || ABS(dy) < ABS(dx) * 1.5 || !kterm_terminal_has_history()) {
            return;
        }
        touch_cancel_page();
        touch.drag_start = gtk_adjustment_get_value(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(terminal)));
        touch.dragging_history = TRUE;
    }
    move_history_drag(terminal, dy, FALSE);
}

/** Move the touched row to the release row and mark it. */
static void terminal_drag_scroll(GtkWidget *terminal, const GdkEventButton *source, gdouble x, gdouble y, gdouble dy) {
    VteTerminal *vte = VTE_TERMINAL(terminal);
    const glong rows = vte_terminal_get_row_count(vte);
    glong column, origin, target;
    cell_at(terminal, x, y + dy, &column, &target);
    cell_at(terminal, x, y, &column, &origin);
    if (origin >= rows || column >= vte_terminal_get_column_count(vte)) {
        return;
    }
    target = MIN(target, rows - 1);
    const glong delta = target - origin;
    if (!delta) {
        return;
    }
    if (kterm_terminal_has_history()) {
        history_page_scroll(terminal, origin, delta);
        return;
    }
    const gint direction = delta > 0 ? -1 : 1;
    if (page_application(terminal, source, x, y, target, column, direction, ABS(delta))) {
        return;
    }
    flash_next_draw();
    terminal_wheel(terminal, source, direction > 0 ? GDK_SCROLL_DOWN : GDK_SCROLL_UP, x, y, (guint) ABS(delta));
}

/** Move content from the opposite edge to a page's target row. */
static gboolean terminal_turn_page(GtkWidget *terminal, const GdkEventButton *source,
                                    gdouble x, gdouble y, glong column, glong row, gint direction) {
    const glong rows = vte_terminal_get_row_count(VTE_TERMINAL(terminal));
    const glong origin = direction > 0 ? rows - 1 : 0;
    const glong delta = row - origin;
    D printf("page moves content %ld rows\n", delta);
    if (kterm_terminal_has_history()) {
        history_page_scroll(terminal, origin, delta);
        return TRUE;
    }
    return page_application(terminal, source, x, y, row, column, direction, 0);
}

/** A flick turns a page with up to three rows of overlap at the destination edge. */
static void terminal_flick_page(GtkWidget *terminal, const GdkEventButton *source, gdouble x, gdouble y, gdouble dy) {
    VteTerminal *vte = VTE_TERMINAL(terminal);
    const glong rows = vte_terminal_get_row_count(vte);
    glong column, origin;
    cell_at(terminal, x, y, &column, &origin);
    if (rows >= 2 && origin < rows && column < vte_terminal_get_column_count(vte)) {
        const glong inset = MIN(2, rows / 6);
        const gint direction = dy > 0 ? -1 : 1;
        const glong target = direction > 0 ? inset : rows - 1 - inset;
        terminal_turn_page(terminal, source, x, y, column, target, direction);
    }
}

/**
 * A tap on the top or bottom third moves content from the opposite edge
 * to the tap. The middle third remains available for the program's clicks.
 * @param terminal Terminal widget
 * @param source Release event of the tap
 * @param x,y Where the tap was, in the terminal
 * @return TRUE if a page was turned, FALSE if the tap is the program's
 */
static gboolean terminal_tap_page(GtkWidget *terminal, const GdkEventButton *source, gdouble x, gdouble y) {
    const glong rows = vte_terminal_get_row_count(VTE_TERMINAL(terminal));
    const glong cell = vte_terminal_get_char_height(VTE_TERMINAL(terminal));
    glong col = 0, row = 0;
    cell_at(terminal, x, y, &col, &row);
    if (cell <= 0 || rows < 2 || row >= rows || col >= vte_terminal_get_column_count(VTE_TERMINAL(terminal))) {
        return FALSE;
    }
    if (row < rows / 3.0) {
        return terminal_turn_page(terminal, source, x, y, col, row, 1);
    }
    if (row >= rows * 2 / 3.0) {
        return terminal_turn_page(terminal, source, x, y, col, row, -1);
    }
    return FALSE;
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
        } else {
            follow_history_drag(terminal, &any->motion);
        }
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
        if (event->type == GDK_BUTTON_PRESS && !touch.swiping && !touch.two_fingers
            && !touch.selecting && !touch.dragging_history) {
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
        touch.pressed_at = event->time;
        touch.state = event->state;
        touch.swiping = touch.two_fingers = touch.selecting = touch.dragging_history = FALSE;
        return TRUE;
    }
    const guint32 held_ms = event->time - touch.pressed_at;
    touch.pressed_at = 0;
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
    if (touch.dragging_history) {
        touch.dragging_history = FALSE;
        move_history_drag(terminal, dy, TRUE);
        return TRUE;
    }
    const gboolean vertical = ABS(dy) >= TAP_SLOP_PX && ABS(dy) >= ABS(dx) * 1.5;
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
    if (vertical) {
        if (held_ms <= FLICK_MAX_MS && ABS(dy) >= TOUCH_SWIPE_MIN_PX) {
            terminal_flick_page(terminal, event, touch.x, touch.y, dy);
        } else {
            terminal_drag_scroll(terminal, event, touch.x, touch.y, dy);
        }
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
