/* kindle.c
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

#include <stdio.h>
#include <gio/gio.h>
#include "kindle.h"
#include "config.h"

/** Global config */
extern KTconf *conf;

#ifdef HAVE_DBUS

#include <dbus/dbus.h>

/**
 * Send dbus message, get string response if requested
 * @param destination Name that the message should be sent to
 * @param method Method to invoke
 * @param param Optional param do append to message
 * @param reply_msg Location to store reply message or NULL to ignore response
 * @return True on success, false otherwise
 */
static gboolean dbus_send(const gchar *dest, const gchar *method, const gchar *param, gchar **reply_msg) {
    gboolean ret = TRUE;
    DBusError error;
    DBusBusType type = DBUS_BUS_SYSTEM;
    gchar iface[256];
    snprintf(iface, sizeof(iface), "%s.%s", dest, method);
    const gchar *path = "/default";
    
    dbus_error_init(&error);
    DBusConnection *connection = dbus_bus_get(type, &error);
    if G_UNLIKELY(!connection) {
        D printf("Dbus connection failed: %s\n", error.message);
        dbus_error_free(&error);
        return FALSE;
    }
    DBusMessage *message = dbus_message_new_method_call(dest, path, iface, method);
    if G_UNLIKELY(!message) {
        D printf("Dbus message allocation failed\n");
        dbus_connection_unref(connection);
        return FALSE;
    }
    if (param && !dbus_message_append_args(message, DBUS_TYPE_STRING, &param, DBUS_TYPE_INVALID)) {
        D printf("Dbus append args failed\n");
        dbus_message_unref(message);
        dbus_connection_unref(connection);
        return FALSE;
    }
    
    if (reply_msg) {
        dbus_error_init(&error);
        gint timeout_ms = 500;
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(connection, message, timeout_ms, &error);
        if G_UNLIKELY(!reply) {
            D printf("Dbus reply error: %s: %s\n", error.name, error.message);
            dbus_error_free(&error);
            ret = FALSE;
        } else {
            DBusMessageIter args;
            dbus_message_iter_init(reply, &args);
            do {
                if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
                    dbus_message_iter_get_basic(&args, reply_msg);
                    break;
                }
            } while (dbus_message_iter_next(&args));
            dbus_message_unref(reply);
        }
    } else {
        dbus_connection_send(connection, message, NULL);
        dbus_connection_flush(connection);
    }
    dbus_message_unref(message);
    dbus_connection_unref(connection);
    return ret;
}

/**
 * Get screen orientation
 * @return 'U', 'L', 'R' or zero on failure
 */
gchar get_orientation(void) {
    const gchar *dest = "com.lab126.winmgr";
    const gchar *reply_method = "getorientationLockStr";
    gchar *reply = NULL;
    gboolean ret = dbus_send(dest, reply_method, NULL, &reply);
    if G_LIKELY(ret && reply) {
        D printf("screen orientation: %s\n", reply);
        gchar orientation = reply[0];
        if (orientation == 'U' || orientation == 'L' || orientation == 'R') {
            return orientation;
        }
    }
    return 0;
}

/**
 * Set screen orientation
 * @param orientation One of 'U', 'L', 'R'
 * @return True on success, false otherwise
 */
gboolean set_orientation(gchar orientation) {
    if (orientation != 'U' && orientation != 'L' && orientation != 'R') {
        return FALSE;
    }
    gchar param[2] = { 0 };
    param[0] = orientation;
    const gchar *dest = "com.lab126.winmgr";
    const gchar *query_method = "setorientationLockStr";
    gboolean ret = dbus_send(dest, query_method, param, NULL);
    if G_LIKELY(ret) {
        D printf("screen orientation set to %c\n", orientation);
    }
    return ret;
}

#else

gchar get_orientation(void) {
    D printf("dbus support not compiled\n");
    return 0;
}

gboolean set_orientation(gchar orientation) {
    UNUSED(orientation);
    D printf("dbus support not compiled\n");
    return FALSE;
}

#endif

/**
 * Initialize orientation settings, set orientation if needed
 */
void orientation_init(void) {
    // save current screen orientation
    conf->orientation_saved = get_orientation();
    // set orientation from config if any
    if (conf->orientation > 0) {
        if (conf->orientation != conf->orientation_saved) {
            set_orientation(conf->orientation);
        }
    } else {
        conf->orientation = conf->orientation_saved;
    }
}

/**
 * Restore initial screen orientation
 */
void orientation_restore(void) {
    if (conf->orientation_saved > 0 && conf->orientation_saved != conf->orientation) {
        set_orientation(conf->orientation_saved);
    }
}

/**
 * Grab/ungrab keyboard
 * @param window Gdk window
 * @param grab Grab if true, ungrab if false
 * @return True on success, false otherwise
 */
