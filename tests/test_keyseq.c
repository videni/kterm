// cc $(pkg-config --cflags --libs glib-2.0) -I.. test_keyseq.c ../keyseq.c -o /tmp/test_keyseq
#include <glib.h>
#include <string.h>
#include "keyseq.h"

static void is(const gchar *spec, const gchar *expected, gsize length) {
    GError *error = NULL;
    GBytes *bytes = keyseq_parse(spec, &error);
    g_assert_no_error(error);
    g_assert_nonnull(bytes);
    gsize size = 0;
    const guint8 *data = g_bytes_get_data(bytes, &size);
    g_assert_cmpmem(data, size, expected, length);
    g_bytes_unref(bytes);
}

static void fails(const gchar *spec) {
    GError *error = NULL;
    GBytes *bytes = keyseq_parse(spec, &error);
    g_assert_null(bytes);
    g_assert_nonnull(error);
    g_error_free(error);
}

static void test_ctrl(void) {
    is("C-u", "\x15", 1);
    is("C-a", "\x01", 1);
    is("c-A", "\x01", 1);
    is("C-[", "\x1b", 1);
    is("C-?", "\x7f", 1);
    // the tmux prefix of the user who asked for this: one NUL byte
    is("C-Space", "\0", 1);
    is("C-@", "\0", 1);
}

static void test_sequences(void) {
    is("C-Space c", "\0c", 2);
    is("C-b n", "\x02n", 2);
    is("  C-b    %  ", "\x02%", 2);
    is("Esc : w q Enter", "\x1b:wq\r", 5);
    is("C-Space |", "\0|", 2);
}

static void test_named_and_alt(void) {
    is("Enter", "\r", 1);
    is("tab", "\t", 1);
    is("Up", "\x1b[A", 3);
    is("PgDn", "\x1b[6~", 4);
    is("F5", "\x1b[15~", 5);
    is("M-b", "\x1b" "b", 2);
    is("M-Left", "\x1b\x1b[D", 4);
    is("M-C-h", "\x1b\x08", 2);
    is("Space", " ", 1);
}

static void test_text(void) {
    is("\"git pull\" Enter", "git pull\r", 9);
    is("\"say \\\"hi\\\"\"", "say \"hi\"", 8);
    is("\"a\\\\b\"", "a\\b", 3);
    is("\"\" x", "x", 1);
    // one character outside ASCII is still one key
    is("你", "\xe4\xbd\xa0", 3);
    is("\"你好\"", "\xe4\xbd\xa0\xe5\xa5\xbd", 6);
}

static void test_errors(void) {
    fails("");
    fails("   ");
    fails(NULL);
    fails("git");          // a word: text needs quotes
    fails("C-");           // modifier without a key
    fails("C-1");          // no such control character
    fails("C-Spce");       // misspelled name
    fails("Entr");
    fails("\"open");       // quote not closed
    fails("M-");
}

static void test_plus_spelling(void) {
    // the way most people write it
    is("Ctrl+Space c", "\0c", 2);
    is("ctrl+u", "\x15", 1);
    is("Alt+b", "\x1b" "b", 2);
    is("Alt+Ctrl+h", "\x1b\x08", 2);
    fails("Ctrl+");
    fails("Ctrl+1");
}

static void describes(const gchar *spec, const gchar *expected) {
    GError *error = NULL;
    gchar *text = keyseq_describe(spec, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(text, ==, expected);
    g_free(text);
}

static void test_describe(void) {
    describes("C-u", "按 Ctrl+U");
    describes("Ctrl+Space c", "按 Ctrl+空格，然后按 c（小写）");
    describes("C-Space C", "按 Ctrl+空格，然后按 C（大写）");
    describes("M-b", "按 Alt+b（小写）");
    describes("\"git pull\" Enter", "输入“git pull”，然后按 回车");
    describes("Esc : w", "按 Esc，然后按 :，然后按 w（小写）");
    describes("BSpace Up PgDn F5", "按 退格，然后按 ↑，然后按 PgDn，然后按 F5");
    // what does not parse is explained the same way as for keyseq_parse()
    GError *error = NULL;
    gchar *text = keyseq_describe("C-Spcae", &error);
    g_assert_null(text);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "Spcae"));
    g_error_free(error);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/keyseq/ctrl", test_ctrl);
    g_test_add_func("/keyseq/sequences", test_sequences);
    g_test_add_func("/keyseq/named-and-alt", test_named_and_alt);
    g_test_add_func("/keyseq/text", test_text);
    g_test_add_func("/keyseq/errors", test_errors);
    g_test_add_func("/keyseq/plus-spelling", test_plus_spelling);
    g_test_add_func("/keyseq/describe", test_describe);
    return g_test_run();
}
