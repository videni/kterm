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
 * Add or take away a style class, only if that changes something: a change
 * redraws the widget
 * @param widget Widget
 * @param name Class name
 * @param on TRUE to have it
 */
static void style_class_set(GtkWidget *widget, const gchar *name, gboolean on) {
#if GTK_CHECK_VERSION(3,0,0)
    GtkStyleContext *style = gtk_widget_get_style_context(widget);
    if (on && !gtk_style_context_has_class(style, name)) {
        gtk_style_context_add_class(style, name);
    } else if (!on && gtk_style_context_has_class(style, name)) {
        gtk_style_context_remove_class(style, name);
    }
#else
    UNUSED(widget);
    UNUSED(name);
    UNUSED(on);
#endif
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
            // GTK rebuilds the label whenever it is set: only the keys that
            // change are redrawn, the rest of the e-ink screen stays still
            if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(key->button)), key->label[type])) {
                gtk_button_set_label(GTK_BUTTON(key->button), key->label[type]);
            }
            // a label never widens its key, which would push the row and
            // the whole window past the screen
            GtkWidget *label = gtk_bin_get_child(GTK_BIN(key->button));
            if (GTK_IS_LABEL(label) && gtk_label_get_ellipsize(GTK_LABEL(label)) != PANGO_ELLIPSIZE_END) {
                gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            }
            // letters large; words smaller (Ctrl, Enter), and on keys one
            // unit wide, longer ones (PgUp, Home) smaller still, to fit
            const glong length = g_utf8_strlen(key->label[type], -1);
            const gboolean narrow = !key->width || key->width <= KEY_UNIT;
            style_class_set(key->button, "word", length >= 3);
            style_class_set(key->button, "long", length >= 4 && narrow);
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
 * @param keyboard Keyboard structure, may be NULL
 */
void keyboard_set_size(Keyboard *keyboard) {
    if G_UNLIKELY(keyboard == NULL || keyboard->key_count == 0 || keyboard->row_count == 0) {
        return;
    }
    GtkWidget *keyboard_box = keyboard->container;
    // the labels of the variant in use
    keyboard_set_layout(keyboard);
    // update geometry
    GdkScreen *screen = gdk_screen_get_default();
    gint screen_height = gdk_screen_get_height(screen);
    gint screen_width = gdk_screen_get_width(screen);
    GtkAllocation alloc;
    gtk_widget_get_allocation(gtk_widget_get_toplevel(keyboard_box), &alloc);
    gint window_width = alloc.width;
    gint window_height = alloc.height;
#ifdef KINDLE
    // The window always covers the screen, but its own allocation cannot
    // be used: on the device the keys were sized from an early, narrow one
    // and stayed that small. GDK's idea of the screen size can still be the
    // old one right after a rotation, so ask the X server for the size of
    // the root window, which follows the rotation at once.
    gint root_width = 0, root_height = 0;
    gdk_window_get_geometry(gdk_screen_get_root_window(screen), NULL, NULL, &root_width, &root_height);
    if (root_width > 0 && root_height > 0) {
        screen_width = root_width;
        screen_height = root_height;
    }
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
    // the gap around each key, inside its size
    GtkBorder margin = {0, 0, 0, 0};
    gtk_style_context_get_margin(style, 0, &margin);
    unit_wmin += (guint) (margin.left + margin.right);
    unit_hmin += (guint) (margin.top + margin.bottom);
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
    // Rows of their own height (height="..." in the layout): the letters,
    // tapped most, taller than the rows of numbers and of special keys.
    gint kb_height = 0;
    for (guint i = 0; i < keyboard->row_count; i++) {
        const guint height = MAX(unit_hmin, unit_h * keyboard->row_height[i] / KEY_UNIT);
        if (keyboard->row_box[i]) {
            gtk_widget_set_size_request(keyboard->row_box[i], -1, (gint) height);
        }
        kb_height += (gint) height;
        if (i == 0) {
            // the shortcut bar is as tall as the first row, see shortbar.c
            g_object_set_data(G_OBJECT(keyboard_box), KB_FIRST_ROW_HEIGHT, GUINT_TO_POINTER(height));
        }
    }
    D printf("keyboard size: %ix%i\n", kb_width, kb_height);
    gtk_widget_set_size_request(keyboard_box, -1, kb_height);
}

/**
 * Act on a key that was tapped: send it through the X keymap
 * @param key Key structure
 * @return True on success, false if its key is not in the keymap
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
    if (keyval == KTERM_KEY_IME || keyval == KTERM_KEY_HIDE) {
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
    if (keyval == KTERM_KEY_HIDE) {
        kterm_hide_keyboard();
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
 * Type a key whose key is not in the X keymap straight into the terminal
 * @param key Key structure
 */
static void keyboard_terminal_feed(const Key *key) {
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
    // as keyboard_event_press(): a variant without a key of its own types
    // the default one
    guint keyval = key->keyval[kb_type] ? key->keyval[kb_type] : key->keyval[KBT_DEFAULT];
    const gunichar character = gdk_keyval_to_unicode(keyval);
    if (!character) {
        return;
    }
    gchar utf[6];
    gint utf_len = g_unichar_to_utf8(character, utf);
    D printf("feed child: %i chars\n", utf_len);
    vte_terminal_feed_child(VTE_TERMINAL(terminal), utf, utf_len);
}

/**
 * Timer callback: draw a key tapped a moment ago released again
 * @param data Key structure
 * @return G_SOURCE_REMOVE
 */
static gboolean keyboard_unpress(gpointer data) {
    Key *key = data;
    key->unpress_source = 0;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(key->button), FALSE);
    return G_SOURCE_REMOVE;
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
            // show the touch at once; a key tapped again while still drawn
            // pressed stays so
            if (key->unpress_source) {
                g_source_remove(key->unpress_source);
                key->unpress_source = 0;
            }
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
        // a touch that went sideways across the keyboard is no tap
        const gboolean sideways = ABS(dx) >= KB_SWIPE_MIN_PX;
        if (sideways || (panel_is_open() && !panel_wants_keys())) {
            // with the panel open, a tap on the keyboard puts it away and
            // types nothing; only a form of the panel is typed into
            if (!key->modifier) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
            }
            if (!sideways) {
                panel_close();
            }
            return TRUE;
        }
        if (!keyboard_event_press(key)) {
            // not in keymap, try workaround
            keyboard_terminal_feed(key);
        }
        if (!key->modifier) {
            g_timeout_add(KB_RELEASE_DELAY_MS, keyboard_event_release, key);
            key->unpress_source = g_timeout_add(KB_PRESSED_SHOW_MS, keyboard_unpress, key);
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
        if (key->unpress_source) { g_source_remove(key->unpress_source); }
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
