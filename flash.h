/* flash.h
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

#ifndef flash_h
#define flash_h

#include <gtk/gtk.h>

/** Pages turned and swipes shown as a full refresh (conf->full_refresh):
 *  none... */
#define FLASH_NEVER 0
/** ...every one... */
#define FLASH_ALWAYS 1
/** ...or those after the terminal has been written over since the last,
 *  by other changes */
#define FLASH_WRITTEN_OVER 2
/** By default */
#define FLASH_DEFAULT FLASH_WRITTEN_OVER

void flash_init(GtkWidget *window, GtkWidget *terminal);
void flash_set_when(guint when);
void flash_next_draw(void);

#endif /* flash_h */
