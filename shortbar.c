/* shortbar.c
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
 * The shortcut bar: one row of buttons above the keyboard, each typing a
 * whole key sequence with one tap (C-u, or the tmux prefix). The buttons
 * come from shortcuts.conf, see shortcuts.c, and are edited on the panel's
 * shortcuts page. More than fit in the row are on further pages of the
 * bar, reached by swiping it left or right: a whole page at a time, which
 * an e-ink screen shows more cleanly than a sliding row. The bar is on
 * screen while a keyboard is, the built-in one or the system one.
 */

#include <string.h>
#include <vte/vte.h>
#include "config.h"
#include "keyboard.h"
#include "panel.h"
#include "shortcuts.h"
#include "shortbar.h"

/** Buttons in the row when they all fit */
#define SHORTBAR_PER_PAGE 8
/** A finger moving this far sideways turns the page, upwards opens the panel */
#define SHORTBAR_SWIPE_PX 60

/** The one bar: an event box over its buttons, touches are sorted here */
static GtkWidget *bar = NULL;
static GtkWidget *problem_label = NULL;
static GtkWidget *row = NULL;
/** Places for the shortcuts, between the two tools */
static GtkWidget *slots = NULL;
/** At the ends of the row, half a place wide: the panel... */
static GtkWidget *menu_tool = NULL;
/** ...and the keyboard: folds it away (preview) or brings it back */
static GtkWidget *keyboard_tool = NULL;
static gchar *file_path = NULL;
/** Buttons, in order */
static GPtrArray *items = NULL;
/** First button of the page shown */
static guint page_start = 0;
/** Text last copied from the terminal: once there is some, a paste
 *  button leads the row */
static gchar *paste_text = NULL;
/** Where the finger went down */
static gdouble press_x = 0;
static gdouble press_y = 0;
static gboolean pressed = FALSE;

/** The two widgets whose being on screen decides the bar's */
static GtkWidget *keyboard_box = NULL;
static GtkWidget *other_keyboard = NULL;
static guint keyboard_rows = 0;

/**
 * @return Buttons on one page of the bar
 */
static guint per_page(void) {
    // the two tools take one place between them
    const guint places = SHORTBAR_PER_PAGE - 1 - (paste_text ? 1 : 0);
    // when there is more than one page, the last place says which page it is
    return items->len > places ? places - 1 : places;
}

/**
 * Show the bar while a keyboard is shown, and while previewing: there it
 * stays alone at the foot of the screen, its tools and shortcuts at hand
 */
static void update_visibility(void) {
    if (!bar || !keyboard_box) {
        return;
    }
    const gboolean keyboard = gtk_widget_get_visible(keyboard_box)
        || (other_keyboard && gtk_widget_get_visible(other_keyboard));
    gtk_widget_set_visible(bar, keyboard || kterm_previewing());
}

/*
 * Signs of the two tools: Material icons (Google, Apache License 2.0),
 * their SVG paths on a grid of 24 by 24
 */
/** "menu": the panel */
#define ICON_MENU "M3 18h18v-2H3v2zm0-5h18v-2H3v2zm0-7v2h18V6H3z"
/** "keyboard_hide": with the keyboard out, a tap folds it away */
#define ICON_KEYBOARD_HIDE "M20 3H4c-1.1 0-1.99.9-1.99 2L2 15c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2z" \
    "m-9 3h2v2h-2V6zm0 3h2v2h-2V9zM8 6h2v2H8V6zm0 3h2v2H8V9zm-1 2H5V9h2v2zm0-3H5V6h2v2zm9 7H8v-2h8v2z" \
    "m0-4h-2V9h2v2zm0-3h-2V6h2v2zm3 3h-2V9h2v2zm0-3h-2V6h2v2zm-7 15l4-4H8l4 4z"
