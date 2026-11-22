/* Wada — core tests */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wada.h"
#include "wada_iface.h"

static int g_checks = 0;

#define CHECK(expr)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(expr)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);             \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

/* ---- error ------------------------------------------------------------- */

static int test_error(void) {
    wada_err err;
    wada_err_clear(&err);
    CHECK(err.code == WADA_OK);
    CHECK(strcmp(wada_err_msg(&err), "") == 0);

    wada_status st = wada_err_set(&err, WADA_ERR_IO, "open %s failed", "store.db");
    CHECK(st == WADA_ERR_IO);
    CHECK(err.code == WADA_ERR_IO);
    CHECK(strcmp(wada_err_msg(&err), "open store.db failed") == 0);
    CHECK(strcmp(wada_status_name(st), "io error") == 0);
    return 0;
}

/* ---- scope ------------------------------------------------------------- */

static int g_order[8];
static int g_drops = 0;

static void drop_one(void *user) {
    g_order[g_drops++] = (int)(intptr_t)user;
}

static int test_scope(void) {
    wada_scope *root = wada_scope_create(NULL, "root");
    CHECK(root != NULL);
    CHECK(strcmp(wada_scope_name(root), "root") == 0);

    wada_token a = wada_scope_own(root, drop_one, (void *)(intptr_t)1);
    wada_token b = wada_scope_own(root, drop_one, (void *)(intptr_t)2);
    wada_token c = wada_scope_own(root, drop_one, (void *)(intptr_t)3);
    CHECK(a && b && c && a != b && b != c);
    CHECK(wada_scope_count(root) == 3);

    /* Release the middle one: it drops now, the rest after it. */
    CHECK(wada_scope_release(root, b));
    CHECK(g_drops == 1 && g_order[0] == 2);
    CHECK(wada_scope_count(root) == 2);
    CHECK(!wada_scope_release(root, b)); /* never twice */

    wada_scope *child = wada_scope_create(root, "child");
    CHECK(child != NULL);
    wada_scope_own(child, drop_one, (void *)(intptr_t)9);

    g_drops = 0;
    wada_scope_destroy(root);
    /* child first, then its own entries in reverse, then the parent's. */
    CHECK(g_drops == 3);
    CHECK(g_order[0] == 9);
    CHECK(g_order[1] == 3);
    CHECK(g_order[2] == 1);
    return 0;
}

/* ---- events ------------------------------------------------------------ */

static int g_calls[8];
static int g_ncalls = 0;

static void record(void *user, const char *topic, const void *data, size_t len) {
    (void)topic;
    (void)len;
    g_calls[g_ncalls++] = ((int)(intptr_t)user) * 10 + (data ? *(const int *)data : 0);
}

static void canceller(void *user, const char *topic, const void *data, size_t len) {
    (void)topic;
    (void)data;
    (void)len;
    wada_bus_cancel((wada_sub *)user);
}

static wada_sub *g_added_during = NULL;
static wada_bus *g_bus = NULL;

static void adder(void *user, const char *topic, const void *data, size_t len) {
    (void)user;
    (void)topic;
    (void)data;
    (void)len;
    g_added_during = wada_bus_subscribe(g_bus, "late2", 0, record, (void *)(intptr_t)7);
}

static int test_events(void) {
    wada_bus *bus = wada_bus_create();
    CHECK(bus != NULL);
    CHECK(wada_bus_count(bus) == 0);

    /* Priority order: higher first, registration order on ties. */
    wada_bus_subscribe(bus, "turn", 0, record, (void *)(intptr_t)1);
    wada_bus_subscribe(bus, "turn", 5, record, (void *)(intptr_t)2);
    wada_bus_subscribe(bus, "turn", 0, record, (void *)(intptr_t)3);
    CHECK(wada_bus_count(bus) == 3);

    int payload = 4;
    wada_bus_publish(bus, "turn", &payload, sizeof payload);
    CHECK(g_ncalls == 3);
    CHECK(g_calls[0] == 24);
    CHECK(g_calls[1] == 14);
    CHECK(g_calls[2] == 34);

    /* Cancel from inside dispatch: the cancelled listener is skipped. */
    g_ncalls = 0;
    wada_sub *victim = wada_bus_subscribe(bus, "stop", 0, record, (void *)(intptr_t)8);
    wada_bus_subscribe(bus, "stop", 10, canceller, victim);
    wada_bus_publish(bus, "stop", NULL, 0);
    CHECK(g_ncalls == 0);
    CHECK(wada_bus_count(bus) == 4); /* the victim left; the canceller stayed */

    /* A publish with no listeners is a no-op. */
    wada_bus_publish(bus, "nobody", NULL, 0);
    CHECK(g_ncalls == 0);

    /* A subscription made during dispatch is not called by that dispatch. */
    g_ncalls = 0;
    g_bus = bus;
    wada_sub *late = wada_bus_subscribe(bus, "late", 0, adder, NULL);
    CHECK(late != NULL);
    wada_bus_publish(bus, "late", NULL, 0);
    CHECK(g_added_during != NULL);
    CHECK(g_ncalls == 0);
    wada_bus_publish(bus, "late2", NULL, 0);
    CHECK(g_ncalls == 1 && g_calls[0] == 70);

    g_ncalls = 0;
    wada_bus_publish(bus, "turn", &payload, sizeof payload);
    CHECK(g_ncalls == 3);

    wada_bus_destroy(bus);
    return 0;
}

