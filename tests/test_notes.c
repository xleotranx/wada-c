/* Notes tests: split records, migration from the inline layout, and search. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "notes.h"
#include "test_env.h"
#include "wada.h"
#include "wada_iface.h"
#include "wada_store_sqlite.h"

static int g_checks = 0;

#define CHECK(expr)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(expr)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);             \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

static void clean(const char *name) {
    char buf[128];
    const char *suffix[3] = {"", "-wal", "-shm"};
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof buf, "%s%s", name, suffix[i]);
        remove(buf);
    }
}

static bool has_bytes(const void *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (m > n) return false;
    const uint8_t *h = (const uint8_t *)hay;
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(h + i, needle, m) == 0) return true;
    }
    return false;
}

static int test_notes(void) {
    const char *db = "test_notes.db";
    clean(db);
    wada_err err;
    wada_err_clear(&err);
    notes n;

    CHECK(notes_open(&n, wada_store_sqlite(), db, &err) == WADA_OK);
    CHECK(notes_count(&n) == 0);

    int first = notes_create(&n, &err);
    CHECK(first == 0);
    CHECK(notes_save(&n, 0, "Shopping", "milk\neggs", &err) == WADA_OK);
    CHECK(strcmp(notes_at(&n, 0)->title, "Shopping") == 0);

    char body[256];
    CHECK(notes_body(&n, 0, body, sizeof body, &err) == WADA_OK);
    CHECK(strcmp(body, "milk\neggs") == 0);

    int second = notes_create(&n, &err);
    CHECK(second == 1);
    CHECK(notes_save(&n, 1, "Second", "some text", &err) == WADA_OK);

    /* Search: term match, prefix match, body match, no match. */
    int out[8];
    CHECK(notes_search(&n, "milk", out, 8) == 1 && out[0] == 0);
    CHECK(notes_search(&n, "shop", out, 8) == 1 && out[0] == 0);
    CHECK(notes_search(&n, "text", out, 8) == 1 && out[0] == 1);
    CHECK(notes_search(&n, "zzz", out, 8) == 0);
    CHECK(notes_search(&n, "", out, 8) == 0); /* empty: caller lists instead */

    /* A title is sanitized, never split into a fake second line. */
    CHECK(notes_save(&n, 1, "two\nlines", "body", &err) == WADA_OK);
    CHECK(strchr(notes_at(&n, 1)->title, '\n') == NULL);

    CHECK(notes_find(&n, notes_at(&n, 0)->id) == 0);
    CHECK(notes_find(&n, "does-not-exist") == -1);
    notes_close(&n);

    /* Reopen: metadata is loaded, bodies are fetched on demand. */
    CHECK(notes_open(&n, wada_store_sqlite(), db, &err) == WADA_OK);
    CHECK(notes_count(&n) == 2);
    CHECK(strcmp(notes_at(&n, 0)->title, "Shopping") == 0);
    CHECK(strcmp(notes_at(&n, 1)->title, "two lines") == 0);
    CHECK(notes_body(&n, 0, body, sizeof body, &err) == WADA_OK);
    CHECK(strcmp(body, "milk\neggs") == 0);
    CHECK(notes_search(&n, "milk", out, 8) == 1 && out[0] == 0);

    /* Ids stay unique across a close/open cycle. The old scheme formatted
     * "<seconds>-<serial>" and reset the serial on open, so a create in the
     * same second as the previous run's first note overwrote it. */
    char first_id[64];
    snprintf(first_id, sizeof first_id, "%s", notes_at(&n, 0)->id);
    int extra = notes_create(&n, &err);
    CHECK(extra == 2);
    CHECK(strcmp(notes_at(&n, extra)->id, first_id) != 0);
    CHECK(notes_find(&n, first_id) == 0);
    CHECK(notes_save(&n, extra, "After reopen", "", &err) == WADA_OK);
    notes_close(&n);
    CHECK(notes_open(&n, wada_store_sqlite(), db, &err) == WADA_OK);
    CHECK(notes_count(&n) == 3);
    CHECK(notes_find(&n, first_id) == 0);
    CHECK(strcmp(notes_at(&n, 0)->title, "Shopping") == 0);

    CHECK(notes_remove(&n, 0, &err) == WADA_OK);
    CHECK(notes_count(&n) == 2);
    CHECK(strcmp(notes_at(&n, 0)->title, "two lines") == 0);
    notes_close(&n);

    CHECK(notes_open(&n, wada_store_sqlite(), db, &err) == WADA_OK);
    CHECK(notes_count(&n) == 2);
    CHECK(notes_search(&n, "milk", out, 8) == 0); /* the index dropped it too */
    notes_close(&n);
    clean(db);

    /* Migration: a v1 record with an inline body becomes meta + body. */
    const char *legacy_db = "test_notes_legacy.db";
    clean(legacy_db);
    const wada_store_ops *ops = wada_store_sqlite();
    wada_store *store = NULL;
    CHECK(ops->open(legacy_db, &store, &err) == WADA_OK);
    wada_txn *txn = NULL;
    CHECK(ops->begin(store, WADA_STORE_WRITE, &txn, &err) == WADA_OK);
    const char *old_value = "v1\n1700000000\nLegacy title\nold body line";
    const char *old_value_empty = "v1\n1700000001\nEmpty body\n";
    wada_slice k1 = {(const uint8_t *)"note/42", 7};
    wada_slice v1 = {(const uint8_t *)old_value, strlen(old_value)};
    wada_slice k2 = {(const uint8_t *)"note/43", 7};
    wada_slice v2 = {(const uint8_t *)old_value_empty, strlen(old_value_empty)};
    CHECK(ops->put(txn, k1, v1, &err) == WADA_OK);
    CHECK(ops->put(txn, k2, v2, &err) == WADA_OK);
    CHECK(ops->commit(txn, &err) == WADA_OK);
    ops->close(store);

    CHECK(notes_open(&n, wada_store_sqlite(), legacy_db, &err) == WADA_OK);
    CHECK(notes_count(&n) == 2);
    CHECK(strcmp(notes_at(&n, 0)->title, "Legacy title") == 0);
    CHECK(strcmp(notes_at(&n, 1)->title, "Empty body") == 0);
    CHECK(notes_body(&n, 0, body, sizeof body, &err) == WADA_OK);
    CHECK(strcmp(body, "old body line") == 0);
    CHECK(notes_body(&n, 1, body, sizeof body, &err) == WADA_OK);
    CHECK(body[0] == '\0');
    notes_close(&n);

    /* The metadata record no longer carries the body. */
    store = NULL;
    CHECK(ops->open(legacy_db, &store, &err) == WADA_OK);
    CHECK(ops->begin(store, WADA_STORE_READ, &txn, &err) == WADA_OK);
    wada_slice meta_value;
    CHECK(ops->get(txn, k1, &meta_value, &err) == WADA_OK);
    CHECK(!has_bytes(meta_value.p, meta_value.n, "old body"));
    CHECK(ops->commit(txn, &err) == WADA_OK);
    ops->close(store);
    clean(legacy_db);
    return 0;
}

