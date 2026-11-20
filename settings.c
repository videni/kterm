/* settings.c
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
 * The settings page of the panel: font size, and on the Kindle the screen
 * orientation, how often the screen refreshes, full refreshes, typing from
 * a phone. Each is used at once and written to kterm.conf.
 */

#include <vte/vte.h>
#include "config.h"
#include "confedit.h"
#include "kterm.h"
#include "panel.h"
#include "refresh.h"
#include "settings.h"
#ifdef KINDLE
#include "flash.h"
#include "kindle.h"
#include "tmux.h"
#include "voice.h"
#endif

extern KTconf *conf;

#if GTK_CHECK_VERSION(3,2,0)
#ifdef KINDLE
/** Orientation buttons of the settings page */
static GtkWidget *portrait_button = NULL;
static GtkWidget *landscape_button = NULL;

/**
 * @return TRUE if the screen is upright (or its orientation unknown)
 */
static gboolean is_portrait(void) {
    return conf->orientation != 'R' && conf->orientation != 'L';
}

/**
 * Mark the orientation the screen is in: its button is drawn black
 */
static void orientation_show(void) {
    GtkWidget *on = is_portrait() ? portrait_button : landscape_button;
    GtkWidget *off = is_portrait() ? landscape_button : portrait_button;
    gtk_style_context_add_class(gtk_widget_get_style_context(on), "selected");
    gtk_style_context_remove_class(gtk_widget_get_style_context(off), "selected");
}

/**
 * Orientation button callback. The panel stays open on the settings page:
 * the button drawn black tells the change was made, and the font size can
 * be set for the new width at once. The orientation is written to
 * kterm.conf: kterm starts in it the next time. On leaving, the Kindle is
 * still turned back as it was, for its own screens.
 * @param button Calling widget
 * @param data Orientation asked for: 'U' or 'R'
 */
static void orientation_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    const gchar request = (gchar) GPOINTER_TO_INT(data);
    if ((request == 'U') == is_portrait()) {
        return;
    }
    if (set_orientation(request)) {
        conf->orientation = request;
        gchar *line = g_strdup_printf("orientation = %c", request);
        GError *error = NULL;
        if (!confedit_save(conf->conf_path, "orientation", line, &error)) {
            D printf("orientation not saved: %s\n", error->message);
            g_error_free(error);
        }
        g_free(line);
    }
    orientation_show();
}

/**
 * @return Row of the settings page choosing the screen orientation
 */
static GtkWidget * orientation_row(void) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    portrait_button = panel_button_new("竖屏");
    landscape_button = panel_button_new("横屏");
    g_signal_connect(portrait_button, "clicked", G_CALLBACK(orientation_cb), GINT_TO_POINTER('U'));
    g_signal_connect(landscape_button, "clicked", G_CALLBACK(orientation_cb), GINT_TO_POINTER('R'));
    gtk_box_pack_start(GTK_BOX(row), portrait_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), landscape_button, TRUE, TRUE, 0);
    orientation_show();
    return row;
}
#endif

#ifdef KINDLE
/** One choice of a row on the settings page */
typedef struct {
    const gchar *label;
    guint value;
} Choice;

/** A row of choices on the settings page, used at once and written to
 *  kterm.conf */
typedef struct {
    const Choice *choices;
    guint count;
    /** Setting in conf */
    guint *value;
    /** Its key in kterm.conf */
    const gchar *key;
    /** Puts a choice to use */
    void (*apply)(guint value);
    GtkWidget *buttons[4];
} ChoiceRow;

/** Refreshes a second while the terminal changes on its own (0: no limit) */
static const Choice refresh_choices[] = {
    { "省电", 1 }, { "标准", REFRESH_RATE_DEFAULT }, { "流畅", 0 },
};
static ChoiceRow refresh_row_data = { refresh_choices, G_N_ELEMENTS(refresh_choices), NULL, "refresh_rate", refresh_set_rate, { NULL } };

/** Pages turned and swipes shown as a full refresh: none, those after the
 *  terminal has been written over, all */
static const Choice full_refresh_choices[] = {
    { "关", FLASH_NEVER }, { "少", FLASH_WRITTEN_OVER }, { "多", FLASH_ALWAYS },
};
static ChoiceRow full_refresh_row_data = { full_refresh_choices, G_N_ELEMENTS(full_refresh_choices), NULL, "full_refresh", flash_set_when, { NULL } };

static const Choice on_off[] = {
    { "关", 0 }, { "开", 1 },
};

/** "文字加深": coloured and dim text drawn dark, see kterm_set_text_contrast() */
static ChoiceRow contrast_row_data = { on_off, G_N_ELEMENTS(on_off), NULL, "text_contrast", kterm_set_text_contrast, { NULL } };

static const Choice tab_choices[] = {
    { "隐藏", 1 }, { "常显", 0 },
};
static ChoiceRow tab_row_data = { tab_choices, G_N_ELEMENTS(tab_choices), NULL, "auto_hide_tabs", kterm_set_tab_hidden, { NULL } };

/** "tmux 集成": kterm asks tmux over the ssh connection (scrolled back?
 *  which sessions?), switches its windows by swipes; see tmux.c */
static ChoiceRow tmux_row_data = { on_off, G_N_ELEMENTS(on_off), NULL, "tmux", tmux_set_enabled, { NULL } };

