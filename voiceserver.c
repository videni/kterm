/* voiceserver.c
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
 * The web server of typing from a phone (voice.c), on the local network:
 *
 *   GET  /favicon.png, /apple-touch-icon.png
 *                        the icon, without the key: it tells nothing
 *   GET  /KEY            the page (kindle/web/, built into voice-page.h)
 *   GET  /KEY/ping       "ok": the page shows it is connected
 *   POST /KEY/send       the body (UTF-8 text) for the terminal; ?run=1:
 *                        Enter after it. Answers what was done with it.
 *
 * Without the key (voiceserver_set_token()) nothing but the icon is
 * served: another device in the network cannot type into the terminal.
 *
 * Connections are served in worker threads, one at a time each. What a
 * request asks of the terminal is handed to the main thread (VoiceHandler)
 * and the worker waits for the answer, to send it back to the phone.
 */

#include <string.h>
#include <cairo.h>
#include "config.h"
#include "voiceserver.h"
// VOICE_PAGE, built from kindle/web/ by kindle/build-web.sh
#include "voice-page.h"

/** Longest request head and text taken, bytes */
#define HEAD_MAX 8192
#define BODY_MAX 65536
/** Connections served at the same time */
#define WORKERS 4
/** A connection silent for this long is dropped, s */
#define SOCKET_TIMEOUT 15
/** The main thread is waited for this long to tell what it did, us */
#define REPLY_TIMEOUT_US (10 * G_USEC_PER_SEC)

/** The server, NULL while it does not listen */
static GSocketService *service = NULL;
/** Told of requests, in the main thread */
static VoiceHandler handler = NULL;
/** The key in the page's address; read by the workers */
static gchar *token = NULL;
G_LOCK_DEFINE_STATIC(token);
/** The page's icon as PNG, small for the tab and large for a home screen;
 *  made once, only read after that */
static GBytes *favicon = NULL;
static GBytes *touch_icon = NULL;

/**
 * Set the key the page's address must hold: addresses with another one
 * stop working
 * @param key Key, copied
 */
void voiceserver_set_token(const gchar *key) {
    G_LOCK(token);
    g_free(token);
    token = g_strdup(key);
    G_UNLOCK(token);
}

/**
 * In the main thread: what became of the text, for the worker waiting
 * to tell the phone. Once for each request with text.
 * @param status Answer: "staged", "ran", "staged-run" (see voicebar.c)
 * @param reply The request's reply
 */
void voiceserver_answer(const gchar *status, gpointer reply) {
    g_async_queue_push(reply, g_strdup(status));
    g_async_queue_unref(reply);
}

/**
 * In the main thread: hand a request to the handler
 */
static gboolean dispatch_cb(gpointer data) {
    VoiceRequest *request = data;
    handler(request);
    g_free(request->text);
    g_free(request->phone);
    g_free(request);
    return G_SOURCE_REMOVE;
}

/**
 * @return Address of the other end of the connection, newly allocated
 */
static gchar * remote_name(GSocketConnection *connection) {
    GSocketAddress *remote = g_socket_connection_get_remote_address(connection, NULL);
    gchar *name = G_IS_INET_SOCKET_ADDRESS(remote)
                  ? g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(remote)))
                  : g_strdup("?");
    if (remote) {
        g_object_unref(remote);
    }
    return name;
}

/**
 * Write a whole response
 */