static int test_backup(void) {
    const char *source = "test_notes_backup.db";
    const char *target = "test_notes_import.db";
    const char *backup = "test_notes_backup.wnotes";
    clean(source);
    clean(target);
    remove(backup);

    wada_err err;
    wada_err_clear(&err);
    notes n;
    CHECK(notes_open(&n, wada_store_sqlite(), source, &err) == WADA_OK);

    CHECK(notes_create(&n, &err) == 0);
    CHECK(notes_save(&n, 0, "Alpha", "body one", &err) == WADA_OK);
    CHECK(notes_create(&n, &err) == 1);
    CHECK(notes_save(&n, 1, "Beta", "line one\nline two", &err) == WADA_OK);

    /* A body far beyond the old 8 KB editor cap survives the store and backup. */
    const size_t long_len = 50000;
    char *long_body = (char *)malloc(long_len + 1);
    CHECK(long_body != NULL);
    memset(long_body, 'x', long_len);
    long_body[long_len] = '\0';
    CHECK(notes_create(&n, &err) == 2);
    CHECK(notes_save(&n, 2, "Long", long_body, &err) == WADA_OK);

    size_t len = 0;
    CHECK(notes_body_size(&n, 2, &len, &err) == WADA_OK);
    CHECK(len == long_len);
    char *readback = (char *)malloc(long_len + 1);
    CHECK(readback != NULL);
    CHECK(notes_body(&n, 2, readback, long_len + 1, &err) == WADA_OK);
    CHECK(strcmp(readback, long_body) == 0);

    CHECK(notes_export(&n, backup, &err) == WADA_OK);
    notes_close(&n);

    CHECK(notes_open(&n, wada_store_sqlite(), target, &err) == WADA_OK);
    CHECK(notes_count(&n) == 0);
    int added = 0;
    CHECK(notes_import(&n, backup, &added, &err) == WADA_OK);
    CHECK(added == 3);
    CHECK(notes_count(&n) == 3);
    CHECK(strcmp(notes_at(&n, 0)->title, "Alpha") == 0);
    CHECK(strcmp(notes_at(&n, 1)->title, "Beta") == 0);
    CHECK(strcmp(notes_at(&n, 2)->title, "Long") == 0);

    char body[256];
    CHECK(notes_body(&n, 1, body, sizeof body, &err) == WADA_OK);
    CHECK(strcmp(body, "line one\nline two") == 0);
    CHECK(notes_body_size(&n, 2, &len, &err) == WADA_OK);
    CHECK(len == long_len);
    CHECK(notes_body(&n, 2, readback, long_len + 1, &err) == WADA_OK);
    CHECK(strcmp(readback, long_body) == 0);
    notes_close(&n);

    free(readback);
    free(long_body);
    clean(source);
    clean(target);
    remove(backup);
    return 0;
}

int main(void) {
    test_daily_backup(false); /* this test is not about backups */
    if (test_notes() || test_backup()) {
        fprintf(stderr, "notes tests: FAILED after %d checks\n", g_checks);
        return 1;
    }
    printf("notes tests: %d checks passed\n", g_checks);
    return 0;
}
