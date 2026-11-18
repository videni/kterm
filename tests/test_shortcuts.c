// cc $(pkg-config --cflags --libs glib-2.0) -I.. test_shortcuts.c ../shortcuts.c ../keyseq.c -o /tmp/test_shortcuts
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "shortcuts.h"

static void has(GPtrArray *list, guint index, const gchar *label, const gchar *keys, gsize length) {
    g_assert_cmpuint(index, <, list->len);
    const Shortcut *item = g_ptr_array_index(list, index);
    g_assert_cmpstr(item->label, ==, label);
    gsize size = 0;
    const guint8 *data = g_bytes_get_data(item->keys, &size);
    g_assert_cmpmem(data, size, keys, length);
}

static void test_keys_only(void) {
    gchar *problems = NULL;
    GPtrArray *list = shortcuts_parse("C-a\nC-e\n  C-u  \n", &problems);
    g_assert_null(problems);
    g_assert_cmpuint(list->len, ==, 3);
    // the button shows what was written
    has(list, 0, "C-a", "\x01", 1);
    has(list, 1, "C-e", "\x05", 1);
    has(list, 2, "C-u", "\x15", 1);
    g_ptr_array_unref(list);
}

static void test_named(void) {
    gchar *problems = NULL;
    GPtrArray *list = shortcuts_parse("TX = C-Space\n拉代码 = \"git pull\" Enter\n", &problems);
    g_assert_null(problems);
    g_assert_cmpuint(list->len, ==, 2);
    has(list, 0, "TX", "\0", 1);
    has(list, 1, "拉代码", "git pull\r", 9);
    g_ptr_array_unref(list);
}

static void test_equals_sign(void) {
    gchar *problems = NULL;
    // the = key itself, and = inside quoted text, are keys, not a name
    GPtrArray *list = shortcuts_parse("=\n\"a=b\"\n等号 = \"x = 1\"\n", &problems);
    g_assert_null(problems);
    g_assert_cmpuint(list->len, ==, 3);
    has(list, 0, "=", "=", 1);
    has(list, 1, "\"a=b\"", "a=b", 3);
    has(list, 2, "等号", "x = 1", 5);
    g_ptr_array_unref(list);
}

static void test_comments_and_blanks(void) {
    gchar *problems = NULL;
    GPtrArray *list = shortcuts_parse("# 说明\n\n   \n  # 缩进的说明\nC-c\r\n", &problems);
    g_assert_null(problems);
    g_assert_cmpuint(list->len, ==, 1);
    has(list, 0, "C-c", "\x03", 1);
    g_ptr_array_unref(list);
}

static void test_bad_lines_skipped(void) {
    gchar *problems = NULL;
    GPtrArray *list = shortcuts_parse("C-a\nC-Spcae c\n名字 =\n= C-a\nC-e\n", &problems);
    // the rest still works
    g_assert_cmpuint(list->len, ==, 2);
    has(list, 0, "C-a", "\x01", 1);
    has(list, 1, "C-e", "\x05", 1);
    // each bad line is reported with its number
    g_assert_nonnull(problems);
    g_assert_nonnull(strstr(problems, "第 2 行"));
    g_assert_nonnull(strstr(problems, "Spcae"));
    g_assert_nonnull(strstr(problems, "第 3 行"));
    g_assert_nonnull(strstr(problems, "第 4 行"));
    g_free(problems);
    g_ptr_array_unref(list);
}

static void test_too_many(void) {
    GString *text = g_string_new(NULL);
    for (guint i = 0; i < SHORTCUTS_MAX + 2; i++) {
        g_string_append(text, "C-a\n");
    }
    gchar *problems = NULL;
    GPtrArray *list = shortcuts_parse(text->str, &problems);
    g_assert_cmpuint(list->len, ==, SHORTCUTS_MAX);
    g_assert_nonnull(problems);
    g_free(problems);
    g_ptr_array_unref(list);
    g_string_free(text, TRUE);
}

static void test_defaults(void) {
    gchar *problems = NULL;
    GPtrArray *list = shortcuts_parse(SHORTCUTS_DEFAULT, &problems);
    g_assert_null(problems);
    g_assert_cmpuint(list->len, ==, 7);
    has(list, 0, "C-a", "\x01", 1);
    has(list, 3, "C-y", "\x19", 1);
    has(list, 6, "TX", "\0", 1);
    g_ptr_array_unref(list);
}

static void test_load(void) {
    gchar *dir = g_dir_make_tmp("shortcuts-XXXXXX", NULL);
    gchar *path = g_build_filename(dir, "shortcuts.conf", NULL);
    gchar *problems = NULL;
    // no file: the defaults
    GPtrArray *list = shortcuts_load(path, &problems);
    g_assert_null(problems);
    g_assert_cmpuint(list->len, ==, 7);
    g_ptr_array_unref(list);
    // a file: what it says, even when that is nothing
    g_assert_true(g_file_set_contents(path, "C-w\n", -1, NULL));
    list = shortcuts_load(path, &problems);
    g_assert_cmpuint(list->len, ==, 1);
    has(list, 0, "C-w", "\x17", 1);
    g_ptr_array_unref(list);
    g_assert_true(g_file_set_contents(path, "# 全部关掉\n", -1, NULL));
    list = shortcuts_load(path, &problems);
    g_assert_null(problems);
    g_assert_cmpuint(list->len, ==, 0);
    g_ptr_array_unref(list);
    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/shortcuts/keys-only", test_keys_only);
    g_test_add_func("/shortcuts/named", test_named);
    g_test_add_func("/shortcuts/equals-sign", test_equals_sign);
    g_test_add_func("/shortcuts/comments-and-blanks", test_comments_and_blanks);
    g_test_add_func("/shortcuts/bad-lines-skipped", test_bad_lines_skipped);
    g_test_add_func("/shortcuts/too-many", test_too_many);
    g_test_add_func("/shortcuts/defaults", test_defaults);
    g_test_add_func("/shortcuts/load", test_load);
    return g_test_run();
}
