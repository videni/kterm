/* keyboard.c
 *
 * This file is part of kterm
 *
 * Copyright(C) 2016 Bartek Fabiszewski (www.fabiszewski.net)
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
#include <string.h>
#include "keyboard.h"
#include "panel.h"
#include "config.h"

#if GTK_CHECK_VERSION(3,0,0)
/** 
 * Get gdk keyboard device
 * @return GdkDevice device. This object must not be freed.
 */
static GdkDevice * getkbdevice(void) {
    GdkDevice *device = NULL;
#if GTK_CHECK_VERSION(3,20,0)
    GdkDisplay *display = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(display);
    device = gdk_seat_get_keyboard(seat);
#else
    GdkDeviceManager *manager = gdk_display_get_device_manager(gdk_display_get_default());
    GList *list = gdk_device_manager_list_devices(manager, GDK_DEVICE_TYPE_MASTER);
    
    for (GList *cur = list; cur != NULL; cur = cur->next) {
        if (gdk_device_get_source(cur->data) == GDK_SOURCE_KEYBOARD) {
            device = cur->data;
        }
    }
    g_list_free(list);
#endif
    return device;
}
#endif

/**
 * Translate modifier mask to layout variant
 * @param modifier_mask Modifier mask
 * @return KBtype layout variant
 */
static KBtype kbstate_to_kbtype(const guint32 modifier_mask) {
    KBtype type = KBT_DEFAULT;
    if (modifier_mask & (GDK_LOCK_MASK | GDK_SHIFT_MASK)) {
        type = KBT_SHIFT;
    } else if (modifier_mask & GDK_MOD2_MASK) {
        type = KBT_MOD1;
    } else if (modifier_mask & GDK_MOD3_MASK) {
        type = KBT_MOD2;
    } else if (modifier_mask & GDK_MOD4_MASK) {
        type = KBT_MOD3;
    }
    return type;
}

/**
 * Is caps lock selected and caps shift deslected
 * @param keyboard Keyboard structure
 * @return True if only caps lock selected
 */
static gboolean modifier_only_caps(const Keyboard *keyboard) {
    return (keyboard->modifier_mask & GDK_LOCK_MASK) && !(keyboard->modifier_mask & GDK_SHIFT_MASK);
}

/**
 * Markup for the hint line of a key: what shift and the symbol key turn it
 * into. Variants that add nothing (upper case of the same letter, same label)
 * are left out.
 * @param key Key structure
 * @return Newly allocated markup, or NULL if the key needs no hint
 */
static gchar * key_hint_markup(const Key *key) {
    const gchar *base = key->label[KBT_DEFAULT];
    const gchar *hints[2] = { key->label[KBT_SHIFT], key->label[KBT_MOD1] };
    if (!base) {
        return NULL;
    }
    gchar *upper = g_utf8_strup(base, -1);
    GString *line = g_string_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(hints); i++) {
        if (!hints[i] || !strcmp(hints[i], base) || !strcmp(hints[i], upper)) {
            continue;
        }
        if (i > 0 && hints[0] && !strcmp(hints[i], hints[0])) {
            continue;
        }
        gchar *escaped = g_markup_escape_text(hints[i], -1);
        g_string_append_printf(line, "%s%s", line->len ? "  " : "", escaped);
        g_free(escaped);
    }
    g_free(upper);
    return g_string_free(line, line->len == 0);
}

/**
 * Show the text label of a layout variant on a key. In the default variant
 * a small line above it tells what the other variants produce, because
 * nothing else on screen does.
 * @param key Key structure
 * @param type Layout variant, must have a text label
 */
static void key_set_label(Key *key, const KBtype type) {
    gtk_button_set_label(GTK_BUTTON(key->button), key->label[type]);
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(key->button));
    if (!GTK_IS_LABEL(child)) {
        return;
    }
    gchar *hint = (type == KBT_DEFAULT) ? key_hint_markup(key) : NULL;
    if (hint) {
        gchar *text = g_markup_escape_text(key->label[type], -1);
        gchar *markup = g_strdup_printf("<span size=\"x-small\" foreground=\"#606060\">%s</span>\n%s", hint, text);
        gtk_label_set_markup(GTK_LABEL(child), markup);
        gtk_label_set_justify(GTK_LABEL(child), GTK_JUSTIFY_CENTER);
        g_free(markup);
        g_free(text);
        g_free(hint);
    }
}

/**
 * Set layout variant based on modifiers set
 * @param keyboard Keyboard structure
 */
