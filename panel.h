/* panel.h
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

#ifndef panel_h
#define panel_h

#include <gtk/gtk.h>

/** Widget name of the panel, for styling */
#define PANEL_NAME "ktermPanel"
/** Widget name of the panel's buttons, for styling */
#define PANEL_BUTTON_NAME "ktermPanelButton"
/** Widget name of the back button of the panel's pages */
#define PANEL_BACK_NAME "ktermPanelBack"
/** Widget name of the settings page's rows, and of the lines between them */
#define PANEL_SETTINGS_NAME "ktermSettings"
#define PANEL_LINE_NAME "ktermSettingLine"
/** Space around and between the panel's parts, in pixels */
#define PANEL_SPACING 12

typedef enum {
    PANEL_TAB_NONE,
    PANEL_TAB_CONNECTIONS,
    PANEL_TAB_SETTINGS,
    PANEL_TAB_SHORTCUTS,
    PANEL_TAB_TERMINALS,
    PANEL_TAB_COUNT
} PanelTab;

GtkWidget * panel_new(GtkWidget *overlay);
void panel_add_tab(PanelTab tab, GtkWidget *page, void (*refresh)(void));
void panel_add_page(GtkWidget *page, gboolean typing, PanelTab tab);
void panel_show_page(GtkWidget *page);
GtkWidget * panel_page_header(GtkWidget *page, const gchar *title, GtkWidget *parent);
void panel_add_setting(const gchar *title, const gchar *hint, GtkWidget *control);
GtkWidget * panel_button_new(const gchar *label);
void panel_clear_box(GtkWidget *box);
void panel_open(void);
void panel_open_page(GtkWidget *page);
/** Open a page in a centered dialog without the panel's navigation. */
void panel_open_dialog(GtkWidget *page);
void panel_close(void);
gboolean panel_is_open(void);
gboolean panel_is_dialog(void);
/** Get the panel's fully opaque bounds relative to the requested widget. */
gboolean panel_opaque_area(GtkWidget *relative_to, GdkRectangle *area);
gboolean panel_showing(GtkWidget *page);
gboolean panel_wants_keys(void);

#endif /* panel_h */
