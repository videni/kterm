/* panel.c
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
 * The panel opens from the shortcut bar's ☰ button and overlays the terminal
 * without resizing it.
 */

#include <gtk/gtk.h>
#include <string.h>
#include "config.h"
#include "panel.h"
#include "icons.h"

#if GTK_CHECK_VERSION(3,2,0)

#include "kterm.h"

static GtkWidget *root = NULL;
/** Holds the pages, one shown at a time */
static GtkStack *content = NULL;
#if GTK_CHECK_VERSION(3,22,0)
static GtkScrolledWindow *scroll = NULL;
static GtkWidget *dismiss_layer = NULL;
#endif
static GtkWidget *tab_row = NULL;
static GtkWidget *panel_footer = NULL;
static PanelTab selected_tab = PANEL_TAB_CONNECTIONS;
static struct {
    GtkWidget *page;
    GtkWidget *button;
    void (*refresh)(void);
} tabs[PANEL_TAB_COUNT];
/** Rows of the settings page */
static GtkWidget *settings_box = NULL;

#if defined(KINDLE) && GTK_CHECK_VERSION(3,22,0)
static gint opacity_width;
static gint opacity_height;
static gboolean opaque_background;

static void invalidate_panel_opacity(GtkWidget *widget, gpointer data) {
    UNUSED(widget);
    UNUSED(data);
    opacity_width = 0;
}

/** Check the same background and border that GtkEventBox paints. */
static gboolean panel_background_is_opaque(gint width, gint height) {
    GtkStyleContext *style = gtk_widget_get_style_context(root);
    gdouble opacity = 1.0;
    gtk_style_context_get(style, gtk_style_context_get_state(style), "opacity", &opacity, NULL);
    if (opacity < 1.0) {
        return FALSE;
    }

    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
    cairo_t *cr = cairo_create(mask);
    gtk_render_background(style, cr, 0, 0, width, height);
    gtk_render_frame(style, cr, 0, 0, width, height);
    gboolean opaque = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
    cairo_destroy(cr);
    cairo_surface_flush(mask);
    const guchar *row = cairo_image_surface_get_data(mask);
    const gint stride = cairo_image_surface_get_stride(mask);
    for (gint y = 0; opaque && y < height; y++, row += stride) {
        // Equal neighboring pixels and an opaque first pixel imply a solid row.
        opaque = row[0] == 255 && memcmp(row, row + 1, width - 1) == 0;
    }
    cairo_surface_destroy(mask);
    return opaque;
}

gboolean panel_opaque_area(GtkWidget *relative_to, GdkRectangle *area) {
    if (!root || !gtk_widget_get_mapped(root) || panel_is_dialog()
        || gtk_widget_get_scale_factor(root) != 1 || gtk_widget_get_opacity(root) < 1.0
        || !gtk_event_box_get_visible_window(GTK_EVENT_BOX(root)) || gtk_widget_get_app_paintable(root)) {
        return FALSE;
    }
    const gint width = gtk_widget_get_allocated_width(root);
    const gint height = gtk_widget_get_allocated_height(root);
    if (width <= 0 || height <= 0) {
        return FALSE;
    }
    if (width != opacity_width || height != opacity_height) {
        opaque_background = panel_background_is_opaque(width, height);
        opacity_width = width;
        opacity_height = height;
    }
    if (!opaque_background || !gtk_widget_translate_coordinates(root, relative_to, 0, 0, &area->x, &area->y)) {
        return FALSE;
    }
    area->width = width;
    area->height = height;
    return TRUE;
}
#endif

/**
 * Create a button in the panel's style.
 * Panel buttons never take the keyboard focus: keys must keep going to the
 * terminal, or to the field being edited.
 * @param label Button text
 * @return Button widget
 */
GtkWidget * panel_button_new(const gchar *label) {
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_name(button, PANEL_BUTTON_NAME);
    gtk_widget_set_can_focus(button, FALSE);
    return button;
}