static void keyboard_set_layout(const Keyboard *keyboard) {
    KBtype kb_type = kbstate_to_kbtype(keyboard->modifier_mask);
    gboolean only_caps = modifier_only_caps(keyboard);
    D printf("setting layout %d, caps: %i\n", kb_type, only_caps);
    for (guint i = 0; i < keyboard->key_count; i++) {
        Key *key = keyboard->keys[i];
        KBtype type = kb_type;
        if (only_caps && !key->obey_caps) {
            type = KBT_DEFAULT;
        }
        if (key->image[type]) {
            if (key->image[type] != gtk_button_get_image(GTK_BUTTON(key->button))) {
                g_object_ref(key->image[type]);
                gtk_button_set_image(GTK_BUTTON(key->button), key->image[type]);
                gtk_button_set_image_position(GTK_BUTTON(key->button), GTK_POS_BOTTOM);
            }
            if (gtk_button_get_label(GTK_BUTTON(key->button))) {
                gtk_button_set_label(GTK_BUTTON(key->button), NULL);
            }
        } else if (key->label[type]) {
            key_set_label(key, type);
            if (gtk_button_get_image(GTK_BUTTON(key->button))) {
                gtk_button_set_image(GTK_BUTTON(key->button), NULL);
            }
        }
    }
}

/**
 * Send synthetized key event
 * @param key Key structure
 * @param event_type Gdk key event type (press or release)
 * @param keystate Key state
 * @param keyval Key value
 * @return True on success, false otherwise
 */
static gboolean send_key_event(Key *key, GdkEventType event_type, guint keystate, guint keyval) {
    GdkKeymapKey *keys = NULL;
    gint n_keys = 0;
    if (!gdk_keymap_get_entries_for_keyval(gdk_keymap_get_default(), keyval, &keys, &n_keys)) {
        return FALSE;
    }
    GdkEvent *event = gdk_event_new(event_type);
    event->key.window = g_object_ref(gtk_widget_get_window(GTK_WIDGET(key->button)));
    event->key.state = keystate;
    if (!key->modifier) { event->key.state |= (guint) keys[0].level; }
    event->key.hardware_keycode = (guint16) keys[0].keycode;
    event->key.keyval = keyval;
    event->key.send_event = FALSE;
    event->key.time = gtk_get_current_event_time();
    event->key.is_modifier = (key->modifier != 0);
    event->key.group = (guint8) keys[0].group;
#if GTK_CHECK_VERSION(3,0,0)
    // gtk3 complains about fake device
    gdk_event_set_device(event, getkbdevice());
#endif
    g_free(keys);
    gtk_main_do_event(event);
    gdk_event_free(event);
    return TRUE;
}

/**
 * Deactivate modifier buttons based on modifiers set
 * @param keyboard Keyboard structure
 */
static void keyboard_reset_modifiers(Keyboard *keyboard) {
    D printf("resetting modifier_mask\n");
    for (guint i = 0; i < keyboard->key_count; i++) {
        Key *key = keyboard->keys[i];
        if (key->modifier && (keyboard->modifier_mask & key->modifier) && key->modifier != GDK_LOCK_MASK) {
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(key->button))) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(key->button), FALSE);
                send_key_event(key, GDK_KEY_RELEASE, 1, key->keyval[KBT_DEFAULT]);
            }
        }
    }
    keyboard->modifier_mask &= GDK_LOCK_MASK;
}

/**
 * Calculate and set key button sizes
 * @param data Keyboard structure
 * @return Always false to cancel timeout, as called with g_timeout_add();
 */
