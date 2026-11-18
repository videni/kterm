/* confedit.h
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

#ifndef confedit_h
#define confedit_h

#include <glib.h>

gchar * confedit_set(const gchar *text, const gchar *key, const gchar *line);
gboolean confedit_save(const gchar *path, const gchar *key, const gchar *line, GError **error);

#endif /* confedit_h */