static void destroy_cb(GtkWidget *widget, gpointer data) {
    UNUSED(data);
    gtk_widget_destroy(widget);
}

/**
 * Take everything out of a box, to fill it again
 * @param box Container
 */
void panel_clear_box(GtkWidget *box) {
    gtk_container_foreach(GTK_CONTAINER(box), destroy_cb, NULL);
}

/**
 * @return True while the panel is on screen
 */
gboolean panel_is_open(void) {
    return root && gtk_widget_get_visible(root);
}

gboolean panel_showing(GtkWidget *page) {
    return page && panel_is_open() && page == gtk_stack_get_visible_child(content);
}

/**
 * @return True while the page shown is a form the keyboard types into
 */
static gboolean page_is_typing(void) {
    GtkWidget *page = gtk_stack_get_visible_child(content);
    return page && g_object_get_data(G_OBJECT(page), "typing") != NULL;
}

/**
 * @return True while the keyboard is typing into the panel's form, so that
 *         nothing typed should reach the terminal
 */
gboolean panel_wants_keys(void) {
    return panel_is_open() && page_is_typing();
}

/** Send keyboard focus to the terminal when the panel is not editing. */
static void set_terminal_focus(gboolean enabled) {
    GtkWidget *terminal = kterm_get_terminal();
    if (terminal) {
        gtk_widget_set_can_focus(terminal, enabled);
        if (enabled) {
            gtk_widget_grab_focus(terminal);
        }
    }
}

gboolean panel_is_dialog(void) {
    return panel_is_open()
        && gtk_style_context_has_class(gtk_widget_get_style_context(root), "dialog");
}

#if GTK_CHECK_VERSION(3,22,0)
static void sync_dismiss_layer(void) {
    gtk_widget_set_visible(dismiss_layer, panel_is_dialog());
}
#endif

static void show_page(GtkWidget *page, gboolean dialog) {
    const PanelTab tab = dialog ? PANEL_TAB_NONE
            : GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "panel-tab"));
    if (tab != PANEL_TAB_NONE && tab != selected_tab) {
        gtk_style_context_remove_class(gtk_widget_get_style_context(tabs[selected_tab].button), "selected");
        selected_tab = tab;
        gtk_style_context_add_class(gtk_widget_get_style_context(tabs[tab].button), "selected");
    }
    if (tab != PANEL_TAB_NONE && page == tabs[tab].page && tabs[tab].refresh) {
        tabs[tab].refresh();
    }
    gtk_widget_set_visible(tab_row, tab != PANEL_TAB_NONE);
    gtk_widget_set_visible(panel_footer, tab != PANEL_TAB_NONE);
    gtk_stack_set_visible_child(content, page);
    GtkStyleContext *style = gtk_widget_get_style_context(root);
    if (dialog) {
        gtk_style_context_add_class(style, "dialog");
    } else {
        gtk_style_context_remove_class(style, "dialog");
    }
#if GTK_CHECK_VERSION(3,22,0)
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(scroll), 0);
    sync_dismiss_layer();
    gtk_widget_queue_resize(root);
#endif
    // While a form is shown, keys such as Tab must not move the focus to
    // the terminal, where the rest of what is typed would run.
    set_terminal_focus(!dialog && !page_is_typing());
}

/** Show a page within the panel. */
void panel_show_page(GtkWidget *page) {
    show_page(page, FALSE);
}

/**
 * Add a page
 * @param page Page widget to include in the panel
 * @param typing TRUE while the panel receives keyboard input
 * @param tab Tab containing the page, or PANEL_TAB_NONE for a standalone page
 */
void panel_add_page(GtkWidget *page, gboolean typing, PanelTab tab) {
    g_object_set_data(G_OBJECT(page), "panel-tab", GINT_TO_POINTER(tab));
    if (typing) {
        g_object_set_data(G_OBJECT(page), "typing", GINT_TO_POINTER(1));
    }
    gtk_container_add(GTK_CONTAINER(content), page);
    gtk_widget_show_all(page);
}