gboolean keyboard_set_size(gpointer data) {
    Keyboard *keyboard = data;
    if G_UNLIKELY(keyboard == NULL || keyboard->key_count == 0 || keyboard->row_count == 0) {
        return FALSE;
    }
    GtkWidget *keyboard_box = keyboard->container;
    // The parser labels keys one variant at a time; the hint lines need all
    // variants, so (re)apply the labels once the layout is complete.
    keyboard_set_layout(keyboard);
    // update geometry
    GdkScreen *screen = gdk_screen_get_default();
    gint screen_height = gdk_screen_get_height(screen);
    gint screen_width = gdk_screen_get_width(screen);
    GtkWidget *window = gtk_widget_get_toplevel(keyboard_box);
    GtkAllocation* alloc = g_new(GtkAllocation, 1);
    gtk_widget_get_allocation(window, alloc);
    gint window_width = alloc->width;
    gint window_height = alloc->height;
    g_free(alloc);
#ifdef KINDLE
    // The window always covers the screen, but this can run before the
    // window manager has resized it; keys sized from the early, narrower
    // allocation would leave the right side of the keyboard empty.
    window_width = screen_width;
#endif
    gboolean is_portrait = FALSE;
    if (window_width < screen_height) {
        is_portrait = TRUE;
    }
    // count units per row
    guint units_row_max = 0;
    Key **p = keyboard->keys;
    for (guint i = 0; i < keyboard->row_count; i++) {
        guint units = 0;
        for (guint j = 0; j < keyboard->key_per_row[i]; j++) {
            Key *key = *p++;
            if (key->extended && is_portrait) {
                continue;
            }
            if (key->width) {
                units += key->width;
            } else {
                units += KEY_UNIT;
            }
        }
        // round up units per row
        units = (units + (KEY_UNIT - 1)) / KEY_UNIT;
        units_row_max = MAX(units_row_max, units);
    }
    guint unit_hmin = keyboard->unit_height;
    guint unit_wmax = (units_row_max) ? ((guint) window_width / units_row_max) : 0;
    guint unit_wmin = keyboard->unit_width;
    // add padding and border
    GtkWidget *first = keyboard->keys[0]->button;
#if GTK_CHECK_VERSION(3,0,0)
    GtkStyleContext *style = gtk_widget_get_style_context(first);
    GtkBorder extra = {0, 0, 0, 0};
    gtk_style_context_get_padding(style, 0, &extra);
    unit_wmin += (guint) (extra.left + extra.right);
    unit_hmin += (guint) (extra.top + extra.bottom);
    gtk_style_context_get_border(style, 0, &extra);
    unit_wmin += (guint) (extra.left + extra.right);
    unit_hmin += (guint) (extra.top + extra.bottom);
#else
    GtkStyle *style = gtk_widget_get_style(first);
    unit_wmin += (guint) (2 * style->xthickness);
    unit_hmin += (guint) (2 * style->ythickness);
    GtkBorder *border = NULL;
    gtk_widget_style_get(first, "inner-border", &border, NULL);
    if (border) {
        unit_wmin += (guint) (border->left + border->right);
        unit_hmin += (guint) (border->top + border->bottom);
        gtk_border_free(border);
    }
#endif
    // Keys with a hint line are taller than keys without. Reserve that
    // height always: if the keyboard changed height with the variant shown,
    // the terminal would be resized on every shift or symbol key press, and
    // programs such as vi drop their command line when that happens.
    PangoLayout *two_lines = gtk_widget_create_pango_layout(first, NULL);
    pango_layout_set_markup(two_lines, "<span size=\"x-small\">Xg</span>\nXg", -1);
    gint two_lines_height = 0;
    pango_layout_get_pixel_size(two_lines, NULL, &two_lines_height);
    g_object_unref(two_lines);
    unit_hmin = MAX(unit_hmin, (guint) two_lines_height + (unit_hmin - keyboard->unit_height));

    guint unit_w = MAX(unit_wmax, unit_wmin);
    D printf("wmin: %d, wmax: %d => %d\n", unit_wmin, unit_wmax, unit_w);
    for (guint i = 0; i < keyboard->key_count; i++) {
        Key *key = keyboard->keys[i];
        guint width = unit_w;
        if (key->width) {
            width *= key->width;
            width /= KEY_UNIT;
        }
        if (key->extended && is_portrait) {
            if (gtk_widget_get_visible(key->button)) {
                gtk_widget_hide(key->button);
            }
            continue;
        } else if (key->extended) {
            if (!gtk_widget_get_visible(key->button)) {
                gtk_widget_show(key->button);
            }
        }
        if (key->fill) {
            gtk_box_set_child_packing(GTK_BOX(gtk_widget_get_parent(key->button)), key->button, TRUE, TRUE, 0, GTK_PACK_START);
        } else {
            gtk_widget_set_size_request(key->button, (gint) width, -1);
        }
    }
    gint kb_width = window_width;
    
    // calculate keyboard widget height
    gdouble dpi = gdk_screen_get_resolution(screen);
    D printf("window size: %ix%i\n", window_width, window_height);
    D printf("screen size: %ix%i (%i dpi)\n", screen_width, screen_height, (gint) dpi);
    if (dpi < 0) { dpi = 96; }
    guint unit_h = unit_hmin;
    const guint unit_hpref = (guint) (KB_KEYHEIGHT_PREF * dpi);
    const guint unit_hmax = (guint) screen_height / KB_HEIGHTMAX_FACTOR / keyboard->row_count;
    if (unit_hmin > unit_hmax || unit_hpref > unit_hmax) {
        unit_h = unit_hmax;
    }
    else if (unit_hpref >= unit_hmin) {
        unit_h = unit_hpref;
    }
    // never below what the labels need, or the box grows on its own
    unit_h = MAX(unit_h, unit_hmin);
    D printf("hmin: %d, hmax: %d, pref: %d => %d\n", unit_hmin, unit_hmax, unit_hpref, unit_h);
    gint kb_height = (gint) (unit_h * keyboard->row_count);
    D printf("keyboard size: %ix%i\n", kb_width, kb_height);
    gtk_widget_set_size_request(keyboard_box, -1, kb_height);
    return FALSE;
}

