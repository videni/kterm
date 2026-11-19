/* voice.c
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
 * Typing from a phone: kterm serves a small web page in the local network,
 * a text box and a few buttons. Whatever the phone's keyboard puts in the
 * box (its voice input, typing, corrections) is sent to the terminal, as
 * if typed here.
 *
 * The "语音" key shows the page's address as a QR code to scan; the page
 * can then be kept as a bookmark or on the home screen. The address holds
 * a key, kept in kterm.conf (voice_token): without it nothing is served,
 * so another device in the network cannot type into the terminal. A new
 * key, from the panel, makes the old addresses useless.
 *
 * Off unless switched on in the settings (conf->voice_on). On, the server
 * only waits for connections, which costs nothing while there are none. launch.sh lets them through the Kindle's firewall on Wi-Fi.
 *
 *   GET  /favicon.png, /apple-touch-icon.png
 *                        the icon, without the key: it tells nothing
 *   GET  /KEY            the page
 *   GET  /KEY/ping       "ok": the page shows it is connected
 *   POST /KEY/send       the body (UTF-8 text) to the box over the
 *                        shortcut bar; ?run=1: to the terminal with
 *                        Enter after it, unless it is scrolled back (see
 *                        voicebar.c). Answers what was done.
 */

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <vte/vte.h>
#include "config.h"
#include "confedit.h"
#include "panel.h"
#include "qrcodegen.h"
#include "voice.h"
#include "voicebar.h"
// VOICE_PAGE, built from kindle/web/ by kindle/build-web.sh
#include "voice-page.h"

extern KTconf *conf;

/** Port the page is served on */
#define VOICE_PORT 7788
/** Characters of the key */
#define TOKEN_LENGTH 10
/** Longest request head and text taken, bytes */
#define HEAD_MAX 8192
#define BODY_MAX 65536
/** Connections served at the same time */
#define WORKERS 4
/** A connection silent for this long is dropped, s */
#define SOCKET_TIMEOUT 15
/** The main thread is waited for this long to tell what it did, us */
#define REPLY_TIMEOUT_US (10 * G_USEC_PER_SEC)
/** Size of a QR code module on the screen, px */
#define QR_MODULE 7
/** Empty modules around the code, as the standard asks */
#define QR_BORDER 4

static GtkWidget *voice_page = NULL;
static GtkWidget *qr_image = NULL;
static GtkWidget *address_label = NULL;
static GtkWidget *status_label = NULL;
/** The server, NULL while switched off */
static GSocketService *service = NULL;
/** The server listens */
static gboolean serving = FALSE;
/** Address of the phone heard from last, NULL before any */
static gchar *phone = NULL;
/** The page's icon as PNG, small for the tab and large for a home screen;
 *  made once at the start, only read after that */
static GBytes *favicon = NULL;
static GBytes *touch_icon = NULL;
/** conf->voice_token is read by the workers and changed by the panel */
G_LOCK_DEFINE_STATIC(token);


/**
 * @return The Kindle's address in the local network, newly allocated;
 *         Wi-Fi's first. NULL if there is none.
 */
static gchar * local_address(void) {
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list)) {
        return NULL;
    }
    gchar *found = NULL;
    for (int pass = 0; pass < 2 && !found; pass++) {
        for (struct ifaddrs *cur = list; cur && !found; cur = cur->ifa_next) {
            if (!cur->ifa_addr || cur->ifa_addr->sa_family != AF_INET
                || (cur->ifa_flags & IFF_LOOPBACK) || !(cur->ifa_flags & IFF_UP)) {
                continue;
            }
            if (pass == 0 && strcmp(cur->ifa_name, "wlan0")) {
                continue;
            }
            char text[INET_ADDRSTRLEN];
            const struct sockaddr_in *in = (const struct sockaddr_in *) cur->ifa_addr;
            if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text))) {
                found = g_strdup(text);
            }
        }
    }
    freeifaddrs(list);
    return found;
}

/**
 * @return The page's address, newly allocated, NULL without a network
 */
static gchar * page_url(void) {
    gchar *address = local_address();
    if (!address) {
        return NULL;
    }
    G_LOCK(token);
    gchar *url = g_strdup_printf("http://%s:%d/%s", address, VOICE_PORT, conf->voice_token);
    G_UNLOCK(token);
    g_free(address);
    return url;
}

/**
 * @param text Text to show
 * @return A QR code of the text, black on white, NULL if it does not fit
 */