static void respond(GOutputStream *out, const gchar *status, const gchar *type, const gchar *body, gsize size) {
    gchar *head = g_strdup_printf("HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %" G_GSIZE_FORMAT "\r\n"
                                  "Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, size);
    g_output_stream_write_all(out, head, strlen(head), NULL, NULL, NULL);
    g_output_stream_write_all(out, body, size, NULL, NULL, NULL);
    g_free(head);
}

static void respond_text(GOutputStream *out, const gchar *status, const gchar *text) {
    respond(out, status, "text/plain; charset=utf-8", text, strlen(text));
}

/**
 * @param head Request head, lower case
 * @return Its Content-Length, 0 if none
 */
static gsize content_length(const gchar *head) {
    const gchar *field = strstr(head, "\r\ncontent-length:");
    return field ? (gsize) g_ascii_strtoull(field + 17, NULL, 10) : 0;
}

/**
 * Read a request: its head, then as much body as it says
 * @param connection Connection
 * @param out Where an error is answered
 * @param body Set to where the body starts in the request
 * @param length Set to the body's length
 * @return The request, NUL ended, newly allocated; NULL if it was not
 *         read whole (answered already, if there was something to say)
 */
static gchar * read_request(GSocketConnection *connection, GOutputStream *out, const gchar **body, gsize *length) {
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    gchar *buffer = g_malloc(HEAD_MAX + BODY_MAX + 1);
    gsize got = 0;
    gchar *end = NULL;
    while (!end && got < HEAD_MAX) {
        const gssize n = g_input_stream_read(in, buffer + got, HEAD_MAX - got, NULL, NULL);
        if (n <= 0) {
            g_free(buffer);
            return NULL;
        }
        got += (gsize) n;
        buffer[got] = '\0';
        end = strstr(buffer, "\r\n\r\n");
    }
    if (!end) {
        respond_text(out, "431 Request Header Fields Too Large", "");
        g_free(buffer);
        return NULL;
    }
    const gsize head_size = (gsize) (end - buffer) + 4;
    gchar *head = g_ascii_strdown(buffer, (gssize) head_size);
    *length = content_length(head);
    g_free(head);
    if (*length > BODY_MAX) {
        respond_text(out, "413 Payload Too Large", "太长了");
        g_free(buffer);
        return NULL;
    }
    while (got < head_size + *length) {
        const gssize n = g_input_stream_read(in, buffer + got, head_size + *length - got, NULL, NULL);
        if (n <= 0) {
            g_free(buffer);
            return NULL;
        }
        got += (gsize) n;
    }
    buffer[head_size + *length] = '\0';
    *body = buffer + head_size;
    return buffer;
}

/**
 * @param path Path of a request
 * @return What follows the key in it ("", "/", "/send"...), NULL if it
 *         does not start with the key
 */
static const gchar * after_token(const gchar *path) {
    G_LOCK(token);
    const gsize size = token ? strlen(token) : 0;
    const gboolean ok = size && path[0] == '/' && !strncmp(path + 1, token, size);
    G_UNLOCK(token);
    const gchar *rest = ok ? path + 1 + size : NULL;
    return rest && (!*rest || *rest == '/') ? rest : NULL;
}

/**
 * In a worker thread: serve one connection
 */
static gboolean run_cb(GThreadedSocketService *server, GSocketConnection *connection,
                       GObject *source, gpointer data) {
    UNUSED(server);
    UNUSED(source);
    UNUSED(data);
    g_socket_set_timeout(g_socket_connection_get_socket(connection), SOCKET_TIMEOUT);
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    const gchar *body = NULL;
    gsize length = 0;
    gchar *request = read_request(connection, out, &body, &length);
    if (!request) {
        return TRUE;
    }
    // "POST /KEY/send?run=1 HTTP/1.1"
    gchar **words = g_strsplit(request, " ", 3);
    const gchar *method = words[0] ? words[0] : "";
    const gchar *target = words[0] && words[1] ? words[1] : "";
    gchar *path = g_strndup(target, strcspn(target, "?"));
    const gchar *query = strchr(target, '?');
    const gchar *rest = after_token(path);
    const gboolean get = !strcmp(method, "GET");
    if (get && (!strcmp(path, "/favicon.png") || !strcmp(path, "/favicon.ico")
                || g_str_has_prefix(path, "/apple-touch-icon"))) {
        GBytes *icon = g_str_has_prefix(path, "/apple") ? touch_icon : favicon;
        gsize size = 0;
        const gchar *png = g_bytes_get_data(icon, &size);
        respond(out, "200 OK", "image/png", png, size);
    } else if (!rest) {
        respond_text(out, "403 Forbidden", "口令不对：请重新扫 Kindle 上的二维码");
    } else if (get && (!*rest || !strcmp(rest, "/"))) {
        respond(out, "200 OK", "text/html; charset=utf-8", VOICE_PAGE, sizeof(VOICE_PAGE) - 1);
    } else if (get && !strcmp(rest, "/ping")) {
        respond_text(out, "200 OK", "ok");
        VoiceRequest *ping = g_new0(VoiceRequest, 1);
        ping->phone = remote_name(connection);
        g_idle_add(dispatch_cb, ping);
    } else if (!strcmp(method, "POST") && !strcmp(rest, "/send")) {
        if (!g_utf8_validate(body, (gssize) length, NULL)) {
            respond_text(out, "400 Bad Request", "不是 UTF-8 文字");
        } else {
            VoiceRequest *send = g_new0(VoiceRequest, 1);
            send->text = g_strndup(body, length);
            send->run = query && !strcmp(query, "?run=1");
            send->phone = remote_name(connection);
            // one reference for the main thread's answer, one for waiting
            GAsyncQueue *reply = g_async_queue_new_full(g_free);
            send->reply = g_async_queue_ref(reply);
            g_idle_add(dispatch_cb, send);
            gchar *status = g_async_queue_timeout_pop(reply, REPLY_TIMEOUT_US);
            respond_text(out, "200 OK", status ? status : "timeout");
            g_free(status);
            g_async_queue_unref(reply);
        }
    } else {
        respond_text(out, "404 Not Found", "没有这个");
    }
    g_free(path);
    g_strfreev(words);
    g_free(request);
    return TRUE;
}

static cairo_status_t png_write(void *closure, const unsigned char *data, unsigned int length) {
    g_byte_array_append(closure, data, length);
    return CAIRO_STATUS_SUCCESS;
}

/**
 * Draw the icon: the prompt of the library cover (kindle/kpm/icon.svg),
 * white on black
 * @param side Width and height, px
 * @return PNG
 */
static GBytes * draw_icon(int side) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, side, side);
    cairo_t *cr = cairo_create(surface);
    // drawn on a 100 x 100 square; a home screen rounds the corners itself
    cairo_scale(cr, side / 100.0, side / 100.0);
    cairo_set_source_rgb(cr, 0x11 / 255.0, 0x11 / 255.0, 0x11 / 255.0);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 9);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    // >
    cairo_move_to(cr, 22, 28);
    cairo_line_to(cr, 46, 46);
    cairo_line_to(cr, 22, 64);
    cairo_stroke(cr);
    // _
    cairo_move_to(cr, 56, 68);
    cairo_line_to(cr, 80, 68);
    cairo_stroke(cr);
    cairo_destroy(cr);
    GByteArray *png = g_byte_array_new();
    cairo_surface_write_to_png_stream(surface, png_write, png);
    cairo_surface_destroy(surface);
    return g_byte_array_free_to_bytes(png);
}

/**
 * Start listening, if not listening yet
 * @param port Port
 * @param on_request Told of requests, in the main thread
 * @param error Set if the port could not be had
 * @return TRUE if listening
 */
gboolean voiceserver_start(guint16 port, VoiceHandler on_request, GError **error) {
    if (service) {
        return TRUE;
    }
    if (!favicon) {
        favicon = draw_icon(64);
        touch_icon = draw_icon(180);
    }
    handler = on_request;
    service = g_threaded_socket_service_new(WORKERS);
    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(service), port, NULL, error)) {
        g_object_unref(service);
        service = NULL;
        return FALSE;
    }
    g_signal_connect(service, "run", G_CALLBACK(run_cb), NULL);
    g_socket_service_start(service);
    return TRUE;
}

/**
 * Stop listening: the port is closed again
 */
void voiceserver_stop(void) {
    if (!service) {
        return;
    }
    g_socket_service_stop(service);
    g_socket_listener_close(G_SOCKET_LISTENER(service));
    g_object_unref(service);
    service = NULL;
}

/**
 * @return TRUE while listening
 */
gboolean voiceserver_running(void) {
    return service != NULL;
}