/**
 * Key press event callback
 * @param key Event key structure
 * @return True on success, false otherwise
 */
static gboolean keyboard_event_press(Key *key) {
    Keyboard *keyboard = key->keyboard;
    GtkWidget *button = key->button;
    if (key->modifier) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)));
    } else {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
    }
    KBtype kb_type = kbstate_to_kbtype(keyboard->modifier_mask);
    if (modifier_only_caps(keyboard)) {
        // only caps lock
        if (!key->obey_caps) { kb_type = KBT_DEFAULT; }
    }
    D printf("press: %s (%i)\n", gdk_keyval_name(key->keyval[kb_type]), key->keyval[kb_type]);
    D printf("modifier: %u\n", key->modifier);
    D printf("modifier_mask: %u\n", keyboard->modifier_mask);
    guint keyval = key->keyval[kb_type];
    if (!keyval) {
        if (kb_type == KBT_DEFAULT || (keyval = key->keyval[KBT_DEFAULT]) == 0) {
            D printf("Empty action\n");
            return TRUE;
        }
    }
    if (keyval == KTERM_KEY_QUIT) {
        gtk_main_quit();
        return TRUE;
    }
    if (keyval == KTERM_KEY_IME) {
        // acts on release, see keyboard_event_release()
        return TRUE;
    }
    GdkEventType event_type = GDK_KEY_PRESS;
    guint key_state = (keyboard->modifier_mask & KB_MODIFIERS_BASIC_MASK);
    if (key->modifier) {
        if (keyboard->modifier_mask & key->modifier) {
            event_type = GDK_KEY_RELEASE;
        }
        keyboard->modifier_mask ^= key->modifier;
        if (kbstate_to_kbtype(key->modifier) != KBT_DEFAULT) {
            keyboard_set_layout(keyboard);
        }
    }

    return send_key_event(key, event_type, key_state, keyval);
}

/**
 * Key release event callback
 * @param data Event key structure
 * @return Always false to cancel timout, as called by g_timeout_add()
 */
static gboolean keyboard_event_release(gpointer data) {
    Key *key = data;
    Keyboard *keyboard = key->keyboard;
    GtkWidget *button = key->button;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
    KBtype kb_type = kbstate_to_kbtype(keyboard->modifier_mask);
    if (modifier_only_caps(keyboard) && !key->obey_caps) {
        kb_type = KBT_DEFAULT;
    }
    D printf("release: %s (%i)\n", gdk_keyval_name(key->keyval[kb_type]), key->keyval[kb_type]);
    guint keyval = 0;
    if ((keyval = key->keyval[kb_type]) == 0 && (keyval = key->keyval[KBT_DEFAULT]) == 0) {
        D printf("Empty action\n");
        return FALSE;
    }
    
    if (keyval == KTERM_KEY_QUIT) {
        return FALSE;
    }
    if (keyval == KTERM_KEY_IME) {
        // Switching hides this keyboard. Doing it on press would lose the
        // release event and leave the key drawn as held down.
        kterm_toggle_ime();
        return FALSE;
    }

    guint key_state = (keyboard->modifier_mask & KB_MODIFIERS_BASIC_MASK);

    send_key_event(key, GDK_KEY_RELEASE, key_state, keyval);
    if (keyboard->modifier_mask & KB_MODIFIERS_SET_MASK) {
        keyboard_reset_modifiers(keyboard);
        keyboard_set_layout(keyboard);
    }
    return FALSE;
}

/**
 * Key release event callback
 * @param button Key button widget
 * @param key Key structure
 */