/** Register a tab's starting page and its optional refresh before display. */
void panel_add_tab(PanelTab tab, GtkWidget *page, void (*refresh)(void)) {
    tabs[tab].page = page;
    tabs[tab].refresh = refresh;
    gtk_widget_set_margin_top(page, PANEL_SPACING);
    panel_add_page(page, FALSE, tab);
}

/**
 * Hide the panel
 */
void panel_close(void) {
    if (!panel_is_open()) {
        return;
    }
    D printf("panel: close\n");
    gtk_widget_hide(root);
#if GTK_CHECK_VERSION(3,22,0)
    sync_dismiss_layer();
#endif
    set_terminal_focus(TRUE);
}

/**
 * Reopen the panel on the selected tab's starting page.
 */
void panel_open(void) {
    if (!root || panel_is_open()) {
        return;
    }
    panel_open_page(tabs[selected_tab].page);
}

static void open_page(GtkWidget *page, gboolean dialog) {
    show_page(page, dialog);
    gtk_widget_show(root);
#if GTK_CHECK_VERSION(3,22,0)
    sync_dismiss_layer();
#endif
}

void panel_open_page(GtkWidget *page) {
    open_page(page, FALSE);
}

void panel_open_dialog(GtkWidget *page) {
    open_page(page, TRUE);
}

/**
 * Back button callback
 * @param button Calling widget
 * @param parent Page to go back to, NULL for the selected tab
 */
static void back_cb(GtkWidget *button, gpointer parent) {
    UNUSED(button);
    panel_show_page(parent ? GTK_WIDGET(parent) : tabs[selected_tab].page);
}

/**
 * Start a page with its back button and title
 * @param page Page widget
 * @param title Title text
 * @param parent Page the back button goes to, NULL for the selected tab
 * @return Header row, for more buttons on the right
 */
GtkWidget * panel_page_header(GtkWidget *page, const gchar *title, GtkWidget *parent) {
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING);
    // an arrow, as on the Kindle's own pages
    GtkWidget *back = panel_button_new("←");
    gtk_widget_set_name(back, PANEL_BACK_NAME);
    g_signal_connect(back, "clicked", G_CALLBACK(back_cb), parent);
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(header), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);
    return header;
}

static void tab_clicked(GtkButton *button, gpointer data) {
    UNUSED(button);
    const PanelTab tab = GPOINTER_TO_INT(data);
    panel_show_page(tabs[tab].page);
}

static GtkWidget * build_tab_row(void) {
    static const struct {
        const gchar *label;
        const gchar *icon;
    } items[] = {
        [PANEL_TAB_CONNECTIONS] = {"连接", ICON_LINK},
        [PANEL_TAB_SETTINGS] = {"设置", ICON_SETTINGS},
        [PANEL_TAB_SHORTCUTS] = {"快捷键", ICON_SHORTCUT},
#ifdef KINDLE
        [PANEL_TAB_TERMINALS] = {"标签页", ICON_TAB_LIST},
#endif
    };
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(row, "ktermPanelTabs");
    gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
    for (guint tab = PANEL_TAB_CONNECTIONS; tab < G_N_ELEMENTS(items); tab++) {
        GtkWidget *button = panel_button_new(items[tab].label);
        GtkWidget *icon = icon_new(items[tab].icon, 40);
        gtk_widget_set_margin_end(icon, 8);
        gtk_button_set_image(GTK_BUTTON(button), icon);
        gtk_button_set_always_show_image(GTK_BUTTON(button), TRUE);
        tabs[tab].button = button;
        if (tab == selected_tab) {
            gtk_style_context_add_class(gtk_widget_get_style_context(button), "selected");
        }
        g_signal_connect(button, "clicked", G_CALLBACK(tab_clicked), GINT_TO_POINTER(tab));
        gtk_box_pack_start(GTK_BOX(row), button, TRUE, TRUE, 0);
    }
    gtk_widget_show_all(row);
    return row;
}

