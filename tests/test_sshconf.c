/* Tests for sshconf.c: cc $(pkg-config --cflags --libs glib-2.0) -I.. test_sshconf.c ../sshconf.c -o /tmp/test_sshconf */
#include <glib.h>
#include <string.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sshconf.h"

static gchar *path = NULL;

static void put(const gchar *text) {
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

static gchar * get(void) {
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    return text;
}

static void save(const gchar *old, const gchar *name, const gchar *host, const gchar *user, const gchar *port) {
    SshHost *item = ssh_host_new(name, host, user, port);
    GError *error = NULL;
    g_assert_true(sshconf_save(path, old, item, &error));
    g_assert_no_error(error);
    ssh_host_free(item);
}

static void save_fails(const gchar *old, const gchar *name, const gchar *host, const gchar *user, const gchar *port) {
    SshHost *item = ssh_host_new(name, host, user, port);
    GError *error = NULL;
    g_assert_false(sshconf_save(path, old, item, &error));
    g_assert_nonnull(error);
    g_error_free(error);
    ssh_host_free(item);
}

static const gchar *SAMPLE =
    "# my machines\n"
    "Host vidymacpro 192.168.1.232\n"
    "    HostName 192.168.1.232\n"
    "    User vidy\n"
    "    IdentityFile ~/.ssh/id_ed25519\n"
    "\n"
    "# work\n"
    "host=build\n"
    "\tHostname = build.example.org\n"
    "\tPort 2222\n"
    "\n"
    "Host bare\n"
    "\n"
    "# defaults\n"
    "Host *\n"
    "    ServerAliveInterval 30\n"
    "    User nobody\n";

static void test_list(void) {
    put(SAMPLE);
    GPtrArray *hosts = sshconf_list(path, NULL);
    g_assert_cmpuint(hosts->len, ==, 3);
    SshHost *h = g_ptr_array_index(hosts, 0);
    g_assert_cmpstr(h->name, ==, "vidymacpro");
    g_assert_cmpstr(h->host, ==, "192.168.1.232");
    g_assert_cmpstr(h->user, ==, "vidy");
    g_assert_cmpstr(h->port, ==, "");
    h = g_ptr_array_index(hosts, 1);
    g_assert_cmpstr(h->name, ==, "build");
    g_assert_cmpstr(h->host, ==, "build.example.org");
    g_assert_cmpstr(h->user, ==, "");
    g_assert_cmpstr(h->port, ==, "2222");
    h = g_ptr_array_index(hosts, 2);
    g_assert_cmpstr(h->name, ==, "bare");
    g_assert_cmpstr(h->host, ==, "bare"); // no HostName: the name is the address
    g_assert_cmpstr(h->user, ==, "");     // "Host *" options are not this block's
    g_ptr_array_unref(hosts);
}

static void test_missing_file(void) {
    g_remove(path);
    GPtrArray *hosts = sshconf_list(path, NULL);
    g_assert_nonnull(hosts);
    g_assert_cmpuint(hosts->len, ==, 0);
    g_ptr_array_unref(hosts);
    save(NULL, "pi", "10.0.0.5", "root", "");
    gchar *text = get();
    g_assert_cmpstr(text, ==, "Host pi\n    HostName 10.0.0.5\n    User root\n");
    g_free(text);
    struct stat st;
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
}

static void test_add(void) {
    put(SAMPLE);
    save(NULL, "pi", "10.0.0.5", "", "2200");
    gchar *text = get();
    // before the "Host *" defaults and the comment that belongs to them
    g_assert_cmpstr(text, ==,
        "# my machines\n"
        "Host vidymacpro 192.168.1.232\n"
        "    HostName 192.168.1.232\n"
        "    User vidy\n"
        "    IdentityFile ~/.ssh/id_ed25519\n"
        "\n"
        "# work\n"
        "host=build\n"
        "\tHostname = build.example.org\n"
        "\tPort 2222\n"
        "\n"
        "Host bare\n"
        "\n"
        "Host pi\n"
        "    HostName 10.0.0.5\n"
        "    Port 2200\n"
        "\n"
        "# defaults\n"
        "Host *\n"
        "    ServerAliveInterval 30\n"
        "    User nobody\n");
    g_free(text);
    // no rules in the file: at the end
    put("Host a\n    HostName 1.1.1.1\n");
    save(NULL, "b", "2.2.2.2", "", "");
    text = get();
    g_assert_cmpstr(text, ==, "Host a\n    HostName 1.1.1.1\n\nHost b\n    HostName 2.2.2.2\n");
    g_free(text);
    // rules first: still before them
    put("Match all\n    Compression yes\n");
    save(NULL, "b", "2.2.2.2", "", "");
    text = get();
    g_assert_cmpstr(text, ==, "Host b\n    HostName 2.2.2.2\n\nMatch all\n    Compression yes\n");
    g_free(text);
    put(SAMPLE);
    save(NULL, "pi", "10.0.0.5", "", "2200");
    save_fails(NULL, "pi", "10.0.0.6", "", "");      // name taken
    save_fails(NULL, "build", "10.0.0.6", "", "");   // name taken by a hand-written block
}

static void test_edit_keeps_the_rest(void) {
    put(SAMPLE);
    save("vidymacpro", "vidymacpro", "192.168.1.50", "", "22");
    gchar *text = get();
    g_assert_cmpstr(text, ==,
        "# my machines\n"
        "Host vidymacpro 192.168.1.232\n"
        "    HostName 192.168.1.50\n"
        "    Port 22\n"
        "    IdentityFile ~/.ssh/id_ed25519\n"
        "\n"
        "# work\n"
        "host=build\n"
        "\tHostname = build.example.org\n"
        "\tPort 2222\n"
        "\n"
        "Host bare\n"
        "\n"
        "# defaults\n"
        "Host *\n"
        "    ServerAliveInterval 30\n"
        "    User nobody\n");
    g_free(text);
}

static void test_rename_and_fill(void) {
    put(SAMPLE);
    save("vidymacpro", "mac", "192.168.1.232", "vidy", "");
    save("build", "build", "build.example.org", "ci", "");
    save("bare", "bare", "10.1.1.1", "me", "");
    gchar *text = get();
    g_assert_cmpstr(text, ==,
        "# my machines\n"
        "Host mac 192.168.1.232\n"
        "    HostName 192.168.1.232\n"
        "    User vidy\n"
        "    IdentityFile ~/.ssh/id_ed25519\n"
        "\n"
        "# work\n"
        "host=build\n"
        "\tHostName build.example.org\n"
        "\tUser ci\n"
        "\n"
        "Host bare\n"
        "    HostName 10.1.1.1\n"
        "    User me\n"
        "\n"
        "# defaults\n"
        "Host *\n"
        "    ServerAliveInterval 30\n"
        "    User nobody\n");
    g_free(text);
    save_fails("mac", "build", "192.168.1.232", "", ""); // rename onto another connection
    save_fails("gone", "gone", "1.1.1.1", "", "");       // editing what is not there
}

static void test_delete(void) {
    put(SAMPLE);
    g_assert_true(sshconf_delete(path, "build", NULL));
    gchar *text = get();
    g_assert_cmpstr(text, ==,
        "# my machines\n"
        "Host vidymacpro 192.168.1.232\n"
        "    HostName 192.168.1.232\n"
        "    User vidy\n"
        "    IdentityFile ~/.ssh/id_ed25519\n"
        "\n"
        "# work\n"
        "\n"
        "Host bare\n"
        "\n"
        "# defaults\n"
        "Host *\n"
        "    ServerAliveInterval 30\n"
        "    User nobody\n");
    g_free(text);
    g_assert_true(sshconf_delete(path, "bare", NULL));
    g_assert_true(sshconf_delete(path, "vidymacpro", NULL));
    text = get();
    g_assert_cmpstr(text, ==,
        "# my machines\n"
        "\n"
        "# work\n"
        "\n"
        "# defaults\n"
        "Host *\n"
        "    ServerAliveInterval 30\n"
        "    User nobody\n");
    g_free(text);
    g_assert_false(sshconf_delete(path, "vidymacpro", NULL));
    // a wildcard block is not a connection and cannot be removed from here
    g_assert_false(sshconf_delete(path, "*", NULL));
    put("Host only\n    HostName 1.2.3.4\n");
    g_assert_true(sshconf_delete(path, "only", NULL));
    text = get();
    g_assert_cmpstr(text, ==, "");
    g_free(text);
}

static void test_validate(void) {
    const gchar *bad[][4] = {
        { "", "1.2.3.4", "", "" },
        { "a b", "1.2.3.4", "", "" },
        { "-oProxyCommand=x", "1.2.3.4", "", "" },
        { "x;reboot", "1.2.3.4", "", "" },
        { "*", "1.2.3.4", "", "" },
        { "ok", "", "", "" },
        { "ok", "-oProxyCommand=x", "", "" },
        { "ok", "host name", "", "" },
        { "ok", "1.2.3.4", "ro ot", "" },
        { "ok", "1.2.3.4", "$(id)", "" },
        { "ok", "1.2.3.4", "", "0" },
        { "ok", "1.2.3.4", "", "65536" },
        { "ok", "1.2.3.4", "", "22x" },
        { "ok", "1.2.3.4", "", "+22" },
        // ssh would read %w as one of its tokens and refuse the file
        { "ok", "fe80::1%wlan0", "", "" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
        SshHost *item = ssh_host_new(bad[i][0], bad[i][1], bad[i][2], bad[i][3]);
        GError *error = NULL;
        g_assert_false(sshconf_validate(item, &error));
        g_assert_nonnull(error);
        g_error_free(error);
        ssh_host_free(item);
    }
    SshHost *good = ssh_host_new("my-box_1.lan", "fe80::1", "vidy.v", "65535");
    g_assert_true(sshconf_validate(good, NULL));
    ssh_host_free(good);
}

static void test_no_final_newline_and_crlf(void) {
    put("Host a\r\n  HostName 1.1.1.1\r\nHost b\n  HostName 2.2.2.2");
    GPtrArray *hosts = sshconf_list(path, NULL);
    g_assert_cmpuint(hosts->len, ==, 2);
    g_assert_cmpstr(((SshHost *) g_ptr_array_index(hosts, 0))->host, ==, "1.1.1.1");
    g_assert_cmpstr(((SshHost *) g_ptr_array_index(hosts, 1))->host, ==, "2.2.2.2");
    g_ptr_array_unref(hosts);
    save("b", "b", "3.3.3.3", "", "");
    gchar *text = get();
    // written back with plain line ends throughout, never mixed
    g_assert_cmpstr(text, ==, "Host a\n  HostName 1.1.1.1\nHost b\n  HostName 3.3.3.3\n");
    g_free(text);
}

static void test_alias_is_taken(void) {
    // "two" already leads to 1.2.3.4: ssh would never reach a new block
    put("Host one two\n  HostName 1.2.3.4\n");
    save_fails(NULL, "two", "5.6.7.8", "", "");
}

static void test_through_symlink(void) {
    // a config kept elsewhere and linked in stays a link
    gchar *dir = g_path_get_dirname(path);
    gchar *real = g_build_filename(dir, "config.real", NULL);
    g_assert_true(g_file_set_contents(real, "Host a\n  HostName 1.1.1.1\n", -1, NULL));
    g_unlink(path);
    g_assert_cmpint(symlink(real, path), ==, 0);
    save("a", "a", "3.3.3.3", "", "");
    g_assert_true(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(real, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "Host a\n  HostName 3.3.3.3\n");
    g_free(text);
    g_unlink(path);
    g_unlink(real);
    g_free(real);
    g_free(dir);
}

static void test_local_name_proxy(void) {
    put("Host mac\n    HostName 192.168.1.232\n    User vidy\n"
        "\nHost ts\n    HostName mac.tail.ts.net\n    ProxyCommand /usr/bin/tailscale nc %h %p\n");
    // a name of the local network: kterm-mdns finds it
    save("mac", "mac", "vidy-macbookpro.local", "vidy", "");
    gchar *text = get();
    g_assert_nonnull(strstr(text, "    HostName vidy-macbookpro.local\n    ProxyCommand " SSHCONF_MDNS_PROXY "\n    User vidy\n"));
    g_free(text);
    // saved again: still one line
    save("mac", "mac", "vidy-macbookpro.local", "vidy", "");
    text = get();
    g_assert_null(strstr(strstr(text, "kterm-mdns") + 1, "kterm-mdns"));
    g_free(text);
    // back to an address: the line goes
    save("mac", "mac", "192.168.1.232", "vidy", "");
    text = get();
    g_assert_null(strstr(text, "kterm-mdns"));
    // the user's own ProxyCommand stays
    g_assert_nonnull(strstr(text, "ProxyCommand /usr/bin/tailscale nc %h %p"));
    g_free(text);
    save("ts", "ts", "mac.tail.ts.net", "vidy", "");
    text = get();
    g_assert_nonnull(strstr(text, "ProxyCommand /usr/bin/tailscale nc %h %p"));
    g_free(text);
    g_assert_true(sshconf_is_local("a.local"));
    g_assert_true(sshconf_is_local("a.local."));
    g_assert_false(sshconf_is_local("a.localhost"));
}

static void test_tightens_loose_mode(void) {
    // ssh refuses a config others may write to; a save has to repair it
    put("Host a\n    HostName 1.1.1.1\n");
    g_assert_cmpint(g_chmod(path, 0666), ==, 0);
    save("a", "a", "3.3.3.3", "", "");
    GStatBuf st;
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    g_assert_cmpint(g_chmod(path, 0666), ==, 0);
    GError *error = NULL;
    g_assert_true(sshconf_delete(path, "a", &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    gchar *dir = g_dir_make_tmp("kterm-sshconf-XXXXXX", NULL);
    path = g_build_filename(dir, ".ssh", "config", NULL);
    gchar *ssh_dir = g_path_get_dirname(path);
    g_mkdir_with_parents(ssh_dir, 0700);
    g_test_add_func("/sshconf/list", test_list);
    g_test_add_func("/sshconf/tightens-loose-mode", test_tightens_loose_mode);
    g_test_add_func("/sshconf/missing-file", test_missing_file);
    g_test_add_func("/sshconf/add", test_add);
    g_test_add_func("/sshconf/edit-keeps-the-rest", test_edit_keeps_the_rest);
    g_test_add_func("/sshconf/rename-and-fill", test_rename_and_fill);
    g_test_add_func("/sshconf/delete", test_delete);
    g_test_add_func("/sshconf/validate", test_validate);
    g_test_add_func("/sshconf/no-final-newline-and-crlf", test_no_final_newline_and_crlf);
    g_test_add_func("/sshconf/alias-is-taken", test_alias_is_taken);
    g_test_add_func("/sshconf/through-symlink", test_through_symlink);
    g_test_add_func("/sshconf/local-name-proxy", test_local_name_proxy);
    const int result = g_test_run();
    g_remove(path);
    g_rmdir(ssh_dir);
    g_rmdir(dir);
    return result;
}