static void keyboard_terminal_feed(GtkWidget *button, const Key *key) {
    UNUSED(button);
    // send characters directly via vte_terminal_feed_child()
    GtkWidget *terminal = kterm_get_terminal();
    // not while a form of the panel is being filled in: this path bypasses
    // the keyboard focus, and the character would land in the terminal
    if G_UNLIKELY(!terminal || panel_wants_keys()) {
        return;
    }

    Keyboard *keyboard = key->keyboard;
    KBtype kb_type = kbstate_to_kbtype(keyboard->modifier_mask);
    if (modifier_only_caps(keyboard)) {
        // only caps lock
        if (!key->obey_caps) { kb_type = KBT_DEFAULT; }
    }
    gchar utf[6];
    gint utf_len = g_unichar_to_utf8(gdk_keyval_to_unicode(key->keyval[kb_type]), utf);
    D printf("feed child: %i chars\n", utf_len);
    vte_terminal_feed_child(VTE_TERMINAL(terminal), utf, utf_len);
}

/**
 * Key event callback
 * @param button Key button widget
 * @param ev Gdk event
 * @param key Key structure
 * @return True to stop processing event, false otherwise
 */
gboolean keyboard_event(GtkWidget *button, GdkEvent *ev, Key *key) {
    // A key acts when the finger leaves it, not when it lands: only then is
    // it known whether this was a tap on the key or the start of a swipe
    // across the keyboard.
    static Key *pressed = NULL;
    static gdouble press_x = 0, press_y = 0;
    D printf("key button event: type %i at %.0f,%.0f\n", ev->type, ev->button.x_root, ev->button.y_root);
    if ((ev->type == GDK_BUTTON_PRESS || ev->type == GDK_BUTTON_RELEASE) && ev->button.button != 1) {
        // The Kindle's touch driver clicks button 3 in the middle of a
        // swipe, while button 1 is still down. That is not a second touch.
        return TRUE;
    }
    if (ev->type == GDK_BUTTON_PRESS) {
        pressed = key;
        press_x = ev->button.x_root;
        press_y = ev->button.y_root;
        if (!key->modifier) {
            // show the touch at once
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
        }
        return TRUE;
    } else if (ev->type == GDK_BUTTON_RELEASE) {
        if (pressed != key) {
            return TRUE;
        }
        pressed = NULL;
        const gdouble dx = ev->button.x_root - press_x;
        const gdouble dy = ev->button.y_root - press_y;
        if (ABS(dy) >= KB_SWIPE_MIN_PX && ABS(dy) > ABS(dx)) {
            if (!key->modifier) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
            }
            // up from the keyboard opens the panel, down puts it away
            if (dy < 0) {
                panel_open();
            } else {
                panel_close();
            }
            return TRUE;
        }
        if (!keyboard_event_press(key)) {
            // not in keymap, try workaround
            keyboard_terminal_feed(button, key);
        }
        if (!key->modifier) {
            g_timeout_add(KB_RELEASE_DELAY_MS, keyboard_event_release, key);
        }
        return TRUE;
    }
    return FALSE;
}

/**
 * Free key structure
 * @param key Key structure
 */
void keyboard_key_free(Key *key) {
    if (key) {
        if (key->button && GTK_IS_WIDGET(key->button)) { gtk_widget_destroy(key->button); }
        for (gint i = 0; i < KBT_COUNT; i++) {
            if (key->image[i] && GTK_IS_WIDGET(key->image[i])) { gtk_widget_destroy(key->image[i]); }
            if (key->label[i]) { g_free(key->label[i]); }
        }
        g_free(key);
        key = NULL;
    }
}

/**
 * Recursively free array of key structures
 * @param keys Array of key structures
 */
static void keyboard_keys_free(Key **keys) {
    if (keys == NULL) {
        return;
    }
    guint count = (*keys && (*keys)->keyboard) ? (*keys)->keyboard->key_count : 0;
    for (guint i = 0; i < count; i++) {
        if (keys[i] == NULL) { break; }
        keyboard_key_free(keys[i]);
    }
    g_free(keys);
}

/**
 * Recursively free keyboard structure
 * @param keyboard structure
 */
void keyboard_free(Keyboard **keyboard) {
    if (keyboard && *keyboard) {
        keyboard_keys_free((*keyboard)->keys);
        (*keyboard)->keys = NULL;
        g_free(*keyboard);
        *keyboard = NULL;
    }
}
