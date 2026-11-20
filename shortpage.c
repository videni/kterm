/* shortpage.c
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
 * The panel's shortcuts pages: the buttons of the shortcut bar in a list,
 * and a form to add or change one. What is typed as keys is said back in
 * words as it is typed, so that a mistake shows before it is saved.
 */

#include <gtk/gtk.h>
#include "config.h"
#include "panel.h"
#include "shortpage.h"
#include "keyboard.h"
#include "kterm.h"
#include "keyseq.h"
#include "shortbar.h"
#include "shortcuts.h"

/** Buttons listed at once; more are reached with the page buttons */
#define ROWS_PER_PAGE 4
/** Space between the parts of a page, in pixels */
#define SPACING 12
/** Shown under the keys field while it is empty */
#define KEYS_HINT "C-x 是 Ctrl+x，M-x 是 Alt+x；空格分开先后按的键；文字加引号，如 \"git pull\" Enter"

static GtkWidget *list_page = NULL;
static GtkWidget *rows_box = NULL;
static GtkWidget *empty_label = NULL;
static GtkWidget *prev_button = NULL;
static GtkWidget *next_button = NULL;
static GtkWidget *page_label = NULL;
static GtkWidget *form_page = NULL;
static GtkWidget *form_title = NULL;
static GtkWidget *entry_name = NULL;
static GtkWidget *entry_keys = NULL;
static GtkWidget *keys_said = NULL;
static GtkWidget *form_message = NULL;
static GtkWidget *delete_button = NULL;
/** Prefix switch of the form, black while on */
static GtkWidget *keyboard_switch = NULL;
static gboolean form_keyboard = FALSE;
static const gchar *const visibility_labels[] = {
    [SHORTCUT_SHOW_ALWAYS] = "始终显示",
    [SHORTCUT_SHOW_WITH_KEYBOARD] = "仅键盘展开时",
    [SHORTCUT_SHOW_IN_PREVIEW] = "仅预览时",
};
static GtkWidget *visibility_buttons[G_N_ELEMENTS(visibility_labels)];
static ShortcutVisibility form_visibility = SHORTCUT_SHOW_ALWAYS;

/**
 * @param on TRUE: the shortcut being edited is a prefix
 */
static void set_form_keyboard(gboolean on) {
    form_keyboard = on;
    GtkStyleContext *style = gtk_widget_get_style_context(keyboard_switch);
    if (on) {
        gtk_style_context_add_class(style, "selected");
    } else {
        gtk_style_context_remove_class(style, "selected");
    }
}

static void keyboard_switch_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    UNUSED(data);
    set_form_keyboard(!form_keyboard);
}

static void set_form_visibility(ShortcutVisibility visibility) {
    form_visibility = visibility;
    for (guint i = 0; i < G_N_ELEMENTS(visibility_buttons); i++) {
        GtkStyleContext *style = gtk_widget_get_style_context(visibility_buttons[i]);
        if (i == (guint) visibility) {
            gtk_style_context_add_class(style, "selected");
        } else {
            gtk_style_context_remove_class(style, "selected");
        }
    }
}

static void visibility_clicked_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    set_form_visibility((ShortcutVisibility) GPOINTER_TO_UINT(data));
}

/** First button of the list page shown */
static guint page_start = 0;
static guint rendered_page_start = G_MAXUINT;
/** Index of the button in the form, -1 for a new one */
static gint editing = -1;

static void show_list(void);

/**
 * The form, empty or with a button to change
 * @param index Index of the button, -1 for a new one
 */
static void show_form(gint index) {
    GPtrArray *items = shortbar_items();
    const Shortcut *item = index >= 0 ? g_ptr_array_index(items, (guint) index) : NULL;
    editing = index;
    // typed with the built-in keyboard, wherever it is at the moment
    kterm_use_builtin_keyboard();
    gtk_label_set_text(GTK_LABEL(form_title), item ? "编辑快捷键" : "新建快捷键");
    // a button showing its keys has no name of its own
    const gboolean named = item && g_strcmp0(item->label, item->spec) != 0;
    gtk_entry_set_text(GTK_ENTRY(entry_name), named ? item->label : "");
    gtk_entry_set_text(GTK_ENTRY(entry_keys), item ? item->spec : "");
    set_form_keyboard(item && item->keyboard);
    set_form_visibility(item ? item->visibility : SHORTCUT_SHOW_ALWAYS);
    gtk_label_set_text(GTK_LABEL(form_message), "");
    gtk_widget_set_visible(delete_button, item != NULL);
    panel_show_page(form_page);
    gtk_widget_grab_focus(item ? entry_keys : entry_name);
}

