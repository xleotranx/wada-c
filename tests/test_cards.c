/* Cards tests: scheduling and persistence of the learning product. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cards.h"
#include "test_env.h"
#include "wada.h"
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

static void clean_db(void) {
    remove("test_cards.db");
    remove("test_cards.db-wal");
    remove("test_cards.db-shm");
}

static int test_cards(void) {
    clean_db();
    wada_err err;
    wada_err_clear(&err);
    cards c;

    CHECK(cards_open(&c, wada_store_sqlite(), "test_cards.db", &err) == WADA_OK);
    CHECK(cards_count(&c) == 0);

    long long now = (long long)time(NULL);
    int first = cards_create(&c, "Question", "Answer", &err);
    CHECK(first == 0);
    CHECK(cards_count(&c) == 1);
    CHECK(cards_at(&c, 0)->due <= now + 2);
    CHECK(cards_due_count(&c, now) == 1);

    /* Good: first successful review schedules a day out. */
    CHECK(cards_review(&c, 0, 2, &err) == WADA_OK);
    const card *one = cards_at(&c, 0);
    CHECK(one->reps == 1);
    CHECK(one->interval_min == 1440);
    CHECK(one->due >= now + 1400 * 60);
    CHECK(cards_due_count(&c, now) == 0);

    /* Easy: second successful review moves to six days. */
    CHECK(cards_review(&c, 0, 3, &err) == WADA_OK);
    CHECK(cards_at(&c, 0)->interval_min == 6 * 1440);
    CHECK(cards_at(&c, 0)->ease > 2.5);

    /* Again: due in ten minutes, reps reset, ease floors at 1.3. */
    CHECK(cards_review(&c, 0, 0, &err) == WADA_OK);
    CHECK(cards_at(&c, 0)->reps == 0);
    CHECK(cards_at(&c, 0)->interval_min == 10);
    CHECK(cards_at(&c, 0)->due <= (long long)time(NULL) + 10 * 60 + 2);

    /* Due ordering: a new card is due now, so it comes first. */
    int second = cards_create(&c, "Second", "Answer 2", &err);
    CHECK(second == 1);
    int due[8];
    int n = cards_due(&c, (long long)time(NULL), due, 8);
    CHECK(n == 1);
    CHECK(due[0] == 1);

    CHECK(cards_next_due(&c) > 0);
    CHECK(cards_find(&c, cards_at(&c, 0)->id) == 0);

    cards_close(&c);

    /* Reopen: scheduling state survives. */
    CHECK(cards_open(&c, wada_store_sqlite(), "test_cards.db", &err) == WADA_OK);
    CHECK(cards_count(&c) == 2);
    CHECK(strcmp(cards_at(&c, 0)->front, "Question") == 0);
    CHECK(strcmp(cards_at(&c, 0)->back, "Answer") == 0);
    CHECK(cards_at(&c, 0)->reps == 0);
    CHECK(cards_at(&c, 0)->interval_min == 10);

    /* A reloaded card must be written back under its own key. Reviewing it and
     * reopening is the regression test for an id that lost its "card/" prefix
     * on load: the write used to land outside the prefix, so the review was
     * silently lost. */
    CHECK(cards_review(&c, 0, 2, &err) == WADA_OK);
    int interval = cards_at(&c, 0)->interval_min;
    CHECK(interval == 1440);
    cards_close(&c);
    CHECK(cards_open(&c, wada_store_sqlite(), "test_cards.db", &err) == WADA_OK);
    CHECK(cards_at(&c, 0)->interval_min == interval);
    CHECK(cards_at(&c, 0)->reps == 1);
    cards_close(&c);

    clean_db();
    return 0;
}

int main(void) {
    test_daily_backup(false); /* this test is not about backups */
    if (test_cards()) {
        fprintf(stderr, "cards tests: FAILED after %d checks\n", g_checks);
        return 1;
    }
    printf("cards tests: %d checks passed\n", g_checks);
    return 0;
}
