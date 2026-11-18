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

GtkWidget * panel_new(GtkWidget *terminal);
void panel_add_action(GtkWidget *button, gint position);
void panel_add_page(GtkWidget *page, gboolean typing);
void panel_show_page(GtkWidget *page);
GtkWidget * panel_page_header(GtkWidget *page, const gchar *title, GtkWidget *parent);
GtkWidget * panel_settings_page(void);
void panel_add_bottom(GtkWidget *widget, gboolean system_bar);
void panel_add_setting(const gchar *title, GtkWidget *control);
GtkWidget * panel_button_new(const gchar *label);
void panel_open(void);
void panel_close(void);
gboolean panel_is_open(void);
gboolean panel_wants_keys(void);

#endif /* panel_h */
