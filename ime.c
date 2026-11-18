/* ime.c
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
 * Bridge to the Kindle system keyboard, which is the only input method on
 * the device (pinyin, kana, ...).
 *
 * Stock applications reach it through a GTK+ 2 input method module. That
 * module is a thin wrapper around lipc, Amazon's property bus on top of
 * D-Bus, so the same conversation can be held without it:
 *
 *   - we own a bus name and tell the keyboard to open for that name:
 *       com.lab126.keyboard  open = "<name>:<type>:<flags>"
 *   - the keyboard then "sets string properties" on us, i.e. calls
 *       <name>.set<Property>Str(s) on object /default
 *     for keyboardCommit, keyboardDelete, keyboardReplace,
 *     keyboardSetPreeditString and keyboardGetSurround.
 *
 * Keys that are not text (backspace, return) and, with prediction off,
 * plain characters come as ordinary X key events instead.
 */

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <vte/vte.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ime.h"
#include "config.h"

#define KB_SERVICE "com.lab126.keyboard"
#define LIPC_PATH "/default"
/** Keyboard type understood by the system keyboard: plain text entry */
#define KB_TYPE "abc"
/** Open flags: prediction on. Without it pinyin does not compose. */
#define KB_FLAGS 3
/** Height used when the keyboard does not tell its own */
#define KB_HEIGHT_FALLBACK 497
#define LIPC_TIMEOUT_MS 1000

static GDBusConnection *bus = NULL;
static gchar *service = NULL;
static guint filter_id = 0;
static GtkWidget *terminal = NULL;
static gboolean opened = FALSE;
static gboolean trace = FALSE;
/** Text most recently entered through the keyboard, see report_surround() */
static gchar *context = NULL;

/** One property write from the keyboard, carried to the main loop */
typedef struct {
    gchar *property;
    gchar *value;
} ImeCall;

/**
 * Write a lipc string property of another service
 * @param dest Service name
 * @param property Property name
 * @param value New value
 * @return True if the call was sent
 */
static gboolean lipc_set_string(const gchar *dest, const gchar *property, const gchar *value) {
    if (!bus) {
        return FALSE;
    }
    gchar *method = g_strdup_printf("set%sStr", property);
    gchar *iface = g_strdup_printf("%s.%s", dest, method);
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(bus, dest, LIPC_PATH, iface, method,
                                                  g_variant_new("(s)", value), NULL,
                                                  G_DBUS_CALL_FLAGS_NONE, LIPC_TIMEOUT_MS, NULL, &error);
    g_free(method);
    g_free(iface);
    if (!reply) {
        D printf("ime: set %s.%s failed: %s\n", dest, property, error->message);
        g_error_free(error);
        return FALSE;
    }
    g_variant_unref(reply);
    return TRUE;
}

/**
 * Read a lipc integer property of another service
 * @param dest Service name
 * @param property Property name
 * @param value Location for the value
 * @return True on success
 */
static gboolean lipc_get_int(const gchar *dest, const gchar *property, gint *value) {
    if (!bus) {
        return FALSE;
    }
    gchar *method = g_strdup_printf("get%sInt", property);
    gchar *iface = g_strdup_printf("%s.%s", dest, method);
    GVariant *reply = g_dbus_connection_call_sync(bus, dest, LIPC_PATH, iface, method, NULL,
                                                  G_VARIANT_TYPE("(ui)"),
                                                  G_DBUS_CALL_FLAGS_NONE, LIPC_TIMEOUT_MS, NULL, NULL);
    g_free(method);
    g_free(iface);
    if (!reply) {
        return FALSE;
    }
    guint32 status = 0;
    g_variant_get(reply, "(ui)", &status, value);
    g_variant_unref(reply);
    return status == 0;
}

/**
 * Remember what was just entered
 * @param text Entered text
 */
static void set_context(const gchar *text) {
    g_free(context);
    context = g_strdup(text);
}

/**
 * Answer the keyboard's question about the text before the cursor.
 * It uses the answer to capitalise the first letter of a sentence and to
 * suggest a next word. A terminal cannot read back what is on the command
 * line, so the answer is the last thing the keyboard itself entered - or a
 * lone digit: an empty answer means "start of a sentence" and turns shift
 * on, which is never wanted in a shell, and letters are taken by the pinyin
 * engine as the beginning of the syllables being typed.
 */