gboolean keyboard_grab(GdkWindow *window, gboolean grab) {
    GdkGrabStatus ret = GDK_GRAB_SUCCESS;
    
#if GTK_CHECK_VERSION(3,20,0)
    GdkDisplay *display = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(display);
    if (!seat) {
        D printf("Getting default seat failed\n");
        return FALSE;
    }
    if (grab) {
        ret = gdk_seat_grab(seat, window, GDK_SEAT_CAPABILITY_KEYBOARD, TRUE, NULL, NULL, NULL, NULL);
    } else {
        gdk_seat_ungrab(seat);
    }
    
#elif GTK_CHECK_VERSION(3,0,0)
    GdkDisplay *display = gdk_display_get_default();
    GdkDeviceManager *manager = gdk_display_get_device_manager(display);
    if (!manager) {
        D printf("Getting display manager failed\n");
        return FALSE;
    }
    GdkDevice *pointer = gdk_device_manager_get_client_pointer(manager);
    if (!pointer) {
        D printf("Getting pointer failed\n");
        return FALSE;
    }
    GdkDevice *keyboard = gdk_device_get_associated_device(pointer);
    if (!keyboard || gdk_device_get_source(keyboard) != GDK_SOURCE_KEYBOARD) {
        D printf("Getting keyboard failed\n");
        return FALSE;
    }
    if (grab) {
        ret = gdk_device_grab(keyboard, window, GDK_OWNERSHIP_NONE, TRUE, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK, NULL, GDK_CURRENT_TIME);
    } else {
        gdk_device_ungrab(keyboard, GDK_CURRENT_TIME);
    }
    
#else
    if (grab) {
        ret = gdk_keyboard_grab(window, FALSE, GDK_CURRENT_TIME);
    } else {
        gdk_keyboard_ungrab(GDK_CURRENT_TIME);
    }
#endif
    if (ret != GDK_GRAB_SUCCESS) {
        D printf("Keyboard grabbing failed (%d)\n", ret);
        return FALSE;
    }
    return TRUE;
}

/**
 * Grab keyboard callback
 * @param widget Calling widget
 * @param event Gdk event
 * @return Always false to propagate event
 */
gboolean grab_keyboard_cb(GtkWidget *widget, GdkEvent *event, gpointer data) {
    UNUSED(event);
    UNUSED(data);
    keyboard_grab(gtk_widget_get_window(widget), TRUE);
    return FALSE;
}

/**
 * Open the device's quick settings, as a swipe down from the top of the
 * screen does in its own applications.
 * This is the lipc call "com.lab126.kppchromebar configurePillState
 * openedBySwipe"; lipc properties are D-Bus methods on the system bus.
 * @return True if the request was sent
 */
