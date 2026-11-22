/* Store contract tests: these hold for any provider that implements
 * wada_store_ops, not just SQLite. */
#include <stdio.h>
#include <string.h>

#include "wada.h"
#include "wada_iface.h"
#include "wada_store_sqlite.h"
#include "test_env.h"

/* Tests may speak to the vendor to build an old-schema fixture; the app never
 * does. */
#include <sqlite3.h>

static int g_checks = 0;

#define CHECK(expr)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(expr)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);             \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

static wada_slice S(const char *s) {
    wada_slice v;
    v.p = (const uint8_t *)s;
    v.n = strlen(s);
    return v;
}

static bool slice_is(wada_slice s, const char *text) {
    return s.n == strlen(text) && memcmp(s.p, text, s.n) == 0;
}

static int search_count(const wada_store_ops *ops, wada_txn *txn, const char *prefix,
                        const char *query, char *first_key, size_t first_cap) {
    wada_iter *it = NULL;
    wada_err err;
    wada_err_clear(&err);
    if (first_key && first_cap) first_key[0] = '\0';

    wada_slice p = S(prefix);
    if (ops->search(txn, p, query, 0, &it, &err) != WADA_OK) return -1;

    int n = 0;
    wada_slice key, value;
    while (ops->next(it, &key, &value, &err) == WADA_OK) {
        if (n == 0 && first_key && first_cap) {
            size_t len = key.n < first_cap - 1 ? key.n : first_cap - 1;
            memcpy(first_key, key.p, len);
            first_key[len] = '\0';
        }
        n++;
    }
    ops->iter_close(it);
    return n;
}

/* ---- migration backup ---------------------------------------------------
 *
 * There is no tool in the app that produces an old schema, so this fixture
 * writes a v1 database with the raw SQLite API — tests are allowed to know the
 * vendor; the app is not. The provider must copy it before upgrading it, and
 * the copy must still be the old database.
 */

static int make_v1_db(const char *path) {
    remove(path);
    sqlite3 *raw = NULL;
    if (sqlite3_open(path, &raw) != SQLITE_OK) return 1;
    int rc = sqlite3_exec(raw,
                          "CREATE TABLE kv (k TEXT PRIMARY KEY NOT NULL, v BLOB NOT NULL)"
                          " WITHOUT ROWID;",
                          NULL, NULL, NULL);
    if (rc == SQLITE_OK) {
        /* Keys and values are BLOBs in this provider; a TEXT key would not match
         * the provider's BLOB binds even though the bytes are equal. */
        rc = sqlite3_exec(raw,
                          "INSERT INTO kv (k, v) VALUES (CAST('note/legacy' AS BLOB),"
                          " CAST('body from v1' AS BLOB));",
                          NULL, NULL, NULL);
    }
    if (rc == SQLITE_OK) rc = sqlite3_exec(raw, "PRAGMA user_version = 1;", NULL, NULL, NULL);
    sqlite3_close(raw);
    return rc == SQLITE_OK ? 0 : 1;
}