static void report_surround(void) {
    const gchar *text = "1";
    if (context && *context && !g_ascii_isalpha(context[strlen(context) - 1])) {
        text = context;
    }
    // "<cursor position in characters>:<text>"
    gchar *value = g_strdup_printf("%ld:%s", g_utf8_strlen(text, -1), text);
    gchar *iface = g_strdup_printf("%s.setsetSurroundStr", KB_SERVICE);
    // No waiting for the reply: the keyboard may be waiting for us.
    g_dbus_connection_call(bus, KB_SERVICE, LIPC_PATH, iface, "setsetSurroundStr",
                           g_variant_new("(s)", value), NULL,
                           G_DBUS_CALL_FLAGS_NONE, LIPC_TIMEOUT_MS, NULL, NULL, NULL);
    g_free(iface);
    g_free(value);
}

/**
 * Send bytes to the program running in the terminal
 * @param text Bytes to send
 * @param count How many times
 */
static void feed(const gchar *text, gint count) {
    if (!terminal) {
        return;
    }
    for (gint i = 0; i < count; i++) {
        vte_terminal_feed_child(VTE_TERMINAL(terminal), text, -1);
    }
}

/**
 * Remove text around the cursor the way a text field would: there is no
 * text field in a terminal, so ask the running program with the keys a
 * user would press.
 * @param before Characters to remove before the cursor
 * @param after Characters to remove after the cursor
 */
static void delete_surrounding(gint before, gint after) {
    feed("\x7f", CLAMP(before, 0, 256));
    feed("\x1b[3~", CLAMP(after, 0, 256));
}

/**
 * Handle one property write from the keyboard. Runs in the main loop.
 * @param data ImeCall, freed here
 * @return Always false, one shot
 */
static gboolean ime_dispatch(gpointer data) {
    ImeCall *call = data;
    const gchar *property = call->property;
    const gchar *value = call->value;
    if (trace) {
        fprintf(stderr, "ime: %s = |%s|\n", property, value);
    }
    if (!strcmp(property, "keyboardCommit")) {
        feed(value, 1);
        set_context(value);
    } else if (!strcmp(property, "keyboardDelete")) {
        // "<before>:<after>"
        gint before = 0, after = 0;
        if (sscanf(value, "%d:%d", &before, &after) >= 1) {
            delete_surrounding(before, after);
        }
    } else if (!strcmp(property, "keyboardReplace")) {
        // "<before>:<after>:<text>"
        gint before = 0, after = 0, offset = 0;
        if (sscanf(value, "%d:%d:%n", &before, &after, &offset) >= 2 && offset > 0) {
            delete_surrounding(before, after);
            feed(value + offset, 1);
            set_context(value + offset);
        }
    } else if (!strcmp(property, "keyboardGetSurround")) {
        report_surround();
    }
    // keyboardSetPreeditString: a terminal has no composition area, and the
    // keyboard shows what is being composed in its own candidate bar.
    g_free(call->property);
    g_free(call->value);
    g_free(call);
    return G_SOURCE_REMOVE;
}

/**
 * D-Bus message filter: pick out the keyboard's property writes, answer
 * them and pass them on to the main loop. Runs in the GDBus worker thread.
 */
static GDBusMessage * ime_filter(GDBusConnection *connection, GDBusMessage *message, gboolean incoming, gpointer data) {
    UNUSED(data);
    if (!incoming || g_dbus_message_get_message_type(message) != G_DBUS_MESSAGE_TYPE_METHOD_CALL) {
        return message;
    }
    // member is "set<Property>Str"
    const gchar *member = g_dbus_message_get_member(message);
    const gchar *path = g_dbus_message_get_path(message);
    if (!member || !path || strcmp(path, LIPC_PATH) || !g_str_has_prefix(member, "setkeyboard") || !g_str_has_suffix(member, "Str")) {
        return message;
    }
    const gchar *value = "";
    GVariant *body = g_dbus_message_get_body(message);
    GVariant *first = NULL;
    if (body && g_variant_n_children(body) > 0) {
        first = g_variant_get_child_value(body, 0);
        if (g_variant_is_of_type(first, G_VARIANT_TYPE_STRING)) {
            value = g_variant_get_string(first, NULL);
        }
    }
    if (trace) {
        gchar *dump = body ? g_variant_print(body, TRUE) : g_strdup("(no body)");
        fprintf(stderr, "ime: call %s %s\n", member, dump);
        g_free(dump);
    }
    ImeCall *call = g_new0(ImeCall, 1);
    call->property = g_strndup(member + 3, strlen(member) - 6);
    call->value = g_strdup(value);
    if (first) {
        g_variant_unref(first);
    }
    // Default priority, like X events: text and key presses keep their order.
    g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, ime_dispatch, call, NULL);

    // lipc expects a status code back, 0 is success
    GDBusMessage *reply = g_dbus_message_new_method_reply(message);
    g_dbus_message_set_body(reply, g_variant_new("(u)", 0));
    g_dbus_connection_send_message(connection, reply, G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL);
    g_object_unref(reply);
    g_object_unref(message);
    return NULL;
}

