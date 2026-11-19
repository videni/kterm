/* sshconf.c
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
 * Saved connections are kept where ssh itself looks for them: as Host
 * blocks in ~/.ssh/config. There is no second list to keep in step, and a
 * connection made here also works from the command line, scp and git.
 *
 * The file belongs to the user. Only the lines of the block being changed
 * are touched; comments, other options, wildcard and Match blocks stay as
 * they are.
 *
 * A machine named in the local network ("vidy-macbookpro.local") is
 * reached through kterm-mdns (ProxyCommand), which finds its address
 * wherever the router put it this time. That line comes and goes with the
 * name; a ProxyCommand of the user's own stays.
 */

#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#include "sshconf.h"

#define SSHCONF_ERROR g_quark_from_static_string("kterm-sshconf")
#define NEW_OPTION_INDENT "    "

/**
 * Create a connection description
 * @param name Name
 * @param host Address
 * @param user User name
 * @param port Port
 * @param key Key file (IdentityFile), NULL or empty for none
 * @return Newly allocated structure, free with ssh_host_free()
 */
SshHost * ssh_host_new(const gchar *name, const gchar *host, const gchar *user, const gchar *port,
                       const gchar *key) {
    SshHost *item = g_new0(SshHost, 1);
    item->name = g_strdup(name ? name : "");
    item->host = g_strdup(host ? host : "");
    item->user = g_strdup(user ? user : "");
    item->port = g_strdup(port ? port : "");
    item->key = g_strdup(key ? key : "");
    return item;
}

/**
 * Free a connection description
 * @param host Structure
 */
void ssh_host_free(SshHost *host) {
    if (!host) {
        return;
    }
    g_free(host->name);
    g_free(host->host);
    g_free(host->user);
    g_free(host->port);
    g_free(host->key);
    g_free(host);
}

/**
 * @return Path of the user's ssh client configuration, to be freed
 */
gchar * sshconf_default_path(void) {
    return g_build_filename(g_get_home_dir(), ".ssh", "config", NULL);
}

/**
 * Split a configuration line into keyword and arguments.
 * ssh accepts "Keyword value", "Keyword=value" and "Keyword = value".
 * @param line Line
 * @param keyword Location for the lower case keyword, to be freed; NULL for
 *        blank lines and comments
 * @param args Location for a pointer into line where the arguments start
 */
static void split_line(const gchar *line, gchar **keyword, const gchar **args) {
    *keyword = NULL;
    *args = NULL;
    const gchar *p = line;
    while (*p == ' ' || *p == '\t') { p++; }
    if (*p == '\0' || *p == '#' || *p == '\r' || *p == '\n') {
        return;
    }
    const gchar *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '=' && *p != '\r' && *p != '\n') { p++; }
    *keyword = g_ascii_strdown(start, p - start);
    while (*p == ' ' || *p == '\t') { p++; }
    if (*p == '=') {
        p++;
        while (*p == ' ' || *p == '\t') { p++; }
    }
    *args = p;
}

/**
 * Take the first argument off an argument string
 * @param args Arguments; moved past the argument and following blanks
 * @return Argument without quotes, to be freed; NULL when there is none
 */
static gchar * next_arg(const gchar **args) {
    const gchar *p = *args;
    while (*p == ' ' || *p == '\t') { p++; }
    if (*p == '\0' || *p == '\r' || *p == '\n') {
        *args = p;
        return NULL;
    }
    GString *arg = g_string_new(NULL);
    gboolean quoted = FALSE;
    for (; *p && *p != '\r' && *p != '\n'; p++) {
        if (*p == '"') {
            quoted = !quoted;
        } else if (!quoted && (*p == ' ' || *p == '\t')) {
            break;
        } else {
            g_string_append_c(arg, *p);
        }
    }
    while (*p == ' ' || *p == '\t') { p++; }
    *args = p;
    return g_string_free(arg, FALSE);
}

/**
 * @param line Line
 * @param wanted Lower case keyword
 * @return First argument of the line if it has that keyword, else NULL
 */
static gchar * line_value(const gchar *line, const gchar *wanted) {
    gchar *keyword = NULL;
    const gchar *args = NULL;
    split_line(line, &keyword, &args);
    gchar *value = NULL;
    if (keyword && !strcmp(keyword, wanted)) {
        value = next_arg(&args);
        if (!value) {
            value = g_strdup("");
        }
    }
    g_free(keyword);
    return value;
}