static int check_v1_copy(const char *path) {
    sqlite3 *raw = NULL;
    if (sqlite3_open(path, &raw) != SQLITE_OK) return 0;
    int version = -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(raw, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    char body[64] = "";
    if (sqlite3_prepare_v2(raw, "SELECT v FROM kv WHERE k = CAST('note/legacy' AS BLOB)", -1,
                           &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *text = sqlite3_column_text(stmt, 0);
            if (text) snprintf(body, sizeof body, "%s", (const char *)text);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(raw);
    return version == 1 && strcmp(body, "body from v1") == 0;
}

static int test_migration_backup(void) {
    const char *db_path = "test_v1.db";
    remove(db_path);
    remove("test_v1.db-wal");
    remove("test_v1.db-shm");
    remove("backups/test_v1-daily.db");
    remove("backups\\test_v1-daily.db");
    CHECK(make_v1_db(db_path) == 0);

    const wada_store_ops *ops = wada_store_sqlite();
    wada_err err;
    wada_err_clear(&err);
    wada_store *db = NULL;
    CHECK(ops->open(db_path, &db, &err) == WADA_OK);
    CHECK(ops->schema_version(db) == 2u);

    const char *pre = ops->last_backup(db);
    CHECK(pre != NULL && pre[0] != '\0');
    CHECK(strstr(pre, "pre-v1") != NULL);
    CHECK(check_v1_copy(pre));

    /* The upgraded database keeps the record. */
    wada_txn *txn = NULL;
    CHECK(ops->begin(db, WADA_STORE_READ, &txn, &err) == WADA_OK);
    wada_slice value;
    CHECK(ops->get(txn, S("note/legacy"), &value, &err) == WADA_OK);
    CHECK(slice_is(value, "body from v1"));
    CHECK(ops->commit(txn, &err) == WADA_OK);

    char kept[820];
    snprintf(kept, sizeof kept, "%s", pre);
    ops->close(db);
    remove(kept);
    remove(db_path);
    remove("test_v1.db-wal");
    remove("test_v1.db-shm");
    remove("backups/test_v1-daily.db");
    remove("backups\\test_v1-daily.db");
    test_remove_dir("backups");
    return 0;
}

static int test_store(void) {
    const wada_store_ops *ops = wada_store_sqlite();
    CHECK(ops != NULL);
    CHECK(wada_ops_ok(&ops->hdr, sizeof(wada_store_ops), WADA_STORE_ABI));
    CHECK(WADA_STORE_ABI == 3u);
    CHECK(ops->last_backup != NULL);
    CHECK((ops->caps(NULL) & WADA_STORE_CAP_TRANSACTIONS) != 0);
    CHECK((ops->caps(NULL) & WADA_STORE_CAP_MIGRATIONS) != 0);
    CHECK((ops->caps(NULL) & WADA_STORE_CAP_SEARCH) != 0);

    wada_err err;
    wada_err_clear(&err);
    wada_store *db = NULL;
    CHECK(ops->open(":memory:", &db, &err) == WADA_OK);
    CHECK(db != NULL);
    CHECK(ops->schema_version(db) == 2u);

    wada_txn *txn = NULL;
    CHECK(ops->begin(db, WADA_STORE_WRITE, &txn, &err) == WADA_OK);
    CHECK(ops->put(txn, S("note/1"), S("alpha beta"), &err) == WADA_OK);
    CHECK(ops->put(txn, S("note/2"), S("gamma delta"), &err) == WADA_OK);
    CHECK(ops->put(txn, S("other"), S("x marks the spot"), &err) == WADA_OK);
    CHECK(ops->commit(txn, &err) == WADA_OK);

    CHECK(ops->begin(db, WADA_STORE_READ, &txn, &err) == WADA_OK);
    wada_slice value;
    CHECK(ops->get(txn, S("note/1"), &value, &err) == WADA_OK);
    CHECK(slice_is(value, "alpha beta"));
    CHECK(ops->get(txn, S("missing"), &value, &err) == WADA_ERR_NOT_FOUND);

    wada_iter *it = NULL;
    CHECK(ops->list(txn, S("note/"), &it, &err) == WADA_OK);
    wada_slice key;
    int n = 0;
    while (ops->next(it, &key, &value, &err) == WADA_OK) {
        n++;
        CHECK(key.n > 5 && memcmp(key.p, "note/", 5) == 0);
    }
    CHECK(n == 2);
    ops->iter_close(it);

    /* Search: terms are ANDed, the last term is a prefix match, and the
     * prefix argument restricts the keys. */
    char first[32];
    CHECK(search_count(ops, txn, "note/", "alpha", first, sizeof first) == 1);
    CHECK(strcmp(first, "note/1") == 0);
    CHECK(search_count(ops, txn, "note/", "alp", first, sizeof first) == 1);
    CHECK(strcmp(first, "note/1") == 0);
    CHECK(search_count(ops, txn, "note/", "gamma", first, sizeof first) == 1);
    CHECK(strcmp(first, "note/2") == 0);
    CHECK(search_count(ops, txn, "note/", "beta gamma", NULL, 0) == 0);
    CHECK(search_count(ops, txn, "note/", "nomatch", NULL, 0) == 0);
    CHECK(search_count(ops, txn, "other", "spot", first, sizeof first) == 1);
    CHECK(strcmp(first, "other") == 0);
    CHECK(search_count(ops, txn, "note/", "", NULL, 0) == 2); /* empty lists the prefix */

    CHECK(ops->commit(txn, &err) == WADA_OK);

    /* Rollback discards the write and its index entry. */
    CHECK(ops->begin(db, WADA_STORE_WRITE, &txn, &err) == WADA_OK);
    CHECK(ops->put(txn, S("note/3"), S("epsilon"), &err) == WADA_OK);
    ops->rollback(txn);
    CHECK(ops->begin(db, WADA_STORE_READ, &txn, &err) == WADA_OK);
    CHECK(ops->get(txn, S("note/3"), &value, &err) == WADA_ERR_NOT_FOUND);
    CHECK(search_count(ops, txn, "note/", "epsilon", NULL, 0) == 0);
    CHECK(ops->commit(txn, &err) == WADA_OK);

    /* Delete removes the index entry too. */
    CHECK(ops->begin(db, WADA_STORE_WRITE, &txn, &err) == WADA_OK);
    CHECK(ops->del(txn, S("note/1"), &err) == WADA_OK);
    CHECK(ops->commit(txn, &err) == WADA_OK);
    CHECK(ops->begin(db, WADA_STORE_READ, &txn, &err) == WADA_OK);
    CHECK(ops->get(txn, S("note/1"), &value, &err) == WADA_ERR_NOT_FOUND);
    CHECK(search_count(ops, txn, "note/", "alpha", NULL, 0) == 0);
    CHECK(ops->list(txn, S("zzz/"), &it, &err) == WADA_OK);
    CHECK(ops->next(it, &key, &value, &err) == WADA_ERR_NOT_FOUND);
    ops->iter_close(it);
    CHECK(ops->commit(txn, &err) == WADA_OK);

    CHECK(ops->migrate(db, 2, &err) == WADA_OK);
    ops->close(db);

    /* A file database reopens with its data and its index. */
    remove("test_store.db");
    remove("test_store.db-wal");
    remove("test_store.db-shm");
    CHECK(ops->open("test_store.db", &db, &err) == WADA_OK);
    CHECK(ops->begin(db, WADA_STORE_WRITE, &txn, &err) == WADA_OK);
    CHECK(ops->put(txn, S("kept"), S("value"), &err) == WADA_OK);
    CHECK(ops->commit(txn, &err) == WADA_OK);
    ops->close(db);

    CHECK(ops->open("test_store.db", &db, &err) == WADA_OK);
    CHECK(ops->schema_version(db) == 2u);

    /* The daily backup was written on this open (the file already had data). */
    const char *backup = ops->last_backup(db);
    CHECK(backup != NULL && backup[0] != '\0');
    CHECK(strstr(backup, "backups") != NULL);
    CHECK(strstr(backup, "test_store-daily.db") != NULL);

    /* The backup is a complete database: open it and read the record. */
    test_daily_backup(false);
    wada_store *copy = NULL;
    CHECK(ops->open(backup, &copy, &err) == WADA_OK);
    CHECK(ops->begin(copy, WADA_STORE_READ, &txn, &err) == WADA_OK);
    wada_slice backed_up;
    CHECK(ops->get(txn, S("kept"), &backed_up, &err) == WADA_OK);
    CHECK(slice_is(backed_up, "value"));
    CHECK(ops->commit(txn, &err) == WADA_OK);
    ops->close(copy);
    test_daily_backup(true);

    CHECK(ops->begin(db, WADA_STORE_READ, &txn, &err) == WADA_OK);
    CHECK(ops->get(txn, S("kept"), &value, &err) == WADA_OK);
    CHECK(slice_is(value, "value"));
    CHECK(search_count(ops, txn, "", "value", NULL, 0) == 1);
    CHECK(ops->commit(txn, &err) == WADA_OK);
    ops->close(db);
    remove("test_store.db");
    remove("test_store.db-wal");
    remove("test_store.db-shm");
    remove("backups/test_store-daily.db");
    remove("backups\\test_store-daily.db");
    test_remove_dir("backups");
    return 0;
}

int main(void) {
    if (test_store() || test_migration_backup()) {
        fprintf(stderr, "store tests: FAILED after %d checks\n", g_checks);
        return 1;
    }
    printf("store tests: %d checks passed\n", g_checks);
    return 0;
}