/** "keyboard": while previewing, a tap brings it back */
#define ICON_KEYBOARD "M20 5H4c-1.1 0-1.99.9-1.99 2L2 17c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V7c0-1.1-.9-2-2-2z" \
    "m-9 3h2v2h-2V8zm0 3h2v2h-2v-2zM8 8h2v2H8V8zm0 3h2v2H8v-2zm-1 2H5v-2h2v2zm0-3H5V8h2v2zm9 7H8v-2h8v2z" \
    "m0-4h-2v-2h2v2zm0-3h-2V8h2v2zm3 3h-2v-2h2v2zm0-3h-2V8h2v2z"

/**
 * Read the next number of an SVG path
 * @param p Position in the path, moved past the number
 * @return The number, 0 if there is none
 */
static gdouble path_number(const gchar **p) {
    while (**p == ' ' || **p == ',') {
        (*p)++;
    }
    gchar *end = NULL;
    const gdouble value = g_ascii_strtod(*p, &end);
    if (end == *p) {
        // no number: step over whatever is there, so that reading ends
        if (**p) {
            (*p)++;
        }
        return 0;
    }
    *p = end;
    return value;
}

/**
 * Add an SVG path to the cairo path: the commands of the icons drawn here
 * (M L H V C S Z, absolute or relative), no arcs
 * @param cr Cairo context, its matrix set to the grid of the path
 * @param d The path
 */
static void svg_path(cairo_t *cr, const gchar *d) {
    gdouble x = 0, y = 0, start_x = 0, start_y = 0;
    // second control point of the last curve, for S
    gdouble ctrl_x = 0, ctrl_y = 0;
    gchar command = 0, last = 0;
    const gchar *p = d;
    while (*p) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (g_ascii_isalpha(*p)) {
            command = *p++;
        } else if (command == 'M') {
            // more points after a move are lines
            command = 'L';
        } else if (command == 'm') {
            command = 'l';
        }
        const gboolean relative = g_ascii_islower(command);
        const gdouble ox = relative ? x : 0, oy = relative ? y : 0;
        switch (g_ascii_toupper(command)) {
        case 'M':
            x = ox + path_number(&p);
            y = oy + path_number(&p);
            cairo_move_to(cr, x, y);
            start_x = x;
            start_y = y;
            break;
        case 'L':
            x = ox + path_number(&p);
            y = oy + path_number(&p);
            cairo_line_to(cr, x, y);
            break;
        case 'H':
            x = ox + path_number(&p);
            cairo_line_to(cr, x, y);
            break;
        case 'V':
            y = oy + path_number(&p);
            cairo_line_to(cr, x, y);
            break;
        case 'C':
        case 'S': {
            gdouble x1, y1;
            if (g_ascii_toupper(command) == 'C') {
                x1 = ox + path_number(&p);
                y1 = oy + path_number(&p);
            } else if (g_ascii_toupper(last) == 'C' || g_ascii_toupper(last) == 'S') {
                // the first control point mirrors the last one
                x1 = 2 * x - ctrl_x;
                y1 = 2 * y - ctrl_y;
            } else {
                x1 = x;
                y1 = y;
            }
            ctrl_x = ox + path_number(&p);
            ctrl_y = oy + path_number(&p);
            x = ox + path_number(&p);
            y = oy + path_number(&p);
            cairo_curve_to(cr, x1, y1, ctrl_x, ctrl_y, x, y);
            break;
        }
        case 'Z':
            cairo_close_path(cr);
            x = start_x;
            y = start_y;
            break;
        default:
            // not drawn here: stop rather than draw something wrong
            return;
        }
        last = command;
    }
}

/**
 * Draw a tool: its sign on the bar, without a key under it, which sets the
 * two tools apart from the shortcuts; drawn black on white, and white on
 * black while pressed. Drawn, not taken from a font: the fonts that have
 * such signs differ from one system to the next, and so did their look.
 * @param tool Tool widget
 * @param cr Cairo context
 * @param data Not used
 * @return FALSE
 */