static GtkWidget * build_footer(void) {
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    GtkWidget *line = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_name(line, PANEL_LINE_NAME);
    gtk_box_pack_start(GTK_BOX(footer), line, FALSE, FALSE, 0);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING);
    GtkWidget *quit = panel_button_new("");
    gtk_button_set_image(GTK_BUTTON(quit), icon_new(ICON_POWER, 32));
    gtk_button_set_always_show_image(GTK_BUTTON(quit), TRUE);
    gtk_widget_set_valign(quit, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(quit, "退出终端");
    atk_object_set_name(gtk_widget_get_accessible(quit), "退出终端");
    gtk_style_context_add_class(gtk_widget_get_style_context(quit), "quit");
    g_signal_connect(quit, "clicked", G_CALLBACK(gtk_main_quit), NULL);
    gtk_box_set_center_widget(GTK_BOX(actions), quit);

    GtkWidget *close = panel_button_new("关闭");
    g_signal_connect(close, "clicked", G_CALLBACK(panel_close), NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(close), "close-panel");
    gtk_widget_set_valign(close, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(actions), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), actions, FALSE, FALSE, 0);
    gtk_widget_show_all(footer);
    return footer;
}

/**
 * Build the settings page; its rows are added with panel_add_setting()
 * @return Page widget
 */
static GtkWidget * build_settings_page(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING);
    // rows closer than the panel's other lists: they all fit above the bar
    settings_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PANEL_SPACING / 2);
    gtk_widget_set_name(settings_box, PANEL_SETTINGS_NAME);
    gtk_box_pack_start(GTK_BOX(page), settings_box, FALSE, FALSE, 0);
    return page;
}

/**
 * Add a row to the settings page: what is set on the left, the widget
 * setting it at the right edge, a line above
 * @param title What is set
 * @param hint Optional explanation below the title
 * @param control Widget that sets it, kept at its own width
 */
void panel_add_setting(const gchar *title, const gchar *hint, GtkWidget *control) {
    GtkWidget *line = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_name(line, PANEL_LINE_NAME);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, PANEL_SPACING * 2);
    GtkWidget *label = gtk_label_new(title);
    if (hint) {
        g_autofree gchar *markup = g_markup_printf_escaped("%s\n<small>%s</small>", title, hint);
        gtk_label_set_markup(GTK_LABEL(label), markup);
    }
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_halign(control, GTK_ALIGN_END);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row), label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings_box), line, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings_box), row, FALSE, FALSE, 0);
    gtk_widget_show_all(line);
    gtk_widget_show_all(row);
}

#if GTK_CHECK_VERSION(3,22,0)
/** Fit overlays above the shortcuts and keyboard. */
static gboolean position_panel(GtkOverlay *overlay, GtkWidget *child, GdkRectangle *area, gpointer data) {
    UNUSED(data);
    if (child != root && child != dismiss_layer) {
        return FALSE;
    }
    GtkWidget *terminal = kterm_get_terminal();
    gint bottom = 0;
    if (!terminal || !gtk_widget_translate_coordinates(terminal, GTK_WIDGET(overlay), 0,
            gtk_widget_get_allocated_height(terminal), NULL, &bottom) || bottom <= 0) {
        return FALSE;
    }
    const gint width = gtk_widget_get_allocated_width(GTK_WIDGET(overlay));
    if (child == dismiss_layer) {
        *area = (GdkRectangle) {0, 0, width, bottom};
        return TRUE;
    }
    const gboolean dialog = panel_is_dialog();
    const gint margin = PANEL_SPACING * 2;
    const gint gap = dialog ? MIN(margin, (bottom - 1) / 2) : 0;
    const gint panel_width = dialog ? CLAMP(width - 2 * margin, 1, 640) : width;
    gint preferred_height;
    gtk_widget_get_preferred_height_for_width(child, panel_width, NULL, &preferred_height);
    const gint height = MIN(preferred_height, bottom - 2 * gap);
    const gint center_y = (gtk_widget_get_allocated_height(GTK_WIDGET(overlay)) - height) / 2;
    const gint y = dialog ? CLAMP(center_y, gap, bottom - gap - height) : 0;
    *area = (GdkRectangle) {(width - panel_width) / 2, y, panel_width, height};
    return TRUE;
}

