/* imageview.c
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
 * Images whose path the terminal shows (Claude Code's Read(~/a.png), ls):
 * a tap on the path shows the image over the whole window, a tap on the
 * image takes it away.
 *
 * The file is read where the terminal's program is (remote.c): on the far
 * machine of the ssh session, or here. A path not starting with / or ~ is
 * taken from the directory of this terminal's tmux pane, or here of the
 * terminal's program, or else the home directory. A path with spaces in it
 * is not seen whole.
 */

#define PCRE2_CODE_UNIT_WIDTH 0
#include <pcre2.h>
#include <string.h>
#include <vte/vte.h>
#include "config.h"
#include "flash.h"
#include "imageview.h"
#include "panel.h"
#include "remote.h"
#include "tmux.h"

/** Larger files are not fetched, for the Kindle's memory */
#define IMAGE_MAX_BYTES 15728640
#define IMAGE_MAX_TEXT "15MB"
/** Fetching through ssh stopped after this long, ms */
#define IMAGE_TIMEOUT_MS 30000

/** What a path may follow: spaces, quotes, brackets, separators, also
 *  the Chinese ones; not ":", that of a URL */
#define BEFORE_PATH "\\s'\"`()<>\\[\\]{}|,;=：，。、（）「」《》“”‘’"
/** Characters ending a path: those, and ":" (a line number after it) */
#define NOT_PATH BEFORE_PATH ":"
/** A path ending in an image's extension, at the line's start or after
 *  BEFORE_PATH */
static const gchar IMAGE_PATTERN[] =
    "(?<![^" BEFORE_PATH "])[^" NOT_PATH "]+\\.(?:png|jpe?g|gif|webp|bmp)(?!\\w|\\.\\w)";

/** Prints "ok" and the file, or "missing" or "big"; FILE is the path */
static const gchar FETCH_SCRIPT[] =
    "PATH=\"$PATH:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin\"\n"
    "f=$FILE\n"
    "case $f in\n"
    "  '~/'*) f=$HOME/${f#'~/'} ;;\n"
    "  /*) ;;\n"
    "  *) d=\n"
    "     if command -v tmux >/dev/null 2>&1; then\n"
    TMUX_FIND_CLIENT
    "       [ -n \"$best\" ] && { set -- $best; d=$(tmux display-message -p -t \"$3\" '#{pane_current_path}' 2>/dev/null); }\n"
    "     fi\n"
    "     f=${d:-${KTERM_DIR:-$HOME}}/$f ;;\n"
    "esac\n"
    "[ -f \"$f\" ] && [ -r \"$f\" ] || { echo missing; exit 0; }\n"
    "[ $(wc -c < \"$f\") -le " G_STRINGIFY(IMAGE_MAX_BYTES) " ] || { echo big; exit 0; }\n"
    "echo ok\n"
    "cat \"$f\"\n";

/** Over the whole window: the image, or what became of it */
static GtkWidget *view = NULL;
static GtkWidget *picture = NULL;
static GtkWidget *message = NULL;
/** VTE's tag of IMAGE_PATTERN */
static int match_tag = -1;
/** Counts the images asked for: an answer for one taken away since is
 *  dropped */
static guint asked = 0;

/**
 * Say what became of the image, in place of it
 * @param path Its path
 * @param what What became of it
 */
static void show_message(const gchar *path, const gchar *what) {
    gchar *text = g_strdup_printf("%s\n\n%s", path, what);
    gtk_label_set_text(GTK_LABEL(message), text);
    g_free(text);
    gtk_image_clear(GTK_IMAGE(picture));
    gtk_widget_show(message);
}

/** The image being decoded */
typedef struct {
    guint asked;
    gchar *path;
} Fetch;

static void fetch_free(Fetch *fetch) {
    g_free(fetch->path);
    g_free(fetch);
}

static void decoded_cb(GObject *source, GAsyncResult *result, gpointer data) {
    UNUSED(source);
    Fetch *fetch = data;
    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream_finish(result, &error);
    D printf("image: decoded %dx%d %s\n", pixbuf ? gdk_pixbuf_get_width(pixbuf) : 0,
             pixbuf ? gdk_pixbuf_get_height(pixbuf) : 0, error ? error->message : "");
    if (fetch->asked == asked) {
        if (pixbuf) {
            // as the camera was held
            GdkPixbuf *upright = gdk_pixbuf_apply_embedded_orientation(pixbuf);
            gtk_image_set_from_pixbuf(GTK_IMAGE(picture), upright);
            g_object_unref(upright);
            gtk_widget_hide(message);
            flash_window_next_draw();
        } else {
            gchar *what = g_strdup_printf("看不了这个图片：%s", error->message);
            show_message(fetch->path, what);
            g_free(what);
        }
    }
    g_clear_error(&error);
    if (pixbuf) {
        g_object_unref(pixbuf);
    }
    fetch_free(fetch);
}