static GdkPixbuf * qr_pixbuf(const gchar *text) {
    uint8_t code[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text, temp, code, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        return NULL;
    }
    const int modules = qrcodegen_getSize(code);
    const int side = (modules + 2 * QR_BORDER) * QR_MODULE;
    GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, side, side);
    gdk_pixbuf_fill(pixbuf, 0xffffffff);
    guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    for (int y = 0; y < modules; y++) {
        for (int x = 0; x < modules; x++) {
            if (!qrcodegen_getModule(code, x, y)) {
                continue;
            }
            for (int dy = 0; dy < QR_MODULE; dy++) {
                guchar *row = pixels + ((y + QR_BORDER) * QR_MODULE + dy) * stride
                              + (x + QR_BORDER) * QR_MODULE * 3;
                memset(row, 0, QR_MODULE * 3);
            }
        }
    }
    return pixbuf;
}

/**
 * Show the address, its QR code and whether a phone is connected
 */
static void update_page(void) {
    if (!voice_page) {
        return;
    }
    gchar *url = serving ? page_url() : NULL;
    GdkPixbuf *pixbuf = url ? qr_pixbuf(url) : NULL;
    gtk_image_set_from_pixbuf(GTK_IMAGE(qr_image), pixbuf);
    gtk_widget_set_visible(qr_image, pixbuf != NULL);
    if (pixbuf) {
        g_object_unref(pixbuf);
    }
    gchar *text = NULL;
    if (!serving) {
        text = g_strdup_printf("没能打开 %d 端口，手机连不进来。", VOICE_PORT);
    } else if (!url) {
        text = g_strdup("Kindle 没连上 Wi-Fi。");
    } else {
        text = g_strdup_printf("手机扫码打开，或者在浏览器里输入：\n%s\n\n"
                               "可以加到书签或主屏幕，下次直接打开。\n%s", url,
                               phone ? "" : "还没有手机连上。");
        if (phone) {
            gchar *with_phone = g_strdup_printf("%s已连上：%s", text, phone);
            g_free(text);
            text = with_phone;
        }
    }
    gtk_label_set_text(GTK_LABEL(address_label), text);
    g_free(text);
    g_free(url);
}

/**
 * Make a new key: the old addresses stop working
 * @return TRUE if it was saved to kterm.conf
 */
static gboolean new_token(void) {
    // no letters easily mistaken for others (l, o, 0, 1) when typed
    static const gchar letters[] = "abcdefghijkmnpqrstuvwxyz23456789";
    G_LOCK(token);
    for (int i = 0; i < TOKEN_LENGTH; i++) {
        conf->voice_token[i] = letters[g_random_int_range(0, (gint) sizeof(letters) - 1)];
    }
    conf->voice_token[TOKEN_LENGTH] = '\0';
    G_UNLOCK(token);
    gchar *line = g_strdup_printf("voice_token = %s", conf->voice_token);
    GError *error = NULL;
    const gboolean saved = confedit_save(conf->conf_path, "voice_token", line, &error);
    if (!saved) {
        g_printerr("kterm: voice key not saved: %s\n", error->message);
        g_error_free(error);
    }
    g_free(line);
    return saved;
}

static void new_token_cb(void) {
    const gboolean saved = new_token();
    g_free(phone);
    phone = NULL;
    update_page();
    gtk_label_set_text(GTK_LABEL(status_label), saved
                       ? "换好了：以前的书签不能再用，请重新扫码。"
                       : "新口令没能存进 kterm.conf，重启 kterm 后会变回去。");
}

/** What a request asks of the terminal, handed to the main thread */
typedef struct {
    /** Text to send, NULL for a ping */
    gchar *text;
    /** Enter after it */
    gboolean run;
    gchar *phone;
    /** Where the main thread puts what it did, for the response */
    GAsyncQueue *reply;
} Request;

/**
 * In the main thread: what became of the text, for the waiting worker
 */
static void reply_cb(const gchar *status, gpointer data) {
    GAsyncQueue *reply = data;
    g_async_queue_push(reply, g_strdup(status));
    g_async_queue_unref(reply);
}

/**
 * In the main thread: a phone was heard from; its text goes to the box
 * or the terminal, see voicebar.c
 */
