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
 * only waits for connections, which costs nothing while there are none.
 * launch.sh lets them through the Kindle's firewall on Wi-Fi.
 *
 * The server itself is voiceserver.c; this file switches it on and off,
 * keeps the key, shows the page in the panel and hands what comes to the
 * box (voicebar.c).
 */

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <gtk/gtk.h>
#include "config.h"
#include "confedit.h"
#include "kterm.h"
#include "panel.h"
#include "qrcodegen.h"
#include "voice.h"
#include "voicebar.h"
#include "voiceserver.h"

extern KTconf *conf;

/** Port the page is served on */
#define VOICE_PORT 7788
/** Characters of the key */
#define TOKEN_LENGTH 10
/** Size of a QR code module on the screen, px */
#define QR_MODULE 7
/** Empty modules around the code, as the standard asks */
#define QR_BORDER 4

static GtkWidget *voice_page = NULL;
static GtkWidget *qr_image = NULL;
static GtkWidget *address_label = NULL;
static GtkWidget *status_label = NULL;
/** Address of the phone heard from last, NULL before any */
static gchar *phone = NULL;


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
    gchar *url = g_strdup_printf("http://%s:%d/%s", address, VOICE_PORT, conf->voice_token);
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
    gchar *url = voiceserver_running() ? page_url() : NULL;
    GdkPixbuf *pixbuf = url ? qr_pixbuf(url) : NULL;
    gtk_image_set_from_pixbuf(GTK_IMAGE(qr_image), pixbuf);
    gtk_widget_set_visible(qr_image, pixbuf != NULL);
    if (pixbuf) {
        g_object_unref(pixbuf);
    }
    gchar *text = NULL;
    if (!voiceserver_running()) {
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
    for (int i = 0; i < TOKEN_LENGTH; i++) {
        conf->voice_token[i] = letters[g_random_int_range(0, (gint) sizeof(letters) - 1)];
    }
    conf->voice_token[TOKEN_LENGTH] = '\0';
    voiceserver_set_token(conf->voice_token);
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

/**
 * In the main thread: a phone was heard from; its text goes to the box
 * or the terminal, see voicebar.c
 */
static void request_cb(const VoiceRequest *request) {
    const gboolean first = !phone || strcmp(phone, request->phone);
    g_free(phone);
    phone = g_strdup(request->phone);
    if (request->text) {
        voicebar_receive(request->text, request->run, voiceserver_answer, request->reply);
    }
    // the code was scanned: it is in the way now
    if (first && panel_is_open() && gtk_widget_get_visible(voice_page)) {
        panel_close();
    } else if (first) {
        update_page();
    }
}

static void start_serving(void);

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
    if (voice_enabled()) {
        // the port may be free by now
        start_serving();
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
    GtkWidget *header = panel_page_header(page, "语音输入", NULL);
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
 * Start listening, if not yet: switched on, or the port was not free
 */
static void start_serving(void) {
    GError *error = NULL;
    if (!voiceserver_start(VOICE_PORT, request_cb, &error)) {
        // tried again when the page is shown or the setting switched on
        g_printerr("kterm: voice page not served: %s\n", error->message);
        g_error_free(error);
    }
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
        voiceserver_stop();
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
    voiceserver_set_token(conf->voice_token);
    voicebar_init(overlay, terminal, shortbar);
    voice_page = build_page();
    panel_add_page(voice_page, FALSE);
    if (voice_enabled()) {
        start_serving();
    }
}