static void edit_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    show_form(GPOINTER_TO_INT(data));
}

static void new_cb(void) {
    show_form(-1);
}

/**
 * Write the list to the file, telling on the page if that failed
 * @param message Label for the message
 * @return TRUE on success
 */
static gboolean save_list(GtkWidget *message) {
    GError *error = NULL;
    if (shortbar_save(&error)) {
        return TRUE;
    }
    gchar *text = g_strdup_printf("没有存进文件：%s", error->message);
    gtk_label_set_text(GTK_LABEL(message), text);
    gtk_widget_show(message);
    g_free(text);
    g_error_free(error);
    return FALSE;
}

/**
 * Up button of a row: the button goes one place to the left on the bar
 */
static void up_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    GPtrArray *items = shortbar_items();
    const guint index = GPOINTER_TO_UINT(data);
    if (index == 0 || index >= items->len) {
        return;
    }
    gpointer moved = items->pdata[index];
    items->pdata[index] = items->pdata[index - 1];
    items->pdata[index - 1] = moved;
    // keep the moved button in sight
    if (index - 1 < page_start) {
        page_start -= ROWS_PER_PAGE;
    }
    rendered_page_start = G_MAXUINT;
    show_list();
    save_list(empty_label);
}

/**
 * One row of the list: the button and what it does, and a button to move
 * it up
 * @param index Index of the button
 * @return Row widget
 */
static GtkWidget * build_row(guint index) {
    const Shortcut *item = g_ptr_array_index(shortbar_items(), index);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING);
    gchar *said = keyseq_describe(item->spec, NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>   %s%s%s%s", item->label, said ? said : item->spec,
                                            item->keyboard ? "，再弹出键盘按一个键" : "",
                                            item->visibility == SHORTCUT_SHOW_ALWAYS ? "" : " · ",
                                            item->visibility == SHORTCUT_SHOW_ALWAYS ? "" : visibility_labels[item->visibility]);
    g_free(said);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    g_free(markup);
    GtkWidget *edit = gtk_button_new();
    gtk_widget_set_name(edit, PANEL_BUTTON_NAME);
    gtk_widget_set_can_focus(edit, FALSE);
    gtk_container_add(GTK_CONTAINER(edit), label);
    g_signal_connect(edit, "clicked", G_CALLBACK(edit_cb), GINT_TO_POINTER((gint) index));
    GtkWidget *up = panel_button_new("↑");
    gtk_widget_set_sensitive(up, index > 0);
    g_signal_connect(up, "clicked", G_CALLBACK(up_cb), GUINT_TO_POINTER(index));
    gtk_box_pack_start(GTK_BOX(row), edit, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), up, FALSE, FALSE, 0);
    return row;
}

