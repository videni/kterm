/* sshconf.h
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

#ifndef sshconf_h
#define sshconf_h

#include <glib.h>

/**
 * One connection: a "Host" block of the OpenSSH client configuration
 */
typedef struct {
    gchar *name; /** First pattern of the Host line, what "ssh <name>" takes */
    gchar *host; /** HostName; the name itself when the block has none */
    gchar *user; /** User, may be empty */
    gchar *port; /** Port, may be empty */
} SshHost;

SshHost * ssh_host_new(const gchar *name, const gchar *host, const gchar *user, const gchar *port);
void ssh_host_free(SshHost *host);
gchar * sshconf_default_path(void);
GPtrArray * sshconf_list(const gchar *path, GError **error);
gboolean sshconf_validate(const SshHost *host, GError **error);
gboolean sshconf_save(const gchar *path, const gchar *old_name, const SshHost *host, GError **error);
gboolean sshconf_delete(const gchar *path, const gchar *name, GError **error);

#endif /* sshconf_h */
