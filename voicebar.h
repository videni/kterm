/* voicebar.h
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

#ifndef voicebar_h
#define voicebar_h

#include <gtk/gtk.h>

/** Told what became of text from the phone, see voicebar_receive() */
typedef void (*VoiceReply)(const gchar *status, gpointer data);

void voicebar_init(GtkWidget *terminal);
void voicebar_receive(const gchar *text, gboolean run, VoiceReply reply, gpointer data);
gboolean voicebar_take_enter(void);
void voicebar_clear(void);

#endif /* voicebar_h */