/** Refresh the shortcut list before display. */
static void refresh_list(void) {
    GPtrArray *items = shortbar_items();
    const guint count = items->len;
    if (page_start >= count) {
        page_start = count ? ((count - 1) / ROWS_PER_PAGE) * ROWS_PER_PAGE : 0;
    }
    if (rendered_page_start != page_start) {
        panel_clear_box(rows_box);
        guint shown = 0;
        for (guint i = page_start; i < count && i < page_start + ROWS_PER_PAGE; i++, shown++) {
            gtk_box_pack_start(GTK_BOX(rows_box), build_row(i), FALSE, FALSE, 0);
        }
        // Pad a short last page to keep the paging buttons in place.
        for (; count > ROWS_PER_PAGE && shown < ROWS_PER_PAGE; shown++) {
            GtkWidget *filler = panel_button_new(" ");
            gtk_widget_set_sensitive(filler, FALSE);
            gtk_widget_set_opacity(filler, 0);
            gtk_box_pack_start(GTK_BOX(rows_box), filler, FALSE, FALSE, 0);
        }
        gtk_widget_show_all(rows_box);
        rendered_page_start = page_start;
    }
    gtk_label_set_text(GTK_LABEL(empty_label), "还没有快捷键，点“新建”添加");
    gtk_widget_set_visible(empty_label, count == 0);
    const gboolean paged = count > ROWS_PER_PAGE;
    gtk_widget_set_visible(prev_button, paged);
    gtk_widget_set_visible(next_button, paged);
    gtk_widget_set_visible(page_label, paged);
    if (paged) {
        gchar *pages = g_strdup_printf("%u/%u", page_start / ROWS_PER_PAGE + 1,
                                       (count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE);
        gtk_label_set_text(GTK_LABEL(page_label), pages);
        g_free(pages);
        gtk_widget_set_sensitive(prev_button, page_start > 0);
        gtk_widget_set_sensitive(next_button, page_start + ROWS_PER_PAGE < count);
    }
}

static void show_list(void) {
    panel_show_page(list_page);
}

static void page_cb(GtkWidget *button, gpointer data) {
    UNUSED(button);
    if (GPOINTER_TO_INT(data) < 0) {
        page_start = page_start >= ROWS_PER_PAGE ? page_start - ROWS_PER_PAGE : 0;
    } else {
        page_start += ROWS_PER_PAGE;
    }
    show_list();
}

/**
 * Say what the keys typed so far do
 */
static void keys_changed_cb(GtkWidget *entry, gpointer data) {
    UNUSED(data);
    const gchar *spec = gtk_entry_get_text(GTK_ENTRY(entry));
    gchar *trimmed = g_strstrip(g_strdup(spec));
    gchar *text = NULL;
    if (!*trimmed) {
        text = g_strdup(KEYS_HINT);
    } else {
        GError *error = NULL;
        gchar *said = keyseq_describe(spec, &error);
        text = said ? g_strdup_printf("→ %s", said) : g_strdup_printf("✗ %s", error->message);
        g_free(said);
        g_clear_error(&error);
    }
    gtk_label_set_text(GTK_LABEL(keys_said), text);
    g_free(text);
    g_free(trimmed);
}

static void save_cb(void) {
    GPtrArray *items = shortbar_items();
    GError *error = NULL;
    Shortcut *item = shortcut_new(gtk_entry_get_text(GTK_ENTRY(entry_name)),
                                  gtk_entry_get_text(GTK_ENTRY(entry_keys)), form_keyboard, &error);
    if (!item) {
        gtk_label_set_text(GTK_LABEL(form_message), error->message);
        g_error_free(error);
        return;
    }
    item->visibility = form_visibility;
    if (editing >= 0 && (guint) editing < items->len) {
        shortcut_free(items->pdata[editing]);
        items->pdata[editing] = item;
    } else if (items->len >= SHORTCUTS_MAX) {
        gchar *text = g_strdup_printf("最多 %d 个快捷键", SHORTCUTS_MAX);
        gtk_label_set_text(GTK_LABEL(form_message), text);
        g_free(text);
        shortcut_free(item);
        return;
    } else {
        g_ptr_array_add(items, item);
        // the new one is on the last page of the list
        page_start = ((items->len - 1) / ROWS_PER_PAGE) * ROWS_PER_PAGE;
    }
    rendered_page_start = G_MAXUINT;
    show_list();
    save_list(empty_label);
}

static void delete_cb(void) {
    GPtrArray *items = shortbar_items();
    if (editing >= 0 && (guint) editing < items->len) {
        g_ptr_array_remove_index(items, (guint) editing);
        rendered_page_start = G_MAXUINT;
    }
    show_list();
    save_list(empty_label);
}

static void entry_activate_cb(GtkWidget *entry, gpointer next) {
    UNUSED(entry);
    if (next) {
        gtk_widget_grab_focus(GTK_WIDGET(next));
    } else {
        save_cb();
    }
}

/**
 * @param grid Form grid
 * @param row Grid row
 * @param title Field label
 * @param hint Shown in the empty field
 * @return Entry widget
 */
static GtkWidget * add_field(GtkWidget *grid, gint row, const gchar *title, const gchar *hint) {
    GtkWidget *label = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), hint);
    gtk_widget_set_hexpand(entry, TRUE);
    // no capital letter suggestions or spelling: keys are typed as they are
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_URL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
    return entry;
}