/**
 * @param host HostName
 * @return TRUE for a name of the local network, found by multicast DNS
 */
gboolean sshconf_is_local(const gchar *host) {
    return host && (g_str_has_suffix(host, ".local") || g_str_has_suffix(host, ".local."));
}

/**
 * @param lines Configuration lines
 * @param first Index of the Host line
 * @param end Index after the block
 * @param wanted Lower case keyword
 * @return First argument of the option in the block, NULL if it has none
 */
static gchar * block_value(GPtrArray *lines, guint first, guint end, const gchar *wanted) {
    for (guint i = first + 1; i < end; i++) {
        gchar *value = line_value(g_ptr_array_index(lines, i), wanted);
        if (value) {
            return value;
        }
    }
    return NULL;
}

/**
 * @param line Line
 * @return True for a line that starts a new section (Host or Match)
 */
static gboolean starts_section(const gchar *line) {
    gchar *keyword = NULL;
    const gchar *args = NULL;
    split_line(line, &keyword, &args);
    const gboolean section = keyword && (!strcmp(keyword, "host") || !strcmp(keyword, "match"));
    g_free(keyword);
    return section;
}

/**
 * @param line Line
 * @return True for a line with a keyword, as opposed to blank and comment lines
 */
static gboolean has_keyword(const gchar *line) {
    gchar *keyword = NULL;
    const gchar *args = NULL;
    split_line(line, &keyword, &args);
    const gboolean result = (keyword != NULL);
    g_free(keyword);
    return result;
}

/**
 * @param line Line
 * @return True for an empty line or one of blanks only
 */
static gboolean is_blank(const gchar *line) {
    return line[strspn(line, " \t\r")] == '\0';
}

/**
 * Check one field for characters that are safe in the configuration file and
 * on a command line: letters, digits and a few punctuation marks. A leading
 * "-" is refused because ssh would read the value as an option.
 * @param value Text
 * @param extra Punctuation allowed besides letters and digits
 * @return True if the text is not empty and safe
 */
static gboolean is_plain(const gchar *value, const gchar *extra) {
    if (!value || !*value || *value == '-') {
        return FALSE;
    }
    for (const gchar *p = value; *p; p++) {
        if (!g_ascii_isalnum(*p) && !strchr(extra, *p)) {
            return FALSE;
        }
    }
    return TRUE;
}

/**
 * Check what the user typed for a connection
 * @param host Connection
 * @param error Location for the complaint, in the user's language
 * @return True if the connection can be saved
 */
gboolean sshconf_validate(const SshHost *host, GError **error) {
    const gchar *complaint = NULL;
    if (!*host->name) {
        complaint = "名称不能为空";
    } else if (!is_plain(host->name, "._-") || strlen(host->name) > 64) {
        complaint = "名称只能用字母、数字和 . _ -";
    } else if (!*host->host) {
        complaint = "地址不能为空";
    } else if (!is_plain(host->host, "._-:")) {
        // ":" is for IPv6 addresses; "%" (an IPv6 zone) is left out, ssh
        // would read it as one of its % tokens
        complaint = "地址只能是主机名或 IP";
    } else if (*host->user && !is_plain(host->user, "._-")) {
        complaint = "用户名只能用字母、数字和 . _ -";
    } else if (*host->port) {
        gchar *end = NULL;
        const guint64 port = g_ascii_strtoull(host->port, &end, 10);
        if (*end || !g_ascii_isdigit(*host->port) || port < 1 || port > 65535) {
            complaint = "端口应为 1-65535";
        }
    }
    if (complaint) {
        g_set_error_literal(error, SSHCONF_ERROR, 1, complaint);
        return FALSE;
    }
    return TRUE;
}

/**
 * Read the configuration file as lines, without line ends
 * @param path File path
 * @param error Location for an error
 * @return Array of strings; empty if the file does not exist; NULL on error
 */
