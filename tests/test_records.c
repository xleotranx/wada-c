/* Records tests: prefix-scoped CRUD, suffix-id discipline, reopen uniqueness,
 * and line record helpers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wada.h"
#include "wada_records.h"
#include "wada_store_sqlite.h"
#include "test_env.h"

static int g_checks = 0;

#define CHECK(expr)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(expr)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);             \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

static void clean_db(const char *path) {
    char buf[128];
    const char *suffix[3] = {"", "-wal", "-shm"};
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof buf, "%s%s", path, suffix[i]);
        remove(buf);
    }
}

typedef struct visit_bag {
    char ids[8][64];
    char vals[8][64];
    int count;
    int stop_after;
} visit_bag;

static bool collect_visitor(const char *id, wada_slice value, void *user) {
    visit_bag *b = (visit_bag *)user;
    if (b->count < 8) {
        snprintf(b->ids[b->count], sizeof b->ids[b->count], "%s", id);
        size_t n = value.n < sizeof b->vals[b->count] - 1 ? value.n : sizeof b->vals[b->count] - 1;
        memcpy(b->vals[b->count], value.p, n);
        b->vals[b->count][n] = '\0';
        b->count++;
    }
    if (b->stop_after > 0 && b->count >= b->stop_after) return false;
    return true;
}

static int test_abi_and_args(void) {
    wada_err err;
    wada_records r;
    const wada_store_ops *sqlite = wada_store_sqlite();

    CHECK(wada_records_open(NULL, sqlite, ":memory:", "item/", &err) == WADA_ERR_INVALID);
    CHECK(wada_records_open(&r, NULL, ":memory:", "item/", &err) == WADA_ERR_INVALID);
    CHECK(wada_records_open(&r, sqlite, ":memory:", "", &err) == WADA_ERR_INVALID);
    CHECK(wada_records_open(&r, sqlite, ":memory:",
                            "this_prefix_is_far_too_long_for_buffer/", &err) == WADA_ERR_INVALID);

    wada_store_ops bad_ops = *sqlite;
    bad_ops.hdr.version = WADA_STORE_ABI + 99u;
    CHECK(wada_records_open(&r, &bad_ops, ":memory:", "item/", &err) == WADA_ERR_ABI);
    return 0;
}

static int test_crud_and_iteration(void) {
    const char *db = "test_records.db";
    clean_db(db);
    wada_err err;
    wada_err_clear(&err);
    const wada_store_ops *sqlite = wada_store_sqlite();

    wada_records items;
    wada_records others;
    CHECK(wada_records_open(&items, sqlite, db, "item/", &err) == WADA_OK);
    CHECK(wada_records_open(&others, sqlite, db, "other/", &err) == WADA_OK);

    char key[64];
    wada_records_key(&items, "abc", key, sizeof key);
    CHECK(strcmp(key, "item/abc") == 0);

    CHECK(!wada_records_exists(&items, "a1"));
    CHECK(wada_records_put(&items, "a1", "alpha", 5, &err) == WADA_OK);
    CHECK(wada_records_put(&items, "b2", "beta", 4, &err) == WADA_OK);
    CHECK(wada_records_put(&others, "o1", "outside", 7, &err) == WADA_OK);
    CHECK(wada_records_exists(&items, "a1"));
    CHECK(!wada_records_exists(&items, "o1"));

    char buf[32];
    size_t len = 0;
    CHECK(wada_records_get(&items, "a1", buf, sizeof buf, &len, &err) == WADA_OK);
    CHECK(len == 5 && strcmp(buf, "alpha") == 0);

    /* Truncated read still reports full stored length. */
    char tiny[4];
    CHECK(wada_records_get(&items, "a1", tiny, sizeof tiny, &len, &err) == WADA_OK);
    CHECK(len == 5 && strcmp(tiny, "alp") == 0);

    CHECK(wada_records_get(&items, "missing", buf, sizeof buf, &len, &err) == WADA_ERR_NOT_FOUND);
    CHECK(len == 0 && buf[0] == '\0');

    /* Prefix iteration strips prefix and excludes "other/" keys. */
    visit_bag bag;
    memset(&bag, 0, sizeof bag);
    CHECK(wada_records_each(&items, collect_visitor, &bag, &err) == WADA_OK);
    CHECK(bag.count == 2);
    CHECK(strcmp(bag.ids[0], "a1") == 0 && strcmp(bag.vals[0], "alpha") == 0);
    CHECK(strcmp(bag.ids[1], "b2") == 0 && strcmp(bag.vals[1], "beta") == 0);

    /* Early stop when visitor returns false. */
    memset(&bag, 0, sizeof bag);
    bag.stop_after = 1;
    CHECK(wada_records_each(&items, collect_visitor, &bag, &err) == WADA_OK);
    CHECK(bag.count == 1 && strcmp(bag.ids[0], "a1") == 0);

    CHECK(wada_records_del(&items, "a1", &err) == WADA_OK);
    CHECK(!wada_records_exists(&items, "a1"));
    CHECK(wada_records_exists(&others, "o1"));

    wada_records_close(&items);
    wada_records_close(&others);

    /* Fresh ids remain unique across close/reopen in the same second. */
    CHECK(wada_records_open(&items, sqlite, db, "item/", &err) == WADA_OK);
    char id1[48], id2[48];
    wada_records_fresh_id(&items, id1, sizeof id1);
    CHECK(id1[0] != '\0');
    CHECK(wada_records_put(&items, id1, "one", 3, &err) == WADA_OK);
    wada_records_close(&items);

    CHECK(wada_records_open(&items, sqlite, db, "item/", &err) == WADA_OK);
    wada_records_fresh_id(&items, id2, sizeof id2);
    CHECK(strcmp(id1, id2) != 0);
    CHECK(wada_records_put(&items, id2, "two", 3, &err) == WADA_OK);

    CHECK(wada_records_get(&items, id1, buf, sizeof buf, &len, &err) == WADA_OK);
    CHECK(strcmp(buf, "one") == 0);
    CHECK(wada_records_get(&items, id2, buf, sizeof buf, &len, &err) == WADA_OK);
    CHECK(strcmp(buf, "two") == 0);

    wada_records_close(&items);
    clean_db(db);
    return 0;
}