static gboolean dismiss_dialog(GtkWidget *widget, GdkEventButton *event, gpointer data) {
    UNUSED(widget);
    UNUSED(data);
    if (event->button == GDK_BUTTON_PRIMARY) {
        panel_close();
    }
    return TRUE;
}
#endif

/** Add the initially hidden panel to the terminal overlay. */
GtkWidget * panel_new(GtkWidget *overlay) {
    // an event box has a window of its own: it paints a background over the
    // terminal and keeps touches on the panel from reaching it
    root = gtk_event_box_new();
#if defined(KINDLE) && GTK_CHECK_VERSION(3,22,0)
    g_signal_connect(root, "style-updated", G_CALLBACK(invalidate_panel_opacity), NULL);
#endif
    gtk_widget_set_name(root, PANEL_NAME);
    // at the top, as the Kindle's own menu bar when reading: the keyboard
    // and the newest output, at the foot of the terminal, stay in view
    gtk_widget_set_valign(root, GTK_ALIGN_START);
    gtk_widget_set_halign(root, GTK_ALIGN_FILL);

    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(layout), PANEL_SPACING * 2);
    gtk_container_add(GTK_CONTAINER(root), layout);
    tab_row = build_tab_row();
    gtk_box_pack_start(GTK_BOX(layout), tab_row, FALSE, FALSE, 0);
    content = GTK_STACK(gtk_stack_new());
    gtk_stack_set_homogeneous(content, FALSE);
#if GTK_CHECK_VERSION(3,22,0)
    // Catch outside taps over the terminal and tab bar. Leave the keyboard
    // and shortcuts below this input-only overlay.
    dismiss_layer = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(dismiss_layer), FALSE);
    gtk_widget_add_events(dismiss_layer, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(dismiss_layer, "button-release-event", G_CALLBACK(dismiss_dialog), NULL);
    gtk_widget_set_no_show_all(dismiss_layer, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), dismiss_layer);
#ifdef KINDLE
    kterm_watch_top_swipe(dismiss_layer);
#endif

    scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(NULL, NULL));
    gtk_scrolled_window_set_policy(scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(scroll, FALSE);
    gtk_scrolled_window_set_propagate_natural_height(scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(content));
    gtk_box_pack_start(GTK_BOX(layout), GTK_WIDGET(scroll), TRUE, TRUE, 0);
    gtk_widget_show(GTK_WIDGET(scroll));
    g_signal_connect(overlay, "get-child-position", G_CALLBACK(position_panel), NULL);
#else
    gtk_box_pack_start(GTK_BOX(layout), GTK_WIDGET(content), FALSE, FALSE, 0);
#endif
    panel_footer = build_footer();
    gtk_widget_set_margin_top(panel_footer, PANEL_SPACING);
    gtk_box_pack_start(GTK_BOX(layout), panel_footer, FALSE, FALSE, 0);
    panel_add_tab(PANEL_TAB_SETTINGS, build_settings_page(), NULL);
    gtk_widget_show(GTK_WIDGET(content));
    gtk_widget_show(layout);
    // stays hidden through gtk_widget_show_all() on the window
    gtk_widget_set_no_show_all(root, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), root);
    return root;
}

#else /* GTK+ 2: no overlay to draw the panel in */

void panel_open(void) {}
void panel_open_page(GtkWidget *page) { (void) page; }
void panel_close(void) {}
gboolean panel_is_open(void) { return FALSE; }
gboolean panel_is_dialog(void) { return FALSE; }
gboolean panel_wants_keys(void) { return FALSE; }

#endif