static GPtrArray * read_lines(const gchar *path, GError **error) {
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    gchar *content = NULL;
    GError *read_error = NULL;
    if (!g_file_get_contents(path, &content, NULL, &read_error)) {
        if (g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_error_free(read_error);
            return lines;
        }
        g_propagate_error(error, read_error);
        g_ptr_array_unref(lines);
        return NULL;
    }
    gchar **split = g_strsplit(content, "\n", -1);
    guint count = g_strv_length(split);
    // the text after the last newline is not a line unless something is there
    if (count && !*split[count - 1]) {
        count--;
    }
    for (guint i = 0; i < count; i++) {
        // a file with Windows line ends is written back with plain ones
        const gsize len = strlen(split[i]);
        g_ptr_array_add(lines, g_strndup(split[i], (len && split[i][len - 1] == '\r') ? len - 1 : len));
    }
    g_strfreev(split);
    g_free(content);
    return lines;
}

/**
 * Write the lines back. The file holds host names and key locations, so it
 * is kept private, as ssh itself requires.
 * @param path File path
 * @param lines Array of strings
 * @param error Location for an error
 * @return True on success
 */
static gboolean write_lines(const gchar *path, GPtrArray *lines, GError **error) {
    gchar *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, SSHCONF_ERROR, 2, "无法创建 %s", dir);
        g_free(dir);
        return FALSE;
    }
    g_free(dir);
    GString *content = g_string_new(NULL);
    for (guint i = 0; i < lines->len; i++) {
        g_string_append(content, g_ptr_array_index(lines, i));
        g_string_append_c(content, '\n');
    }
    // written to a temporary file and renamed: never a half-written config;
    // through a symbolic link, the file it points to is the one replaced
    char *real = realpath(path, NULL);
    const gchar *target = real ? real : path;
    gboolean saved = g_file_set_contents_full(target, content->str, (gssize) content->len,
                                              G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
    g_string_free(content, TRUE);
    // GLib keeps the mode of a file that existed; ssh refuses a config that
    // others may write to ("Bad owner or permissions"), so it is set anyway.
    if (saved && g_chmod(target, 0600) != 0) {
        g_set_error(error, SSHCONF_ERROR, 2, "无法设置 %s 的权限", target);
        saved = FALSE;
    }
    free(real);
    return saved;
}

/**
 * @param line Line
 * @return Name of the connection a Host line starts, to be freed. NULL if
 *         the line is not a Host line or its first pattern is not a plain
 *         name (wildcards, negation): those blocks are rules, not places to
 *         connect to.
 */
static gchar * host_line_name(const gchar *line) {
    gchar *keyword = NULL;
    const gchar *args = NULL;
    split_line(line, &keyword, &args);
    gchar *name = NULL;
    if (keyword && !strcmp(keyword, "host")) {
        name = next_arg(&args);
        if (name && !is_plain(name, "._-")) {
            g_free(name);
            name = NULL;
        }
    }
    g_free(keyword);
    return name;
}

/**
 * Find the block of a connection
 * @param lines Configuration lines
 * @param name Connection name
 * @param first Location for the index of the Host line
 * @param end Location for the index after the block's last option line.
 *        Blank lines and comments between that option and the next section
 *        are left out: a comment there usually describes what follows.
 * @return True if found
 */
static gboolean find_block(GPtrArray *lines, const gchar *name, guint *first, guint *end) {
    for (guint i = 0; i < lines->len; i++) {
        gchar *line_name = host_line_name(g_ptr_array_index(lines, i));
        const gboolean match = line_name && !strcmp(line_name, name);
        g_free(line_name);
        if (!match) {
            continue;
        }
        guint last = i;
        for (guint j = i + 1; j < lines->len && !starts_section(g_ptr_array_index(lines, j)); j++) {
            if (has_keyword(g_ptr_array_index(lines, j))) {
                last = j;
            }
        }
        *first = i;
        *end = last + 1;
        return TRUE;
    }
    return FALSE;
}

/**
 * List the saved connections
 * @param path Configuration file path
 * @param error Location for an error
 * @return Array of SshHost in file order, to be unreferenced; NULL on error
 */
GPtrArray * sshconf_list(const gchar *path, GError **error) {
    GPtrArray *lines = read_lines(path, error);
    if (!lines) {
        return NULL;
    }
    GPtrArray *hosts = g_ptr_array_new_with_free_func((GDestroyNotify) ssh_host_free);
    for (guint i = 0; i < lines->len; i++) {
        gchar *name = host_line_name(g_ptr_array_index(lines, i));
        if (!name) {
            continue;
        }
        // ssh takes the first value it meets for each option
        gchar *host = NULL, *user = NULL, *port = NULL, *key = NULL;
        for (guint j = i + 1; j < lines->len && !starts_section(g_ptr_array_index(lines, j)); j++) {
            const gchar *line = g_ptr_array_index(lines, j);
            if (!host) { host = line_value(line, "hostname"); }
            if (!user) { user = line_value(line, "user"); }
            if (!port) { port = line_value(line, "port"); }
            if (!key) { key = line_value(line, "identityfile"); }
        }
        g_ptr_array_add(hosts, ssh_host_new(name, (host && *host) ? host : name, user, port, key));
        g_free(key);
        g_free(name);
        g_free(host);
        g_free(user);
        g_free(port);
    }
    g_ptr_array_unref(lines);
    return hosts;
}