/* ---- registry ---------------------------------------------------------- */

static int g_ops_v1[1] = {1};
static int g_ops_v2[1] = {2};

static int test_registry(void) {
    wada_registry *reg = wada_registry_create();
    CHECK(reg != NULL);
    CHECK(wada_registry_count(reg) == 0);
    CHECK(wada_registry_get(reg, "store", 1, NULL) == NULL);

    wada_scope *root = wada_scope_create(NULL, "root");
    CHECK(wada_registry_add(reg, root, "store", 1, g_ops_v1) == WADA_OK);
    CHECK(wada_registry_add(reg, root, "store", 1, g_ops_v1) == WADA_ERR_EXISTS);

    uint32_t ver = 0;
    CHECK(wada_registry_get(reg, "store", 1, &ver) == g_ops_v1);
    CHECK(ver == 1);
    CHECK(wada_registry_get(reg, "store", 2, NULL) == NULL);

    CHECK(wada_registry_add(reg, NULL, "model", 2, g_ops_v2) == WADA_OK);
    CHECK(wada_registry_count(reg) == 2);
    CHECK(wada_registry_get(reg, "model", 1, &ver) == g_ops_v2 && ver == 2);

    const char *id = NULL;
    uint32_t v = 0;
    const void *ops = NULL;
    CHECK(wada_registry_at(reg, 0, &id, &v, &ops));
    CHECK(strcmp(id, "store") == 0 && v == 1 && ops == g_ops_v1);

    /* Destroying the owner scope removes the service. */
    wada_scope_destroy(root);
    CHECK(wada_registry_get(reg, "store", 1, NULL) == NULL);
    CHECK(wada_registry_count(reg) == 1);

    wada_registry_destroy(reg);
    return 0;
}

/* ---- interfaces -------------------------------------------------------- */

static int test_iface(void) {
    wada_ops_header hdr = {sizeof(wada_store_ops), WADA_STORE_ABI};
    CHECK(wada_ops_ok(&hdr, sizeof hdr, WADA_STORE_ABI));
    CHECK(wada_ops_ok(&hdr, WADA_STORE_OPS_SIZE, WADA_STORE_ABI));
    CHECK(!wada_ops_ok(&hdr, sizeof hdr, WADA_STORE_ABI + 1));

    wada_ops_header small = {4, WADA_STORE_ABI};
    CHECK(!wada_ops_ok(&small, WADA_STORE_OPS_SIZE, WADA_STORE_ABI));
    CHECK(!wada_ops_ok(NULL, WADA_STORE_OPS_SIZE, WADA_STORE_ABI));
    return 0;
}

/* ---- record ids -------------------------------------------------------- */

static int test_ids(void) {
    char a[64];
    char b[64];
    wada_id_make(a, sizeof a);
    wada_id_make(b, sizeof b);

    CHECK(a[0] != '\0' && strlen(a) < 48);
    CHECK(strcmp(a, b) != 0);

    /* "<seconds>-<serial>-<seed>" parses with no trailing garbage. */
    long long seconds = 0;
    unsigned long serial = 0;
    unsigned seed = 0;
    char tail[8];
    CHECK(sscanf(a, "%lld-%lu-%x%1s", &seconds, &serial, &seed, tail) == 3);
    CHECK(seconds > 0);
    CHECK(serial > 0);
    CHECK(seed != 0);

    /* A thousand ids in one process collide never. */
    char ids[64][64];
    for (int i = 0; i < 64; i++) wada_id_make(ids[i], sizeof ids[i]);
    bool unique = true;
    for (int i = 0; i < 64 && unique; i++) {
        for (int j = i + 1; j < 64; j++) {
            if (strcmp(ids[i], ids[j]) == 0) {
                unique = false;
                break;
            }
        }
    }
    CHECK(unique);
    return 0;
}

int main(void) {
    if (test_error() || test_scope() || test_events() || test_registry() || test_iface() ||
        test_ids()) {
        fprintf(stderr, "wada core tests: FAILED after %d checks\n", g_checks);
        return 1;
    }
    printf("wada core tests: %d checks passed\n", g_checks);
    return 0;
}