gboolean show_system_menu(void) {
    static GDBusConnection *bus = NULL;
    if (!bus) {
        GError *error = NULL;
        bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (!bus) {
            D printf("No system bus: %s\n", error->message);
            g_error_free(error);
            return FALSE;
        }
        // losing the bus must not take the terminal down with it
        g_dbus_connection_set_exit_on_close(bus, FALSE);
    }
    const gchar *service = "com.lab126.kppchromebar";
    const gchar *method = "setconfigurePillStateStr";
    gchar *iface = g_strdup_printf("%s.%s", service, method);
    // not waiting for the answer: the status bar is busy drawing by then
    g_dbus_connection_call(bus, service, "/default", iface, method, g_variant_new("(s)", "openedBySwipe"),
                           NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_free(iface);
    return TRUE;
}

/**
 * Inject custom styles (css)
 * Reason: Kindle users have limited access to style customizations.
 * This scheme may however be overriden by local styles.
 */
void inject_styles(void) {
#if GTK_CHECK_VERSION(3,0,0)
    // No animations: on e-ink each frame of one (a key fading back from
    // black, say) is one more refresh of the screen, in grays nobody asked
    // for.
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    GError *error = NULL;
    GtkCssProvider *provider = gtk_css_provider_new();
    if (!provider) { return; }
    gtk_css_provider_load_from_data(provider,
                                    // The window has 8 levels of gray (see
                                    // kterm.c): #d8d8d8 shows as the light
                                    // gray of the keys, #b0b0b0 as their
                                    // borders; lighter values come out white.
                                    "@define-color gray #d8d8d8;"
                                    "@define-color line #b0b0b0;"
                                    // Theme padding and minimum width would make
                                    // multi-letter keys (Ctrl, Shift) wider than
                                    // their slot and push the row off screen.
                                    // Letters as large as on the Kindle's own keyboard, words
                                    // (Ctrl, Enter) smaller to fit; a gap between keys, so a
                                    // finger between two can tell which one it is on.
                                    "#ktermKbButton { background: @gray; padding: 0; min-width: 0; min-height: 0; border-color: @line;"
                                    " margin: 3px 2px; font-family: \"Amazon Ember\", sans-serif; font-size: 11pt; }"
                                    "#ktermKbButton.word { font-size: 9pt; }"
                                    "#ktermKbButton.long { font-size: 8pt; }"
                                    "#ktermKbButton:hover { background: @gray; }"
                                    "#ktermKbButton:selected { background: @gray; }"
                                    "#ktermKbButton:disabled { background: @gray; }"
                                    "#ktermKbButton:active { background: @gray; }"
                                    // Shift, Ctrl, Alt, the symbol key: black while
                                    // on, until the next key uses them
                                    // a key held down, or a modifier latched: black, as the
                                    // Kindle's own keyboard draws a key pressed
                                    "#ktermKbButton:checked { background: #000000; color: #ffffff; }"
                                    "#ktermKbButton:checked label { color: #ffffff; }"
                                    // shortcut bar: keys like the keyboard's,
                                    // set apart from it by a line
                                    "#ktermShortBar { background: #ffffff; border-bottom: 2px solid #000000; }"
                                    "#ktermShortButton { background: @gray; padding: 0; min-width: 0; min-height: 0; margin: 3px;"
                                    " border: 1px solid @line; border-radius: 4px; color: #2e3436;"
                                    " font-family: \"Amazon Ember\", sans-serif; font-size: 9pt; }"
                                    // the paste button, there for a while: black, set apart from the shortcuts
                                    "#ktermShortPaste { background: #000000; color: #ffffff; padding: 0; min-width: 0; min-height: 0; margin: 3px;"
                                    " border: 1px solid #000000; border-radius: 4px;"
                                    " font-family: \"Amazon Ember\", sans-serif; font-size: 9pt; }"
                                    // the two tools at the ends: keys like the shortcuts, a sign drawn on them
                                    "#ktermShortTool { background: @gray; color: #000000; margin: 3px;"
                                    " border: 1px solid @line; border-radius: 4px; }"
                                    "#ktermShortTool.pressed { background: #000000; color: #ffffff; }"
                                    "#ktermShortButton.pressed { background: #000000; color: #ffffff; border-color: #000000; }"
                                    "#ktermShortPaste.pressed { background: #ffffff; color: #000000; }"
                                    "#ktermShortPages { color: #000000; font-family: \"Amazon Ember\", sans-serif; font-size: 8pt; }"
                                    "#ktermShortProblem { color: #000000; padding: 4px 8px;"
                                    " font-family: \"Amazon Ember\", sans-serif; font-size: 7pt; }"
                                    // the panel over the terminal: black on
                                    // white, large enough for fingers
                                    "#ktermPanel { background-color: #ffffff; border-bottom: 4px solid #000000;"
                                    " color: #000000; font-family: \"Amazon Ember\", sans-serif; font-size: 9pt; }"
                                    "#ktermPanelButton { background: @gray; color: #000000; border: 2px solid #000000;"
                                    " border-radius: 8px; padding: 16px 20px; min-width: 0; min-height: 0;"
                                    " box-shadow: none; text-shadow: none; }"
                                    "#ktermPanelButton:disabled { color: #a0a0a0; border-color: #a0a0a0; }"
                                    // the choice in effect, as a pressed modifier key
                                    "#ktermPanelButton.selected { background: #000000; color: #ffffff; }"
                                    "#ktermPanelButton.selected label { color: #ffffff; }"
                                    // the back arrow: no frame, large, like the Kindle's
                                    "#ktermPanelBack { background: transparent; color: #000000; border: none;"
                                    " box-shadow: none; text-shadow: none; padding: 4px 24px 4px 0;"
                                    " min-width: 0; min-height: 0; font-size: 16pt; }"
                                    "#ktermPanelActions #ktermPanelButton { padding: 16px 2px; }"
                                    "#ktermPanel entry { background: #ffffff; color: #000000; border: 2px solid #000000;"
                                    " border-radius: 8px; padding: 12px 16px; min-height: 0; box-shadow: none; }"
                                    "#ktermPanel separator { background: #000000; min-height: 2px; }"
                                    // settings: thin gray lines between the rows, buttons of one width
                                    "#ktermPanel #ktermSettingLine { background: @line; min-height: 1px; }"
                                    "#ktermSettings #ktermPanelButton { min-width: 80px; }"
                                    , -1, &error);
    if G_UNLIKELY(error) {
        D printf("Style injection failed: %s\n", error->message);
        g_error_free(error);
    } else {
        GdkScreen *screen = gdk_screen_get_default();
        gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
#endif
}

/**
 * Inject custom styles (gtkrc)
 * Reason: Kindle users have limited access to style customizations.
 * This scheme may however be overriden by local styles.
 */
void inject_gtkrc(void) {
#if !GTK_CHECK_VERSION(3,0,0)
    gtk_rc_parse_string("gtk_color_scheme = \"white: #ffffff\ngray: #f0f0f0\""
                        "style \"kterm-style\" {"
                        " bg[NORMAL] = @gray"
                        " bg[PRELIGHT] = @gray"
                        " bg[INSENSITIVE] = @gray"
                        " bg[ACTIVE] = @gray"
                        " bg[SELECTED] = @gray"
                        "}"
                        "widget \"*ktermKbButton\" style : lowest \"kterm-style\"");
#endif
}