/**
 * Set, add or remove one option of a block
 * @param lines Configuration lines
 * @param first Index of the Host line
 * @param end Location of the index after the block, updated
 * @param keyword Option name as it should be written
 * @param value New value; empty removes the option
 * @param indent Indentation for a new line
 * @param insert_at Location of the index where a missing option is added, updated
 */
static void set_option(GPtrArray *lines, guint first, guint *end, const gchar *keyword,
                       const gchar *value, const gchar *indent, guint *insert_at) {
    gchar *wanted = g_ascii_strdown(keyword, -1);
    gboolean found = FALSE;
    for (guint i = first + 1; i < *end; i++) {
        gchar *current = line_value(g_ptr_array_index(lines, i), wanted);
        if (!current) {
            continue;
        }
        g_free(current);
        if (found || !*value) {
            // an emptied option goes; so do repeats, which ssh ignores anyway
            // and which would otherwise look like the value still in force
            g_ptr_array_remove_index(lines, i);
            (*end)--;
            if (*insert_at > i) { (*insert_at)--; }
            i--;
        } else {
            const gchar *old = g_ptr_array_index(lines, i);
            const gsize indent_len = strspn(old, " \t");
            gchar *line = g_strdup_printf("%.*s%s %s", (gint) indent_len, old, keyword, value);
            g_free(lines->pdata[i]);
            lines->pdata[i] = line;
            found = TRUE;
            *insert_at = i + 1;
        }
    }
    g_free(wanted);
    if (!found && *value) {
        g_ptr_array_insert(lines, (gint) *insert_at, g_strdup_printf("%s%s %s", indent, keyword, value));
        (*insert_at)++;
        (*end)++;
    }
}

/**
 * @param lines Lines of the file
 * @param name Connection name
 * @return True if any Host line has the name among its patterns: ssh would
 *         take that block for it, not a new one further down
 */
static gboolean name_taken(GPtrArray *lines, const gchar *name) {
    gboolean taken = FALSE;
    for (guint i = 0; i < lines->len && !taken; i++) {
        gchar *keyword = NULL;
        const gchar *args = NULL;
        split_line(g_ptr_array_index(lines, i), &keyword, &args);
        if (keyword && !strcmp(keyword, "host")) {
            gchar *pattern = NULL;
            while (!taken && (pattern = next_arg(&args))) {
                taken = !strcmp(pattern, name);
                g_free(pattern);
            }
        }
        g_free(keyword);
    }
    return taken;
}

/**
 * Add a connection or change an existing one
 * @param path Configuration file path
 * @param old_name Name of the connection being edited, NULL for a new one
 * @param host New values
 * @param error Location for an error
 * @return True on success
 */