/**
 * Connect to the system bus and get ready to receive text
 * @param term Terminal widget that receives the text
 * @return True if the system keyboard can be used
 */
static gboolean ime_fake(void);

gboolean ime_init(GtkWidget *term) {
    terminal = term;
    trace = (g_getenv("KTERM_IME_TRACE") != NULL);
    if (ime_fake()) {
        return TRUE;
    }
    GError *error = NULL;
    bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!bus) {
        D printf("ime: no system bus: %s\n", error->message);
        g_error_free(error);
        return FALSE;
    }
    // GDBus would otherwise end the process when the bus goes away
    g_dbus_connection_set_exit_on_close(bus, FALSE);
    service = g_strdup_printf("net.fabiszewski.kterm.ime%d", (gint) getpid());
    GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "RequestName",
                                                  g_variant_new("(su)", service, 0), G_VARIANT_TYPE("(u)"),
                                                  G_DBUS_CALL_FLAGS_NONE, LIPC_TIMEOUT_MS, NULL, &error);
    if (!reply) {
        D printf("ime: cannot own %s: %s\n", service, error->message);
        g_error_free(error);
        ime_shutdown();
        return FALSE;
    }
    g_variant_unref(reply);
    filter_id = g_dbus_connection_add_filter(bus, ime_filter, NULL, NULL);
    D printf("ime: ready as %s\n", service);
    return TRUE;
}

/**
 * Close the keyboard and drop the bus connection
 */
void ime_shutdown(void) {
    ime_close();
    if (bus) {
        if (filter_id) {
            g_dbus_connection_remove_filter(bus, filter_id);
            filter_id = 0;
        }
        g_object_unref(bus);
        bus = NULL;
    }
    g_free(service);
    service = NULL;
    set_context(NULL);
}

/**
 * @return True if ime_init() succeeded
 */
/**
 * @return True if KTERM_IME_FAKE is set: the system keyboard is only
 *         pretended to open. For testing the layout around it on a machine
 *         that has no such keyboard.
 */
static gboolean ime_fake(void) {
    return g_getenv("KTERM_IME_FAKE") != NULL;
}

gboolean ime_available(void) {
    return ime_fake() || (bus != NULL && service != NULL);
}

/**
 * @return True while the system keyboard is shown for us
 */
gboolean ime_is_open(void) {
    return opened;
}

/**
 * Show the system keyboard
 * @return True on success
 */
gboolean ime_open(void) {
    if (!ime_available() || opened) {
        return opened;
    }
    if (ime_fake()) {
        opened = TRUE;
        return TRUE;
    }
    gchar *request = g_strdup_printf("%s:%s:%d", service, KB_TYPE, KB_FLAGS);
    opened = lipc_set_string(KB_SERVICE, "open", request);
    g_free(request);
    return opened;
}

/**
 * Hide the system keyboard
 */
void ime_close(void) {
    if (!opened) {
        return;
    }
    if (ime_fake()) {
        opened = FALSE;
        return;
    }
    lipc_set_string(KB_SERVICE, "close", service);
    opened = FALSE;
}

/**
 * @return Height of the system keyboard window in pixels
 */
gint ime_keyboard_height(void) {
    gint height = 0;
    if (!ime_fake() && lipc_get_int(KB_SERVICE, "height", &height) && height > 0) {
        return height;
    }
    return KB_HEIGHT_FALLBACK;
}

/**
 * Key event hook for the main window, used to see what the system keyboard
 * sends as X events.
 * @return False to let the event through
 */
gboolean ime_key_event(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    UNUSED(widget);
    UNUSED(data);
    if (trace && opened) {
        fprintf(stderr, "ime: x key %s keyval=0x%x send_event=%d\n",
                event->type == GDK_KEY_PRESS ? "press" : "release", event->keyval, event->send_event);
    }
    return FALSE;
}
