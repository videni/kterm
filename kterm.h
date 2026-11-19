/* kterm.h
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
 * What kterm.c offers the rest: the terminal, what runs in it, and the
 * keyboard under it.
 */

#ifndef kterm_h
#define kterm_h

#include <sys/types.h>
#include <gtk/gtk.h>

/* the keyboard: built-in or the system one, shown or folded away */
void kterm_toggle_ime(void);
void kterm_hide_keyboard(void);
void kterm_use_builtin_keyboard(void);
void kterm_keyboard_relayout(void);
void kterm_set_preview(gboolean on);
gboolean kterm_previewing(void);
void kterm_preview_after_key(void);
void kterm_key_typed(void);
void kterm_keyboard_back_on_tap(void);

/* the terminal and what runs in it */
GtkWidget * kterm_get_terminal(void);
void kterm_set_font_size(guint size);
gboolean kterm_terminal_has_shell(void);
pid_t kterm_terminal_foreground(void);
pid_t kterm_terminal_owner(void);
gchar * kterm_process_name(pid_t pid);
gboolean kterm_process_is(pid_t pid, const gchar *name);
void kterm_type_command(const gchar *command);

/* its history: the lines scrolled off its top */
gboolean kterm_terminal_has_history(void);
gboolean kterm_terminal_scrolled(void);
void kterm_terminal_to_bottom(void);

#endif /* kterm_h */