/** Typing from a phone (语音输入), see voice.c: off unless asked for, as it
 *  opens a port to the local network */
static ChoiceRow voice_row_data = { on_off, G_N_ELEMENTS(on_off), NULL, "voice", voice_set_enabled, { NULL } };

/**
 * Draw the choice in effect black
 * @param row Row of choices
 */
static void choice_show(const ChoiceRow *row) {
    for (guint i = 0; i < row->count; i++) {
        GtkStyleContext *style = gtk_widget_get_style_context(row->buttons[i]);
        if (row->choices[i].value == *row->value) {
            gtk_style_context_add_class(style, "selected");
        } else {
            gtk_style_context_remove_class(style, "selected");
        }
    }
}

/**
 * Choice button of the settings page: used at once, written to kterm.conf
 * @param button Calling widget
 * @param data Row of choices
 */
static void choice_cb(GtkWidget *button, gpointer data) {
    ChoiceRow *row = data;
    guint i = 0;
    while (i < row->count && row->buttons[i] != button) {
        i++;
    }
    if (i == row->count) {
        return;
    }
    const guint value = row->choices[i].value;
    row->apply(value);
    choice_show(row);
    gchar *line = g_strdup_printf("%s = %u", row->key, value);
    GError *error = NULL;
    if (!confedit_save(conf->conf_path, row->key, line, &error)) {
        D printf("%s not saved: %s\n", row->key, error->message);
        g_error_free(error);
    }
    g_free(line);
}

/**
 * @param row Choices
 * @param value Setting in conf they are for
 * @return Row of buttons of the settings page, one for each choice
 */
static GtkWidget * choice_row(ChoiceRow *row, guint *value) {
    row->value = value;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(box), TRUE);
    for (guint i = 0; i < row->count && i < G_N_ELEMENTS(row->buttons); i++) {
        row->buttons[i] = panel_button_new(row->choices[i].label);
        g_signal_connect(row->buttons[i], "clicked", G_CALLBACK(choice_cb), row);
        gtk_box_pack_start(GTK_BOX(box), row->buttons[i], TRUE, TRUE, 0);
    }
    choice_show(row);
    return box;
}
#endif

/** Font sizes offered on the settings page */
#define FONT_SIZE_MIN 4
#define FONT_SIZE_MAX 30

/**
 * Font size buttons of the settings page: the new size is used at once and
 * written to kterm.conf for the next start.
 * @param button Calling widget, carries the step (-1 or 1)
 * @param label Shows the size
 */
static void font_size_cb(GtkWidget *button, gpointer label) {
    const gint step = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "step"));
    const gint size = CLAMP((gint) conf->font_size + step, FONT_SIZE_MIN, FONT_SIZE_MAX);
    if (size == (gint) conf->font_size) {
        return;
    }
    kterm_set_font_size((guint) size);
    gchar *text = g_strdup_printf("%d", size);
    gtk_label_set_text(GTK_LABEL(label), text);
    g_free(text);
    gchar *line = g_strdup_printf("font_size = %d", size);
    GError *error = NULL;
    if (!confedit_save(conf->conf_path, "font_size", line, &error)) {
        D printf("font size not saved: %s\n", error->message);
        g_error_free(error);
    }
    g_free(line);
}

/**
 * @param text Button text
 * @param step Font size change
 * @param label Shows the size
 * @return Button
 */
static GtkWidget * font_size_button(const gchar *text, gint step, GtkWidget *label) {
    GtkWidget *button = panel_button_new(text);
    g_object_set_data(G_OBJECT(button), "step", GINT_TO_POINTER(step));
    g_signal_connect(button, "clicked", G_CALLBACK(font_size_cb), label);
    return button;
}

/**
 * Fill the settings page of the panel
 */
void settings_init(void) {
    GtkWidget *font_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gchar *size = g_strdup_printf("%u", conf->font_size);
    GtkWidget *size_label = gtk_label_new(size);
    g_free(size);
    gtk_label_set_width_chars(GTK_LABEL(size_label), 3);
    gtk_box_pack_start(GTK_BOX(font_row), font_size_button("−", -1, size_label), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(font_row), size_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(font_row), font_size_button("+", 1, size_label), FALSE, FALSE, 0);
    panel_add_setting("字号", NULL, font_row);
#ifdef KINDLE
    panel_add_setting("屏幕方向", NULL, orientation_row());
    panel_add_setting("标签栏", "轻点终端右上角切换显隐\n长按标签标题改名",
                      choice_row(&tab_row_data, &conf->auto_hide_tabs));
    // how often the screen follows a terminal changing on its own (not
    // while typing: then always at once)
    panel_add_setting("刷新", NULL, choice_row(&refresh_row_data, &conf->refresh_rate));
    panel_add_setting("全刷", NULL, choice_row(&full_refresh_row_data, &conf->full_refresh));
    panel_add_setting("文字加深", NULL, choice_row(&contrast_row_data, &conf->text_contrast));
    panel_add_setting("tmux 集成", NULL, choice_row(&tmux_row_data, &conf->tmux_on));
    panel_add_setting("语音输入", NULL, choice_row(&voice_row_data, &conf->voice_on));
#endif
}
#endif
