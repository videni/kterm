// cc $(pkg-config --cflags --libs glib-2.0) -I.. test_confedit.c ../confedit.c -o /tmp/test_confedit
#include <glib.h>
#include <glib/gstdio.h>
#include "confedit.h"

static void set_is(const gchar *text, const gchar *expected) {
    gchar *result = confedit_set(text, "font_size", "font_size = 10");
    g_assert_cmpstr(result, ==, expected);
    g_free(result);
}

static void test_replace(void) {
    set_is("keyboard = 1\nfont_size = 8\ncursor_shape = B\n",
           "keyboard = 1\nfont_size = 10\ncursor_shape = B\n");
    // spacing as anyone may write it
    set_is("  font_size=8\n", "font_size = 10\n");
}

static void test_append(void) {
    set_is("tmux_prefix = \"C-Space\"\n", "tmux_prefix = \"C-Space\"\nfont_size = 10\n");
    // no newline at the end of the last line
    set_is("tmux_prefix = \"C-Space\"", "tmux_prefix = \"C-Space\"\nfont_size = 10\n");
    set_is("", "font_size = 10\n");
    set_is(NULL, "font_size = 10\n");
}

static void test_comments_kept(void) {
    // a commented-out setting stays as it is, the value goes after it
    set_is("# font size\n#font_size = 8\n", "# font size\n#font_size = 8\nfont_size = 10\n");
}

static void test_other_keys_untouched(void) {
    // a key that only starts the same way is another setting
    set_is("font_size_big = 20\n", "font_size_big = 20\nfont_size = 10\n");
}

static void test_last_wins(void) {
    // the reader keeps the last value: only one line is left
    set_is("font_size = 8\nfont_size = 9\n", "font_size = 10\n");
}

static void test_save_file(void) {
    gchar *dir = g_dir_make_tmp("confedit-XXXXXX", NULL);
    gchar *sub = g_build_filename(dir, "kterm", NULL);
    gchar *path = g_build_filename(sub, "kterm.conf", NULL);
    GError *error = NULL;
    // neither the file nor its directory yet
    g_assert_true(confedit_save(path, "font_size", "font_size = 9", &error));
    g_assert_no_error(error);
    gchar *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "font_size = 9\n");
    g_free(text);
    g_assert_true(confedit_save(path, "font_size", "font_size = 11", &error));
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_cmpstr(text, ==, "font_size = 11\n");
    g_free(text);
    g_unlink(path);
    g_rmdir(sub);
    g_rmdir(dir);
    g_free(path);
    g_free(sub);
    g_free(dir);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/confedit/replace", test_replace);
    g_test_add_func("/confedit/append", test_append);
    g_test_add_func("/confedit/comments-kept", test_comments_kept);
    g_test_add_func("/confedit/other-keys-untouched", test_other_keys_untouched);
    g_test_add_func("/confedit/last-wins", test_last_wins);
    g_test_add_func("/confedit/save-file", test_save_file);
    return g_test_run();
}