gboolean sshconf_save(const gchar *path, const gchar *old_name, const SshHost *host, GError **error) {
    if (!sshconf_validate(host, error)) {
        return FALSE;
    }
    GPtrArray *lines = read_lines(path, error);
    if (!lines) {
        return FALSE;
    }
    guint first = 0, end = 0;
    const gboolean renamed = !old_name || strcmp(old_name, host->name);
    if (renamed && name_taken(lines, host->name)) {
        g_set_error(error, SSHCONF_ERROR, 3, "已有名为 %s 的连接", host->name);
        g_ptr_array_unref(lines);
        return FALSE;
    }
    if (old_name) {
        if (!find_block(lines, old_name, &first, &end)) {
            g_set_error(error, SSHCONF_ERROR, 4, "配置里找不到 %s", old_name);
            g_ptr_array_unref(lines);
            return FALSE;
        }
        if (renamed) {
            // only the first pattern is the name; further patterns stay
            const gchar *old_line = g_ptr_array_index(lines, first);
            gchar *keyword = NULL;
            const gchar *args = NULL;
            split_line(old_line, &keyword, &args);
            g_free(keyword);
            const gchar *rest = args;
            g_free(next_arg(&rest));
            const gsize indent_len = strspn(old_line, " \t");
            gchar *line = g_strdup_printf("%.*sHost %s%s%s", (gint) indent_len, old_line,
                                          host->name, *rest ? " " : "", rest);
            g_free(lines->pdata[first]);
            lines->pdata[first] = line;
        }
    } else {
        // ssh uses the first value it finds for each option, so a new
        // connection has to come before the blocks of general rules
        // ("Host *", "Match ..."), or their User and Port would win.
        guint at = lines->len;
        for (guint i = 0; i < lines->len; i++) {
            const gchar *line = g_ptr_array_index(lines, i);
            gchar *name = host_line_name(line);
            const gboolean rule = starts_section(line) && !name;
            g_free(name);
            if (rule) {
                at = i;
                break;
            }
        }
        // comments directly above that block describe it and stay with it
        while (at > 0 && at < lines->len) {
            const gchar *above = g_ptr_array_index(lines, at - 1);
            if (is_blank(above) || has_keyword(above)) {
                break;
            }
            at--;
        }
        if (at > 0 && !is_blank(g_ptr_array_index(lines, at - 1))) {
            g_ptr_array_insert(lines, (gint) at, g_strdup(""));
            at++;
        }
        first = at;
        g_ptr_array_insert(lines, (gint) first, g_strdup_printf("Host %s", host->name));
        end = first + 1;
        if (end < lines->len) {
            // a blank line between the new block and what follows it
            g_ptr_array_insert(lines, (gint) end, g_strdup(""));
        }
    }
    // new options are indented like the ones already in the block
    gchar *indent = g_strdup(NEW_OPTION_INDENT);
    for (guint i = first + 1; i < end; i++) {
        const gchar *line = g_ptr_array_index(lines, i);
        if (has_keyword(line)) {
            g_free(indent);
            indent = g_strndup(line, strspn(line, " \t"));
            break;
        }
    }
    guint insert_at = first + 1;
    set_option(lines, first, &end, "HostName", host->host, indent, &insert_at);
    gchar *proxy = block_value(lines, first, end, "proxycommand");
    if (sshconf_is_local(host->host)) {
        set_option(lines, first, &end, "ProxyCommand", SSHCONF_MDNS_PROXY, indent, &insert_at);
    } else if (proxy && strstr(proxy, "kterm-mdns")) {
        set_option(lines, first, &end, "ProxyCommand", "", indent, &insert_at);
    }
    g_free(proxy);
    set_option(lines, first, &end, "User", host->user, indent, &insert_at);
    set_option(lines, first, &end, "Port", host->port, indent, &insert_at);
    // the key chosen, and only it: ssh would otherwise try every key it
    // holds first, and some servers count each as a failed login
    set_option(lines, first, &end, "IdentityFile", host->key, indent, &insert_at);
    set_option(lines, first, &end, "IdentitiesOnly", *host->key ? "yes" : "", indent, &insert_at);
    g_free(indent);
    const gboolean saved = write_lines(path, lines, error);
    g_ptr_array_unref(lines);
    return saved;
}

/**
 * Remove a connection
 * @param path Configuration file path
 * @param name Connection name
 * @param error Location for an error
 * @return True on success
 */
gboolean sshconf_delete(const gchar *path, const gchar *name, GError **error) {
    GPtrArray *lines = read_lines(path, error);
    if (!lines) {
        return FALSE;
    }
    guint first = 0, end = 0;
    if (!find_block(lines, name, &first, &end)) {
        g_set_error(error, SSHCONF_ERROR, 4, "配置里找不到 %s", name);
        g_ptr_array_unref(lines);
        return FALSE;
    }
    g_ptr_array_remove_range(lines, first, end - first);
    // do not leave two blank lines where the block was
    if (first < lines->len && is_blank(g_ptr_array_index(lines, first))
        && (first == 0 || is_blank(g_ptr_array_index(lines, first - 1)))) {
        g_ptr_array_remove_index(lines, first);
    }
    while (lines->len && is_blank(g_ptr_array_index(lines, lines->len - 1))) {
        g_ptr_array_remove_index(lines, lines->len - 1);
    }
    const gboolean saved = write_lines(path, lines, error);
    g_ptr_array_unref(lines);
    return saved;
}