static int test_line_helpers(void) {
    char out[12];
    wada_records_sanitize_line("one\ntwo\r\nthree", out, sizeof out);
    CHECK(strcmp(out, "one two  th") == 0);

    wada_records_sanitize_line(NULL, out, sizeof out);
    CHECK(out[0] == '\0');

    const char *raw = "v1\nfirst\nsecond";
    const char *p = raw;
    const char *end = raw + strlen(raw);
    char line[8];

    CHECK(wada_records_next_line(&p, end, line, sizeof line));
    CHECK(strcmp(line, "v1") == 0);
    CHECK(wada_records_next_line(&p, end, line, sizeof line));
    CHECK(strcmp(line, "first") == 0);
    CHECK(wada_records_next_line(&p, end, line, sizeof line));
    CHECK(strcmp(line, "second") == 0);
    CHECK(!wada_records_next_line(&p, end, line, sizeof line));

    const char *empty = "";
    CHECK(!wada_records_next_line(&empty, empty, line, sizeof line));
    return 0;
}

static int test_search(void) {
    const char *db = "test_records_search.db";
    clean_db(db);
    wada_err err;
    wada_err_clear(&err);
    const wada_store_ops *sqlite = wada_store_sqlite();

    wada_records items;
    wada_records others;
    CHECK(wada_records_open(&items, sqlite, db, "item/", &err) == WADA_OK);
    CHECK(wada_records_open(&others, sqlite, db, "other/", &err) == WADA_OK);

    CHECK(wada_records_put(&items, "r1", "alpha kernel routing", 20, &err) == WADA_OK);
    CHECK(wada_records_put(&items, "r2", "beta storage index", 18, &err) == WADA_OK);
    CHECK(wada_records_put(&items, "r3", "gamma kernel snapshot", 21, &err) == WADA_OK);
    CHECK(wada_records_put(&others, "o1", "kernel outside prefix", 21, &err) == WADA_OK);

    visit_bag bag;
    memset(&bag, 0, sizeof bag);
    CHECK(wada_records_search(&items, "kernel", 0, collect_visitor, &bag, &err) == WADA_OK);
    CHECK(bag.count == 2);
    CHECK((strcmp(bag.ids[0], "r1") == 0 && strcmp(bag.ids[1], "r3") == 0) ||
          (strcmp(bag.ids[0], "r3") == 0 && strcmp(bag.ids[1], "r1") == 0));

    /* Prefix match on last token and early stop via stop_after. */
    memset(&bag, 0, sizeof bag);
    bag.stop_after = 1;
    CHECK(wada_records_search(&items, "kern", 0, collect_visitor, &bag, &err) == WADA_OK);
    CHECK(bag.count == 1);

    /* Query matching only "other/" returns 0 under "item/". */
    memset(&bag, 0, sizeof bag);
    CHECK(wada_records_search(&items, "outside", 0, collect_visitor, &bag, &err) == WADA_OK);
    CHECK(bag.count == 0);

    wada_records_close(&others);
    wada_records_close(&items);

    /* Provider without search slot returns WADA_ERR_UNSUPPORTED. */
    wada_store_ops no_search = *sqlite;
    no_search.search = NULL;
    CHECK(wada_records_open(&items, &no_search, ":memory:", "item/", &err) == WADA_OK);
    CHECK(wada_records_search(&items, "x", 0, collect_visitor, &bag, &err) == WADA_ERR_UNSUPPORTED);
    wada_records_close(&items);

    clean_db(db);
    return 0;
}

int main(void) {
    test_daily_backup(false);
    if (test_abi_and_args() || test_crud_and_iteration() || test_line_helpers() || test_search()) {
        fprintf(stderr, "records tests: FAILED after %d checks\n", g_checks);
        return 1;
    }
    printf("records tests: %d checks passed\n", g_checks);
    return 0;
}