static gboolean draw_tool_cb(GtkWidget *tool, cairo_t *cr, gpointer data) {
    UNUSED(data);
    GtkStyleContext *style = gtk_widget_get_style_context(tool);
    const gdouble w = gtk_widget_get_allocated_width(tool);
    const gdouble h = gtk_widget_get_allocated_height(tool);
    gtk_render_background(style, cr, 0, 0, w, h);
    gtk_render_frame(style, cr, 0, 0, w, h);
    GdkRGBA color;
    gtk_style_context_get_color(style, gtk_style_context_get_state(style), &color);
    gdk_cairo_set_source_rgba(cr, &color);
    // the 24 by 24 grid of the icon, 60 % of the tool, in its middle
    const gdouble size = MIN(w, h) * 0.6;
    cairo_save(cr);
    cairo_translate(cr, (w - size) / 2, (h - size) / 2);
    cairo_scale(cr, size / 24, size / 24);
    svg_path(cr, tool == menu_tool ? ICON_MENU : (kterm_previewing() ? ICON_KEYBOARD : ICON_KEYBOARD_HIDE));
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_fill(cr);
    cairo_restore(cr);
    return FALSE;
}

/**
 * Draw the keyboard tool again, for the state it changes
 */
static void update_keyboard_tool(void) {
    if (keyboard_tool) {
        gtk_widget_queue_draw(keyboard_tool);
    }
}

/**
 * The keyboard was folded away or brought back: the bar follows
 */
void shortbar_update(void) {
    update_keyboard_tool();
    update_visibility();
}

/**
 * Fill the row with the buttons of the page shown
 */
static void fill_row(void) {
    GList *old = gtk_container_get_children(GTK_CONTAINER(slots));
    for (GList *l = old; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(old);
    const guint count = per_page();
    if (page_start >= items->len) {
        page_start = items->len ? ((items->len - 1) / count) * count : 0;
    }
    if (paste_text) {
        GtkWidget *paste = gtk_label_new("粘贴");
        gtk_widget_set_name(paste, SHORTBAR_PASTE_NAME);
        g_object_set_data(G_OBJECT(paste), "paste", GINT_TO_POINTER(1));
        gtk_box_pack_start(GTK_BOX(slots), paste, TRUE, TRUE, 0);
    }
    for (guint i = page_start; i < items->len && i < page_start + count; i++) {
        const Shortcut *item = g_ptr_array_index(items, i);
        // A label drawn as a key, not a button: a button has an input
        // window of its own, put above the bar's when the row is filled
        // again, and the bar would miss the touches it needs to tell a
        // swipe from a tap.
        GtkWidget *button = gtk_label_new(item->label);
        gtk_widget_set_name(button, SHORTBAR_BUTTON_NAME);
        gtk_label_set_ellipsize(GTK_LABEL(button), PANGO_ELLIPSIZE_END);
        g_object_set_data(G_OBJECT(button), "index", GUINT_TO_POINTER(i + 1));
        gtk_box_pack_start(GTK_BOX(slots), button, TRUE, TRUE, 0);
    }
    if (items->len > count) {
        // the last page may be short: keep the places, so that the page
        // number stays at the end of the row
        for (guint i = items->len - page_start; i < count; i++) {
            gtk_box_pack_start(GTK_BOX(slots), gtk_label_new(NULL), TRUE, TRUE, 0);
        }
        gchar *text = g_strdup_printf("%u/%u", page_start / count + 1, (items->len + count - 1) / count);
        GtkWidget *pages = gtk_label_new(text);
        gtk_widget_set_name(pages, SHORTBAR_PAGES_NAME);
        g_free(text);
        gtk_box_pack_start(GTK_BOX(slots), pages, TRUE, TRUE, 0);
    }
    gtk_widget_show_all(slots);
}

/**
 * Turn the page
 * @param step -1 or 1
 */
static void turn_page(gint step) {
    const guint count = per_page();
    if (items->len <= count) {
        return;
    }
    const guint pages = (items->len + count - 1) / count;
    const guint page = page_start / count;
    // round: past the last page comes the first one
    page_start = ((page + pages + (guint) (step > 0 ? 1 : pages - 1)) % pages) * count;
    fill_row();
}

/**
 * @param x,y Position in the bar
 * @return The button there, NULL if none
 */
static GtkWidget * button_at(gdouble x, gdouble y) {
    GtkWidget *hit = NULL;
    GList *buttons = gtk_container_get_children(GTK_CONTAINER(slots));
    buttons = g_list_prepend(buttons, menu_tool);
    buttons = g_list_append(buttons, keyboard_tool);
    for (GList *l = buttons; l && !hit; l = l->next) {
        GtkWidget *button = l->data;
        gint bx = 0, by = 0;
        if ((g_object_get_data(G_OBJECT(button), "index") || g_object_get_data(G_OBJECT(button), "paste")
             || g_object_get_data(G_OBJECT(button), "tool"))
            && gtk_widget_translate_coordinates(bar, button, (gint) x, (gint) y, &bx, &by)
            && bx >= 0 && by >= 0 && bx < gtk_widget_get_allocated_width(button)
            && by < gtk_widget_get_allocated_height(button)) {
            hit = button;
        }
    }
    g_list_free(buttons);
    return hit;
}

/** The button under the finger, drawn pressed */
static GtkWidget *pressed_button = NULL;

/**
 * Draw a button released again
 * @param data Button, referenced
 * @return G_SOURCE_REMOVE
 */
static gboolean unpress_cb(gpointer data) {
    g_object_set_data(G_OBJECT(data), "unpress", NULL);
    gtk_style_context_remove_class(gtk_widget_get_style_context(GTK_WIDGET(data)), "pressed");
    g_object_unref(data);
    return G_SOURCE_REMOVE;
}

/**
 * Draw the button under the finger pressed, as the keys are
 * @param button Button, NULL for none
 */
static void press(GtkWidget *button) {
    pressed_button = button;
    if (!button) {
        return;
    }
    // tapped again while still drawn pressed: stays so
    const guint pending = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "unpress"));
    if (pending) {
        g_source_remove(pending);
        g_object_set_data(G_OBJECT(button), "unpress", NULL);
        g_object_unref(button);
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "pressed");
}