static GtkWidget * build_list_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING);
    gtk_widget_set_halign(header, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);
    prev_button = panel_button_new("‹");
    next_button = panel_button_new("›");
    page_label = gtk_label_new(NULL);
    GtkWidget *add = panel_button_new("新建");
    g_signal_connect(prev_button, "clicked", G_CALLBACK(page_cb), GINT_TO_POINTER(-1));
    g_signal_connect(next_button, "clicked", G_CALLBACK(page_cb), GINT_TO_POINTER(1));
    g_signal_connect(add, "clicked", G_CALLBACK(new_cb), NULL);
    gtk_box_pack_start(GTK_BOX(header), prev_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), page_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), next_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), add, FALSE, FALSE, 0);
    rows_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING);
    empty_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(empty_label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(empty_label), TRUE);
    gtk_box_pack_start(GTK_BOX(page), rows_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), empty_label, FALSE, FALSE, 0);
    return page;
}

static GtkWidget * build_form_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING);
    form_title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(form_title), 0);
    PangoAttrList *bold = pango_attr_list_new();
    pango_attr_list_insert(bold, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(form_title), bold);
    pango_attr_list_unref(bold);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), SPACING);
    gtk_grid_set_column_spacing(GTK_GRID(grid), SPACING * 2);
    entry_name = add_field(grid, 0, "名字", "可不填，不填就显示按键");
    entry_keys = add_field(grid, 1, "按键", "例如 C-u 或 C-Space c");
    keys_said = gtk_label_new(KEYS_HINT);
    gtk_label_set_xalign(GTK_LABEL(keys_said), 0);
    gtk_label_set_line_wrap(GTK_LABEL(keys_said), TRUE);
    gtk_grid_attach(GTK_GRID(grid), keys_said, 1, 2, 1, 1);
    g_signal_connect(entry_keys, "changed", G_CALLBACK(keys_changed_cb), NULL);

    GtkWidget *visibility_title = gtk_label_new("显示时机");
    gtk_label_set_xalign(GTK_LABEL(visibility_title), 0);
    GtkWidget *visibility_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING);
    gtk_box_set_homogeneous(GTK_BOX(visibility_box), TRUE);
    for (guint i = 0; i < G_N_ELEMENTS(visibility_labels); i++) {
        GtkWidget *button = panel_button_new(visibility_labels[i]);
        GtkLabel *label = GTK_LABEL(gtk_bin_get_child(GTK_BIN(button)));
        gtk_label_set_line_wrap(label, TRUE);
        gtk_label_set_justify(label, GTK_JUSTIFY_CENTER);
        visibility_buttons[i] = button;
        g_signal_connect(button, "clicked", G_CALLBACK(visibility_clicked_cb), GUINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(visibility_box), button, TRUE, TRUE, 0);
    }
    gtk_grid_attach(GTK_GRID(grid), visibility_title, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), visibility_box, 1, 3, 1, 1);

    // a prefix (tmux's): tapped while previewing, the keyboard comes out
    // for the key that follows it, then goes away again
    GtkWidget *prefix_title = gtk_label_new("前缀");
    gtk_label_set_xalign(GTK_LABEL(prefix_title), 0);
    keyboard_switch = panel_button_new("预览时，发完弹出键盘按一个键");
    g_signal_connect(keyboard_switch, "clicked", G_CALLBACK(keyboard_switch_cb), NULL);
    gtk_grid_attach(GTK_GRID(grid), prefix_title, 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), keyboard_switch, 1, 4, 1, 1);
    g_signal_connect(entry_name, "activate", G_CALLBACK(entry_activate_cb), entry_keys);
    g_signal_connect(entry_keys, "activate", G_CALLBACK(entry_activate_cb), NULL);

    form_message = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(form_message), 0);
    gtk_label_set_line_wrap(GTK_LABEL(form_message), TRUE);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING);
    gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
    GtkWidget *save = panel_button_new("保存");
    delete_button = panel_button_new("删除");
    GtkWidget *cancel = panel_button_new("取消");
    g_signal_connect(save, "clicked", G_CALLBACK(save_cb), NULL);
    g_signal_connect(delete_button, "clicked", G_CALLBACK(delete_cb), NULL);
    g_signal_connect(cancel, "clicked", G_CALLBACK(show_list), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), save, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), delete_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), cancel, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(page), form_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), form_message, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    return page;
}

/** Add the list and editor to the shortcuts tab. */
void shortpage_init(void) {
    list_page = build_list_page();
    form_page = build_form_page();
    panel_add_tab(PANEL_TAB_SHORTCUTS, list_page, refresh_list);
    panel_add_page(form_page, TRUE, PANEL_TAB_SHORTCUTS);
}