static void fetched_cb(GBytes *output, gpointer data) {
    Fetch *fetch = data;
    if (fetch->asked != asked) {
        fetch_free(fetch);
        return;
    }
    gsize size = 0;
    const gchar *bytes = output ? g_bytes_get_data(output, &size) : NULL;
    const gchar *line_end = bytes ? memchr(bytes, '\n', size) : NULL;
    const gsize status = line_end ? (gsize) (line_end - bytes) : 0;
    D printf("image: fetched %" G_GSIZE_FORMAT " bytes, \"%.*s\"\n", size, (int) status, bytes ? bytes : "");
    if (status == 2 && !strncmp(bytes, "ok", 2)) {
        // decoded to the window's size, not larger: the Kindle's memory
        const gint width = gtk_widget_get_allocated_width(view) - 2 * PANEL_SPACING;
        const gint height = gtk_widget_get_allocated_height(view) - 2 * PANEL_SPACING;
        GBytes *file = g_bytes_new_from_bytes(output, status + 1, size - status - 1);
        GInputStream *stream = g_memory_input_stream_new_from_bytes(file);
        gdk_pixbuf_new_from_stream_at_scale_async(stream, MAX(width, 1), MAX(height, 1), TRUE, NULL,
                                                  decoded_cb, fetch);
        g_object_unref(stream);
        g_bytes_unref(file);
        return;
    }
    if (status == 7 && !strncmp(bytes, "missing", 7)) {
        show_message(fetch->path, "找不到这个文件");
    } else if (status == 3 && !strncmp(bytes, "big", 3)) {
        show_message(fetch->path, "图片太大（超过 " IMAGE_MAX_TEXT "）");
    } else if (remote_is_ssh()) {
        show_message(fetch->path, "取不到图片\n自己输入的 ssh 要用密钥登录，或者从面板连接");
    } else {
        show_message(fetch->path, "取不到图片");
    }
    fetch_free(fetch);
}

/**
 * Show an image over the window: what is being done first, then the image
 * @param path Its path, as the terminal shows it
 */
static void open_image(const gchar *path) {
    D printf("image: %s\n", path);
    asked++;
    show_message(path, "正在取图片…");
    gtk_widget_show(view);
    flash_window_next_draw();
    Fetch *fetch = g_new0(Fetch, 1);
    fetch->asked = asked;
    fetch->path = g_strdup(path);
    gchar *var = g_strconcat("FILE=", path, NULL);
    const gchar *vars[] = { var, NULL };
    if (!remote_run(FETCH_SCRIPT, vars, TRUE, IMAGE_TIMEOUT_MS, fetched_cb, fetch)) {
        show_message(path, "取不到图片");
        fetch_free(fetch);
    }
    g_free(var);
}

/**
 * A tap on the image takes it away; the press too is kept from what is
 * under it
 */
static gboolean view_event_cb(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    UNUSED(widget);
    UNUSED(data);
    if (event->type == GDK_BUTTON_RELEASE) {
        asked++;
        gtk_widget_hide(view);
        gtk_image_clear(GTK_IMAGE(picture));
        flash_window_next_draw();
    }
    return TRUE;
}

/**
 * A tap on the terminal: on the path of an image, the image is shown
 * @param terminal Terminal widget
 * @param event Release of the tap
 * @return TRUE if it was on such a path
 */
gboolean imageview_tap(GtkWidget *terminal, GdkEvent *event) {
    if (match_tag < 0) {
        return FALSE;
    }
    int tag = -1;
    gchar *path = vte_terminal_match_check_event(VTE_TERMINAL(terminal), event, &tag);
    const gboolean image = path && tag == match_tag;
    if (image) {
        open_image(path);
    }
    g_free(path);
    return image;
}

/**
 * @param overlay Window's overlay: the image is shown over everything
 * @param terminal Terminal widget, whose image paths are looked for
 */
void imageview_init(GtkWidget *overlay, GtkWidget *terminal) {
    GError *error = NULL;
    VteRegex *regex = vte_regex_new_for_match(IMAGE_PATTERN, -1, PCRE2_MULTILINE | PCRE2_CASELESS, &error);
    if (regex) {
        match_tag = vte_terminal_match_add_regex(VTE_TERMINAL(terminal), regex, 0);
        vte_regex_unref(regex);
    } else {
        D printf("image: %s\n", error->message);
        g_error_free(error);
    }
    view = gtk_event_box_new();
    gtk_widget_set_name(view, "ktermImageView");
    gtk_widget_add_events(view, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(view, "button-press-event", G_CALLBACK(view_event_cb), NULL);
    g_signal_connect(view, "button-release-event", G_CALLBACK(view_event_cb), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    picture = gtk_image_new();
    message = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(message), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(message), PANGO_WRAP_CHAR);
    gtk_label_set_justify(GTK_LABEL(message), GTK_JUSTIFY_CENTER);
    gtk_widget_set_margin_start(message, PANEL_SPACING * 4);
    gtk_widget_set_margin_end(message, PANEL_SPACING * 4);
    gtk_box_pack_start(GTK_BOX(box), picture, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), message, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(view), box);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), view);
    gtk_widget_show_all(view);
    gtk_widget_hide(view);
}