/**
 * The finger has left the bar: the button pressed is drawn as before, at
 * once, or for a tap a moment later, as the keys are (KB_PRESSED_SHOW_MS)
 * @param tapped TRUE for a tap
 */
static void unpress(gboolean tapped) {
    if (!pressed_button) {
        return;
    }
    g_object_ref(pressed_button);
    if (tapped) {
        const guint source = g_timeout_add(KB_PRESSED_SHOW_MS, unpress_cb, pressed_button);
        g_object_set_data(G_OBJECT(pressed_button), "unpress", GUINT_TO_POINTER(source));
    } else {
        unpress_cb(pressed_button);
    }
    pressed_button = NULL;
}

/**
 * Show or take away the paste button, staying on the same page
 * @param text Text to paste, NULL for none
 */
static void set_paste(const gchar *text) {
    const guint page = page_start / per_page();
    g_free(paste_text);
    paste_text = g_strdup(text);
    page_start = page * per_page();
    fill_row();
    update_visibility();
}

/**
 * Offer text copied from the terminal: a paste button leads the bar from
 * then on, pasting the last text copied
 * @param text Text, copied
 */
void shortbar_offer_paste(const gchar *text) {
    set_paste(text);
}

/**
 * Type the keys of the button under the finger, or paste
 * @param x Position in the bar
 * @param y Position in the bar
 */
static void tap(gdouble x, gdouble y) {
    GtkWidget *terminal = kterm_get_terminal();
    // not while a form of the panel is being filled in, as for the keys
    if (!terminal || panel_wants_keys()) {
        return;
    }
    GtkWidget *hit = button_at(x, y);
    if (!hit) {
        return;
    }
    const guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(hit), "index"));
    if (hit == menu_tool) {
        panel_open();
    } else if (hit == keyboard_tool) {
        kterm_set_preview(!kterm_previewing());
    } else if (g_object_get_data(G_OBJECT(hit), "paste")) {
        // as if pasted from a menu: a program that asked for it (Claude
        // Code, a shell) is told where the pasted text starts and ends
        // the button stays: a paste undone can be pasted again
        vte_terminal_paste_text(VTE_TERMINAL(terminal), paste_text);
        kterm_key_typed();
    } else if (index) {
        const Shortcut *item = g_ptr_array_index(items, index - 1);
        gsize size = 0;
        const gchar *keys = g_bytes_get_data(item->keys, &size);
        vte_terminal_feed_child(VTE_TERMINAL(terminal), keys, (gssize) size);
        if (item->keyboard && kterm_previewing()) {
            // a prefix: the keyboard comes out for the key that follows
            kterm_set_preview(FALSE);
            kterm_preview_after_key();
        } else {
            kterm_key_typed();
        }
    }
}

