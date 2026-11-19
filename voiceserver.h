/* voiceserver.h
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

#ifndef voiceserver_h
#define voiceserver_h

#include <gio/gio.h>

/** What a phone asks of the terminal, handed to the main thread */
typedef struct {
    /** Text to type, NULL for a ping (the page saying it is there) */
    gchar *text;
    /** Enter after it */
    gboolean run;
    /** Address of the phone */
    gchar *phone;
    /** For voiceserver_answer(): the worker waits there for what was done
     *  with the text; NULL for a ping */
    gpointer reply;
} VoiceRequest;

/** In the main thread: a request came. Must not keep it; must answer a
 *  request with text, at once or later, with voiceserver_answer(). */
typedef void (*VoiceHandler)(const VoiceRequest *request);

gboolean voiceserver_start(guint16 port, VoiceHandler handler, GError **error);
void voiceserver_stop(void);
gboolean voiceserver_running(void);
void voiceserver_set_token(const gchar *token);
void voiceserver_answer(const gchar *status, gpointer reply);

#endif /* voiceserver_h */