static gboolean request_cb(gpointer data) {
    Request *request = data;
    const gboolean first = !phone || strcmp(phone, request->phone);
    g_free(phone);
    phone = g_strdup(request->phone);
    if (request->text) {
        voicebar_receive(request->text, request->run, reply_cb, request->reply);
    }
    // the code was scanned: it is in the way now
    if (first && panel_is_open() && gtk_widget_get_visible(voice_page)) {
        panel_close();
    } else if (first) {
        update_page();
    }
    g_free(request->text);
    g_free(request->phone);
    g_free(request);
    return G_SOURCE_REMOVE;
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
 * In a worker thread: serve one connection
 */
static gboolean run_cb(GThreadedSocketService *service, GSocketConnection *connection,
                       GObject *source, gpointer data) {
    UNUSED(service);
    UNUSED(source);
    UNUSED(data);
    g_socket_set_timeout(g_socket_connection_get_socket(connection), SOCKET_TIMEOUT);
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gchar *buffer = g_malloc(HEAD_MAX + BODY_MAX + 1);
    gsize got = 0;
    gchar *end = NULL;
    while (!end && got < HEAD_MAX) {
        const gssize n = g_input_stream_read(in, buffer + got, HEAD_MAX - got, NULL, NULL);
        if (n <= 0) {
            g_free(buffer);
            return TRUE;
        }
        got += (gsize) n;
        buffer[got] = '\0';
        end = strstr(buffer, "\r\n\r\n");
    }
    if (!end) {
        respond_text(out, "431 Request Header Fields Too Large", "");
        g_free(buffer);
        return TRUE;
    }
    const gsize head_size = (gsize) (end - buffer) + 4;
    gchar *head = g_ascii_strdown(buffer, (gssize) head_size);
    const gsize length = content_length(head);
    g_free(head);
    if (length > BODY_MAX) {
        respond_text(out, "413 Payload Too Large", "太长了");
        g_free(buffer);
        return TRUE;
    }
    while (got < head_size + length) {
        const gssize n = g_input_stream_read(in, buffer + got, head_size + length - got, NULL, NULL);
        if (n <= 0) {
            g_free(buffer);
            return TRUE;
        }
        got += (gsize) n;
    }
    buffer[head_size + length] = '\0';

    // "GET /key/send?enter=1 HTTP/1.1"
    gchar **words = g_strsplit(buffer, " ", 3);
    const gchar *method = words[0] ? words[0] : "";
    const gchar *target = words[0] && words[1] ? words[1] : "";
    gchar *path = g_strndup(target, strcspn(target, "?"));
    const gchar *query = strchr(target, '?');
    G_LOCK(token);
    gchar *prefix = g_strdup_printf("/%s", conf->voice_token);
    G_UNLOCK(token);
    const gchar *rest = g_str_has_prefix(path, prefix) ? path + strlen(prefix) : NULL;
    if (rest && *rest && *rest != '/') {
        rest = NULL;
    }
    if (!strcmp(method, "GET") && (!strcmp(path, "/favicon.png") || !strcmp(path, "/favicon.ico")
                                   || g_str_has_prefix(path, "/apple-touch-icon"))) {
        GBytes *icon = g_str_has_prefix(path, "/apple") ? touch_icon : favicon;
        gsize size = 0;
        const gchar *data = g_bytes_get_data(icon, &size);
        respond(out, "200 OK", "image/png", data, size);
    } else if (!rest) {
        respond_text(out, "403 Forbidden", "口令不对：请重新扫 Kindle 上的二维码");
    } else if (!strcmp(method, "GET") && (!*rest || !strcmp(rest, "/"))) {
        respond(out, "200 OK", "text/html; charset=utf-8", VOICE_PAGE, sizeof(VOICE_PAGE) - 1);
    } else if (!strcmp(method, "GET") && !strcmp(rest, "/ping")) {
        respond_text(out, "200 OK", "ok");
        GInetSocketAddress *remote = G_INET_SOCKET_ADDRESS(g_socket_connection_get_remote_address(connection, NULL));
        Request *request = g_new0(Request, 1);
        request->phone = remote ? g_inet_address_to_string(g_inet_socket_address_get_address(remote)) : g_strdup("?");
        if (remote) {
            g_object_unref(remote);
        }
        g_idle_add(request_cb, request);
    } else if (!strcmp(method, "POST") && !strcmp(rest, "/send")) {
        const gchar *body = buffer + head_size;
        if (!g_utf8_validate(body, (gssize) length, NULL)) {
            respond_text(out, "400 Bad Request", "不是 UTF-8 文字");
        } else {
            GInetSocketAddress *remote = G_INET_SOCKET_ADDRESS(g_socket_connection_get_remote_address(connection, NULL));
            Request *request = g_new0(Request, 1);
            request->text = g_strndup(body, length);
            request->run = query && strstr(query, "run=1") != NULL;
            request->phone = remote ? g_inet_address_to_string(g_inet_socket_address_get_address(remote)) : g_strdup("?");
            if (remote) {
                g_object_unref(remote);
            }
            // one reference for the main thread, one for waiting here
            GAsyncQueue *reply = g_async_queue_new_full(g_free);
            request->reply = g_async_queue_ref(reply);
            g_idle_add(request_cb, request);
            gchar *status = g_async_queue_timeout_pop(reply, REPLY_TIMEOUT_US);
            respond_text(out, "200 OK", status ? status : "timeout");
            g_free(status);
            g_async_queue_unref(reply);
        }
    } else {
        respond_text(out, "404 Not Found", "没有这个");
    }
    g_free(prefix);
    g_free(path);
    g_strfreev(words);
    g_free(buffer);
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
 * Show or put away the page with the QR code
 */
void voice_toggle(void) {
    if (!voice_page) {
        return;
    }
    if (panel_is_open() && gtk_widget_get_visible(voice_page)) {
        panel_close();
        return;
    }
    panel_open();
    gtk_label_set_text(GTK_LABEL(status_label), "");
    update_page();
    panel_show_page(voice_page);
}

/**
 * @return The page shown by the "语音" key
 */
static GtkWidget * build_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *header = panel_page_header(page, "手机输入", NULL);
    GtkWidget *renew = panel_button_new("换口令");
    g_signal_connect(renew, "clicked", G_CALLBACK(new_token_cb), NULL);
    gtk_box_pack_start(GTK_BOX(header), renew, FALSE, FALSE, 0);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    qr_image = gtk_image_new();
    gtk_widget_set_valign(qr_image, GTK_ALIGN_START);
    address_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(address_label), 0);
    gtk_label_set_yalign(GTK_LABEL(address_label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(address_label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(address_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(address_label), FALSE);
    gtk_box_pack_start(GTK_BOX(body), qr_image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), address_label, TRUE, TRUE, 0);
    status_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(status_label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(status_label), TRUE);
    gtk_box_pack_start(GTK_BOX(page), body, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), status_label, FALSE, FALSE, 0);
    return page;
}

/**
 * Start listening
 */
static void start_serving(void) {
    if (service) {
        return;
    }
    service = g_threaded_socket_service_new(WORKERS);
    GError *error = NULL;
    if (g_socket_listener_add_inet_port(G_SOCKET_LISTENER(service), VOICE_PORT, NULL, &error)) {
        g_signal_connect(service, "run", G_CALLBACK(run_cb), NULL);
        g_socket_service_start(service);
        serving = TRUE;
        D printf("voice: serving on port %d\n", VOICE_PORT);
    } else {
        g_printerr("kterm: voice page not served: %s\n", error->message);
        g_error_free(error);
        serving = FALSE;
    }
}

/**
 * Stop listening: the port is closed again
 */
static void stop_serving(void) {
    if (!service) {
        return;
    }
    g_socket_service_stop(service);
    g_socket_listener_close(G_SOCKET_LISTENER(service));
    g_object_unref(service);
    service = NULL;
    serving = FALSE;
    D printf("voice: stopped\n");
}

/**
 * @return TRUE if typing from a phone is switched on (conf->voice_on)
 */
gboolean voice_enabled(void) {
    return conf->voice_on != 0;
}

/**
 * Switch typing from a phone on or off: the page is served and the
 * "语音" key shown only while it is on
 * @param on Non-zero for on
 */
void voice_set_enabled(guint on) {
    conf->voice_on = on ? 1 : 0;
    if (on) {
        start_serving();
    } else {
        stop_serving();
        voicebar_clear();
        if (voice_page && panel_is_open() && gtk_widget_get_visible(voice_page)) {
            panel_close();
        }
    }
    kterm_keyboard_relayout();
}

/**
 * Add the page to the panel and the box over the shortcut bar; serve the
 * page if switched on
 * @param overlay Overlay of the window
 * @param terminal Terminal typed into
 * @param shortbar Shortcut bar, NULL if there is none
 */
void voice_init(GtkWidget *overlay, GtkWidget *terminal, GtkWidget *shortbar) {
    if (strlen(conf->voice_token) < TOKEN_LENGTH) {
        new_token();
    }
    favicon = draw_icon(64);
    touch_icon = draw_icon(180);
    voicebar_init(overlay, terminal, shortbar);
    voice_page = build_page();
    panel_add_page(voice_page, FALSE);
    if (voice_enabled()) {
        start_serving();
    }
}