/**
 * Touches on the bar. The Kindle reports a swipe as button 1 going down
 * and up, with a click of button 3 on the way: only button 1 counts.
 */
static gboolean touch_cb(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    UNUSED(widget);
    UNUSED(data);
    if (event->button != 1) {
        return TRUE;
    }
    if (event->type == GDK_BUTTON_PRESS) {
        press_x = event->x;
        press_y = event->y;
        pressed = TRUE;
        press(button_at(press_x, press_y));
        return TRUE;
    }
    if (event->type != GDK_BUTTON_RELEASE || !pressed) {
        return TRUE;
    }
    pressed = FALSE;
    const gdouble dx = event->x - press_x;
    const gdouble dy = event->y - press_y;
    const gboolean tapped = ABS(dx) < SHORTBAR_SWIPE_PX && ABS(dy) < SHORTBAR_SWIPE_PX;
    unpress(tapped && !(panel_is_open() && !panel_wants_keys()));
    if (ABS(dx) >= SHORTBAR_SWIPE_PX && ABS(dx) > ABS(dy)) {
        // towards the left brings the next page in, like turning a page
        turn_page(dx < 0 ? 1 : -1);
    } else if (dy <= -SHORTBAR_SWIPE_PX && ABS(dy) > ABS(dx)) {
        // up from the bar opens the panel, as from the keyboard
        panel_open();
    } else if (ABS(dx) < SHORTBAR_SWIPE_PX && ABS(dy) < SHORTBAR_SWIPE_PX) {
        if (panel_is_open() && !panel_wants_keys()) {
            // as on the keyboard: a tap puts the panel away, types nothing
            panel_close();
        } else {
            tap(press_x, press_y);
        }
    }
    return TRUE;
}

/**
 * Show the buttons again, from the first page
 * @param problems Mistakes found in the file, or NULL
 */
static void rebuild(const gchar *problems) {
    if (problems) {
        // the first mistake is enough to find the file and the line
        gchar *first = g_strndup(problems, strcspn(problems, "\n"));
        gchar *text = g_strdup_printf("%s %s", file_path ? file_path : SHORTCUTS_FILE, first);
        gtk_label_set_text(GTK_LABEL(problem_label), text);
        g_free(text);
        g_free(first);
    }
    gtk_widget_set_visible(problem_label, problems != NULL);
    page_start = 0;
    fill_row();
    update_visibility();
}

/**
 * Build the bar
 * @param path shortcuts.conf; the built-in buttons are used when it does
 *  not exist
 * @return Bar widget
 */
