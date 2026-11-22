/* i18n tests: lookup, fallback, scope disposal, miss reporting, formatting. */
#include <stdio.h>
#include <string.h>

#include "wada.h"
#include "wada_i18n.h"

static int g_checks = 0;

#define CHECK(expr)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(expr)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);             \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

/* Static, as the contract requires. */
static const wada_i18n_pair extra[] = {
    {"Scope string", "Chuỗi trong phạm vi"},
    {"%d chars", "%d ký tự"},
};

int main(void) {
    wada_i18n_init();
    wada_i18n_init(); /* idempotent */

    CHECK(wada_i18n_count() == 2);
    CHECK(strcmp(wada_i18n_code(0), "en") == 0);
    CHECK(strcmp(wada_i18n_code(1), "vi") == 0);
    CHECK(strcmp(wada_i18n_name(1), "Tiếng Việt") == 0);
    CHECK(wada_i18n_lang() == 0);

    /* English: the source text is the answer, nothing is a miss. */
    CHECK(strcmp(wada_tr("Settings"), "Settings") == 0);
    CHECK(wada_i18n_miss_count() == 0);

    CHECK(wada_i18n_set_lang_code("vi"));
    CHECK(wada_i18n_lang() == 1);
    CHECK(strcmp(wada_tr("Settings"), "Cài đặt") == 0);
    CHECK(strcmp(wada_tr("Done"), "Xong") == 0);
    CHECK(wada_i18n_miss_count() == 0);

    /* A missing translation degrades to English and is recorded once. */
    CHECK(strcmp(wada_tr("Untranslated phrase"), "Untranslated phrase") == 0);
    CHECK(wada_i18n_miss_count() == 1);
    CHECK(strcmp(wada_i18n_miss_at(0), "Untranslated phrase") == 0);
    CHECK(strcmp(wada_tr("Untranslated phrase"), "Untranslated phrase") == 0);
    CHECK(wada_i18n_miss_count() == 1);

    /* Versions and ids are not UI text: they must not pollute the report. */
    CHECK(strcmp(wada_tr("0.1.0"), "0.1.0") == 0);
    CHECK(strcmp(wada_tr(""), "") == 0);
    CHECK(wada_i18n_miss_count() == 1);

    /* A table owned by a scope disappears with the scope. */
    wada_scope *scope = wada_scope_create(NULL, "test");
    CHECK(scope != NULL);
    CHECK(wada_i18n_register(scope, extra, 2) == WADA_OK);
    CHECK(strcmp(wada_tr("Scope string"), "Chuỗi trong phạm vi") == 0);
    wada_scope_destroy(scope);
    CHECK(strcmp(wada_tr("Scope string"), "Scope string") == 0);

    /* Formatting goes through the translated format string. */
    char line[64];
    wada_scope *fmt_scope = wada_scope_create(NULL, "test");
    CHECK(fmt_scope != NULL);
    CHECK(wada_i18n_register(fmt_scope, extra, 2) == WADA_OK);
    wada_trf(line, sizeof line, "%d chars", 12);
    CHECK(strcmp(line, "12 ký tự") == 0);
    wada_trf(line, sizeof line, "Wada %s · core ABI %u", "0.1.0", 1u);
    CHECK(strcmp(line, "Wada 0.1.0 · lõi ABI 1") == 0);
    wada_scope_destroy(fmt_scope);
    wada_trf(line, sizeof line, "%d chars", 7);
    CHECK(strcmp(line, "7 chars") == 0);

    /* Back to English: misses reset, source returned. */
    wada_i18n_set_lang(0);
    CHECK(wada_i18n_miss_count() == 0);
    CHECK(strcmp(wada_tr("Settings"), "Settings") == 0);

    /* Unknown codes are refused, the language stays. */
    CHECK(!wada_i18n_set_lang_code("xx"));
    CHECK(!wada_i18n_set_lang_code(""));
    CHECK(wada_i18n_lang() == 0);

    printf("i18n tests: %d checks passed\n", g_checks);
    return 0;
}