/** The key kterm makes when there is none, in ~/.ssh */
#define NEW_KEY_NAME "id_ed25519_kindle"

/**
 * @param key Key file as written in the configuration ("~/.ssh/name")
 * @return Its path, newly allocated
 */
static gchar * key_path(const gchar *key) {
    if (g_str_has_prefix(key, "~/")) {
        return g_build_filename(g_get_home_dir(), key + 2, NULL);
    }
    return g_strdup(key);
}

static gint compare_strings(gconstpointer a, gconstpointer b) {
    return strcmp(*(const gchar * const *) a, *(const gchar * const *) b);
}

/**
 * The keys there are to log in with: files of ~/.ssh with a public key
 * (NAME.pub) next to them
 * @return Array of "~/.ssh/NAME", sorted, to be unreferenced
 */
GPtrArray * sshconf_keys(void) {
    GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);
    gchar *dir_path = g_build_filename(g_get_home_dir(), ".ssh", NULL);
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    const gchar *entry = NULL;
    while (dir && (entry = g_dir_read_name(dir))) {
        if (!g_str_has_suffix(entry, ".pub")) {
            continue;
        }
        gchar *name = g_strndup(entry, strlen(entry) - 4);
        gchar *private_path = g_build_filename(dir_path, name, NULL);
        if (g_file_test(private_path, G_FILE_TEST_IS_REGULAR)) {
            g_ptr_array_add(keys, g_strconcat("~/.ssh/", name, NULL));
        }
        g_free(private_path);
        g_free(name);
    }
    if (dir) {
        g_dir_close(dir);
    }
    g_free(dir_path);
    g_ptr_array_sort(keys, compare_strings);
    return keys;
}

/**
 * Make a key to log in with, without a passphrase: ~/.ssh/id_ed25519_kindle,
 * or with a number after it if that is taken
 * @param error Location for an error
 * @return The key as written in the configuration ("~/.ssh/NAME"), newly
 *         allocated; NULL on error
 */
gchar * sshconf_new_key(GError **error) {
    gchar *dir_path = g_build_filename(g_get_home_dir(), ".ssh", NULL);
    if (g_mkdir_with_parents(dir_path, 0700)) {
        g_set_error(error, SSHCONF_ERROR, 2, "无法创建 %s", dir_path);
        g_free(dir_path);
        return NULL;
    }
    gchar *name = g_strdup(NEW_KEY_NAME);
    gchar *path = g_build_filename(dir_path, name, NULL);
    for (guint n = 2; g_file_test(path, G_FILE_TEST_EXISTS); n++) {
        g_free(name);
        g_free(path);
        name = g_strdup_printf("%s_%u", NEW_KEY_NAME, n);
        path = g_build_filename(dir_path, name, NULL);
    }
    gchar *argv[] = { "ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", "kindle", "-f", path, NULL };
    gchar *errors = NULL;
    // whether the key is there tells, whatever ssh-keygen said
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, &errors,
                 NULL, NULL);
    gchar *pub = g_strconcat(path, ".pub", NULL);
    gchar *key = NULL;
    if (g_file_test(path, G_FILE_TEST_EXISTS) && g_file_test(pub, G_FILE_TEST_EXISTS)) {
        key = g_strconcat("~/.ssh/", name, NULL);
    } else {
        g_set_error(error, SSHCONF_ERROR, 5, "没能生成密钥：%s", errors && *errors ? g_strstrip(errors) : "ssh-keygen 没有运行");
    }
    g_free(errors);
    g_free(pub);
    g_free(path);
    g_free(name);
    g_free(dir_path);
    return key;
}

/**
 * @param key Key as written in the configuration ("~/.ssh/NAME")
 * @return Its public key, the line to put in authorized_keys, newly
 *         allocated; NULL if there is none
 */
gchar * sshconf_public_key(const gchar *key) {
    gchar *path = key_path(key);
    gchar *pub_path = g_strconcat(path, ".pub", NULL);
    gchar *text = NULL;
    if (g_file_get_contents(pub_path, &text, NULL, NULL)) {
        g_strstrip(text);
        // one line, and nothing a shell would take apart inside quotes
        if (!*text || strchr(text, '\n') || strchr(text, '\'')) {
            g_free(text);
            text = NULL;
        }
    }
    g_free(pub_path);
    g_free(path);
    return text;
}