GtkWidget * shortbar_new(const gchar *path) {
    file_path = g_strdup(path);
    gchar *problems = NULL;
    items = shortcuts_load(path, &problems);

    bar = gtk_event_box_new();
    gtk_widget_set_name(bar, SHORTBAR_NAME);
    // the box takes every touch, to tell taps from swipes
    gtk_widget_add_events(bar, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(bar, "button-press-event", G_CALLBACK(touch_cb), NULL);
    g_signal_connect(bar, "button-release-event", G_CALLBACK(touch_cb), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(bar), box);
    problem_label = gtk_label_new(NULL);
    gtk_widget_set_name(problem_label, SHORTBAR_PROBLEM_NAME);
    gtk_label_set_xalign(GTK_LABEL(problem_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(problem_label), PANGO_ELLIPSIZE_START);
    gtk_widget_set_no_show_all(problem_label, TRUE);
    gtk_box_pack_start(GTK_BOX(box), problem_label, FALSE, FALSE, 0);
    // [panel] shortcuts, all as wide [keyboard]
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    menu_tool = gtk_drawing_area_new();
    keyboard_tool = gtk_drawing_area_new();
    slots = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(slots), TRUE);
    // the line under the bar runs under the shortcuts only: they belong
    // together, the tools stand apart
    gtk_widget_set_name(slots, SHORTBAR_SLOTS_NAME);
    gtk_box_pack_start(GTK_BOX(row), menu_tool, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), slots, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), keyboard_tool, FALSE, FALSE, 0);
    for (GtkWidget **tool = (GtkWidget *[]) { menu_tool, keyboard_tool, NULL }; *tool; tool++) {
        gtk_widget_set_name(*tool, SHORTBAR_TOOL_NAME);
        g_object_set_data(G_OBJECT(*tool), "tool", GINT_TO_POINTER(1));
        g_signal_connect(*tool, "draw", G_CALLBACK(draw_tool_cb), NULL);
    }
    update_keyboard_tool();
    gtk_widget_show_all(box);
    // shown and hidden with the keyboard only, see update_visibility()
    gtk_widget_set_no_show_all(bar, TRUE);
    rebuild(problems);
    g_free(problems);
    return bar;
}

/**
 * @return The buttons, in order; changed by the panel's page, which then
 *  calls shortbar_save()
 */
GPtrArray * shortbar_items(void) {
    return items;
}

/**
 * Write the buttons to the file and show them
 * @param error Set when the file could not be written (the bar shows the
 *  buttons anyway)
 * @return TRUE on success
 */
gboolean shortbar_save(GError **error) {
    const gboolean saved = shortcuts_save(file_path, items, error);
    // lines of the file that make no button are kept in it: still tell
    gchar *problems = NULL;
    g_ptr_array_unref(shortcuts_load(file_path, &problems));
    rebuild(problems);
    g_free(problems);
    return saved;
}

static void follow_visibility_cb(GtkWidget *widget, gpointer data) {
    UNUSED(widget);
    UNUSED(data);
    update_visibility();
}

/**
 * Buttons as tall as the keys: the keyboard sets its height once it knows
 * the screen.
 */
static void follow_height_cb(GObject *object, GParamSpec *spec, gpointer data) {
    UNUSED(spec);
    UNUSED(data);
    // as tall as the keyboard's first row (special keys, a short one)
    gint height = GPOINTER_TO_INT(g_object_get_data(object, KB_FIRST_ROW_HEIGHT));
    if (height <= 0 && keyboard_rows > 0) {
        gtk_widget_get_size_request(GTK_WIDGET(object), NULL, &height);
        height /= (gint) keyboard_rows;
    }
    if (height > 0) {
        gtk_widget_set_size_request(row, -1, height);
        // the tools are half a place wide: as wide as tall
        gtk_widget_set_size_request(menu_tool, height, -1);
        gtk_widget_set_size_request(keyboard_tool, height, -1);
    }
}

/**
 * Show the bar while a keyboard is shown, with buttons as tall as its keys
 * @param keyboard Built-in keyboard
 * @param rows Rows of keys in it
 * @param other Shown instead of the keyboard while the system keyboard is
 *  up, or NULL
 */
void shortbar_follow(GtkWidget *keyboard, guint rows, GtkWidget *other) {
    keyboard_box = keyboard;
    other_keyboard = other;
    keyboard_rows = rows;
    g_signal_connect_after(keyboard, "show", G_CALLBACK(follow_visibility_cb), NULL);
    g_signal_connect_after(keyboard, "hide", G_CALLBACK(follow_visibility_cb), NULL);
    if (other) {
        g_signal_connect_after(other, "show", G_CALLBACK(follow_visibility_cb), NULL);
        g_signal_connect_after(other, "hide", G_CALLBACK(follow_visibility_cb), NULL);
    }
    g_signal_connect(keyboard, "notify::height-request", G_CALLBACK(follow_height_cb), NULL);
    follow_height_cb(G_OBJECT(keyboard), NULL, NULL);
    update_visibility();
}
